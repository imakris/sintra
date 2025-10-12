[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$Executable = ".\build-ninja\tests\sintra_recovery_test.exe",

    [Parameter(Mandatory = $false)]
    [string[]]$Arguments = @(),

    [Parameter(Mandatory = $false)]
    [int]$TimeoutSeconds = 10,

    [Parameter(Mandatory = $false)]
    [int]$StartupTimeoutSeconds = 5,

    [Parameter(Mandatory = $false)]
    [int]$ExpectedReadyCount = 0,

    [Parameter(Mandatory = $false)]
    [string]$ReadyFilePattern = "ready_*",

    [Parameter(Mandatory = $false)]
    [string]$WorkingDirectory = "",

    [Parameter(Mandatory = $false)]
    [int]$MaxCleanupIterations = 10,

    [Parameter(Mandatory = $false)]
    [int]$CleanupDelayMilliseconds = 200,

    [Parameter(Mandatory = $false)]
    [int]$TotalTimeoutSeconds = 15,

    [switch]$KeepSharedDir
)

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Not running with administrative privileges; forced termination may fail for protected processes."
}

function Get-RemainingMilliseconds {
    param([datetime]$Deadline)

    $remaining = [int][Math]::Floor(($Deadline - (Get-Date)).TotalMilliseconds)
    if ($remaining -lt 0) {
        return 0
    }
    return $remaining
}

function New-SharedDirectory {
    $base = Join-Path ([System.IO.Path]::GetTempPath()) "sintra_tests"
    if (-not (Test-Path $base)) {
        New-Item -Path $base -ItemType Directory -Force | Out-Null
    }

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmssfff"
    $unique = "{0}_{1}" -f $timestamp, $PID
    $dir = Join-Path $base "run_$unique"
    New-Item -Path $dir -ItemType Directory -Force | Out-Null
    return $dir
}

function Remove-SharedDirectory($Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    if (-not (Test-Path $Path)) {
        return
    }

    try {
        Remove-Item -Path $Path -Recurse -Force -ErrorAction Stop
    }
    catch {
        Write-Warning ("Failed to remove shared directory '{0}': {1}" -f $Path, $_)
    }
}

function Get-ProcessIdsByPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return @()
    }

    $list = @()
    $procs = Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and ($_.ExecutablePath -ieq $Path)
    }

    foreach ($proc in $procs) {
        $list += [int]$proc.ProcessId
    }

    return $list
}

function Kill-ProcessTree {
    param(
        [uint32]$ProcessId,
        [int]$WaitMilliseconds = 200,
        [datetime]$Deadline
    )

    $target = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $target) {
        return $true
    }

    try {
        Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
    }
    catch {
        Write-Warning ("Stop-Process initial attempt failed for PID {0}: {1}" -f $ProcessId, $_)
    }

    if ($WaitMilliseconds -gt 0) {
        $sleep1 = if ($Deadline) { [Math]::Min($WaitMilliseconds, (Get-RemainingMilliseconds -Deadline $Deadline)) } else { $WaitMilliseconds }
        if ($sleep1 -gt 0) {
            Start-Sleep -Milliseconds $sleep1
        }
    }

    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
        return $true
    }

    try {
        $taskkillArgs = "/PID $ProcessId /F /T"
        Start-Process -FilePath "$env:SystemRoot\System32\taskkill.exe" `
            -ArgumentList $taskkillArgs `
            -NoNewWindow `
            -Wait `
            -ErrorAction Stop | Out-Null
    }
    catch {
        Write-Warning ("taskkill failed for PID {0}: {1}" -f $ProcessId, $_)
    }

    if ($WaitMilliseconds -gt 0) {
        $sleep2 = if ($Deadline) { [Math]::Min($WaitMilliseconds, (Get-RemainingMilliseconds -Deadline $Deadline)) } else { $WaitMilliseconds }
        if ($sleep2 -gt 0) {
            Start-Sleep -Milliseconds $sleep2
        }
    }

    if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
        return $true
    }

    try {
        $procObj = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$ProcessId" -ErrorAction Stop
        $result = Invoke-CimMethod -InputObject $procObj -MethodName Terminate -ErrorAction Stop
        if ($result.ReturnValue -ne 0) {
            Write-Warning ("WMI Terminate returned {0} for PID {1}" -f $result.ReturnValue, $ProcessId)
        }
        else {
            $sleep3 = if ($Deadline) { [Math]::Min($WaitMilliseconds, (Get-RemainingMilliseconds -Deadline $Deadline)) } else { $WaitMilliseconds }
            if ($sleep3 -gt 0) {
                Start-Sleep -Milliseconds $sleep3
            }
        }
    }
    catch {
        Write-Warning ("WMI terminate failed for PID {0}: {1}" -f $ProcessId, $_)
    }

    return -not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

$script:streamsClosed = $false
function Close-ProcessStreams {
    param([System.Diagnostics.Process]$Target)

    if ($script:streamsClosed) {
        return
    }

    try { $Target.CancelOutputRead() } catch { }
    try { $Target.CancelErrorRead() } catch { }
    $script:streamsClosed = $true
}

function ForceTerminateProcess {
    param(
        [System.Diagnostics.Process]$Target,
        [string]$Reason,
        [int]$WaitAfterKillMs = 300,
        [datetime]$Deadline
    )

    if ($null -eq $Target) {
        return $true
    }

    Close-ProcessStreams -Target $Target

    try {
        if ($Target.CloseMainWindow()) {
            $waitClose = if ($Deadline) { [Math]::Min(500, (Get-RemainingMilliseconds -Deadline $Deadline)) } else { 500 }
            if ($waitClose -gt 0 -and $Target.WaitForExit($waitClose)) {
                return $true
            }
        }
    }
    catch {
        Write-Warning ("CloseMainWindow failed for PID {0}: {1}" -f $Target.Id, $_)
    }

    $terminated = $false
    try {
        $terminated = Kill-ProcessTree -ProcessId ([uint32]$Target.Id) -WaitMilliseconds $WaitAfterKillMs -Deadline $Deadline
    }
    catch {
        Write-Warning ("Kill-ProcessTree failed for PID {0}: {1}" -f $Target.Id, $_)
    }

    if ($terminated) {
        return $true
    }

    $waitFinal = if ($Deadline) { [Math]::Min(2000, (Get-RemainingMilliseconds -Deadline $Deadline)) } else { 2000 }
    if ($waitFinal -gt 0 -and $Target.WaitForExit($waitFinal)) {
        return $true
    }

    Write-Warning ("Unable to terminate PID {0} after {1}; process may still be running." -f $Target.Id, $Reason)
    return $false
}

if (-not (Test-Path $Executable)) {
    throw "Executable '$Executable' was not found. Provide a valid path."
}

$resolvedExecutable = (Resolve-Path $Executable).ProviderPath
if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
    $WorkingDirectory = Split-Path -Path $resolvedExecutable -Parent
}

if (-not (Test-Path $WorkingDirectory)) {
    throw "Working directory '$WorkingDirectory' does not exist."
}

$effectiveStartupTimeout = [Math]::Max([Math]::Min($StartupTimeoutSeconds, 60), 1)
$effectiveTimeout = [Math]::Max([Math]::Min($TimeoutSeconds, 60), 1)
if ($TimeoutSeconds -gt 60) {
    Write-Warning "Requested timeout $TimeoutSeconds exceeds 60 seconds. Clamping to 60 seconds."
}
if ($StartupTimeoutSeconds -gt 60) {
    Write-Warning "Requested startup timeout $StartupTimeoutSeconds exceeds 60 seconds. Clamping to 60 seconds."
}

$effectiveTotalTimeout = [Math]::Max($TotalTimeoutSeconds, 1)
if ($TotalTimeoutSeconds -gt 120) {
    
}

$scriptStart = Get-Date
$globalDeadline = $scriptStart.AddSeconds($effectiveTotalTimeout)

if ($effectiveTimeout -gt $effectiveTotalTimeout) {
    $effectiveTimeout = $effectiveTotalTimeout
}
if ($effectiveStartupTimeout -gt $effectiveTotalTimeout) {
    $effectiveStartupTimeout = $effectiveTotalTimeout
}

$existingSharedDir = [System.Environment]::GetEnvironmentVariable("SINTRA_TEST_SHARED_DIR", "Process")
$ownsSharedDir = $false
if ([string]::IsNullOrWhiteSpace($existingSharedDir)) {
    $sharedDir = New-SharedDirectory
    [System.Environment]::SetEnvironmentVariable("SINTRA_TEST_SHARED_DIR", $sharedDir, "Process")
    Write-Host "Set SINTRA_TEST_SHARED_DIR to '$sharedDir'"
    $ownsSharedDir = $true
}
else {
    $sharedDir = $existingSharedDir
    Write-Host "Using existing SINTRA_TEST_SHARED_DIR '$sharedDir'"
}

$baselineReadyNames = New-Object 'System.Collections.Generic.HashSet[string]'
if (Test-Path $sharedDir) {
    $initialReady = Get-ChildItem -Path $sharedDir -Filter $ReadyFilePattern -ErrorAction SilentlyContinue
    foreach ($entry in $initialReady) {
        $null = $baselineReadyNames.Add($entry.Name)
    }
}
$readySeenNames = New-Object 'System.Collections.Generic.HashSet[string]'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $resolvedExecutable
$psi.Arguments = ($Arguments -join " ")
$psi.WorkingDirectory = $WorkingDirectory
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi

$standardOutput = New-Object System.Text.StringBuilder
$standardError = New-Object System.Text.StringBuilder

$outputHandler = [System.Diagnostics.DataReceivedEventHandler]{
    param($sender, $args)
    if ($args.Data) {
        $standardOutput.AppendLine($args.Data) | Out-Null
        Write-Host $args.Data
    }
}

$errorHandler = [System.Diagnostics.DataReceivedEventHandler]{
    param($sender, $args)
    if ($args.Data) {
        $standardError.AppendLine($args.Data) | Out-Null
        Write-Error $args.Data
    }
}

if (-not $process.Start()) {
    throw "Failed to start process '$resolvedExecutable'"
}

$startTime = $scriptStart
$processDeadline = $startTime.AddSeconds($effectiveTimeout)
$deadline = if ($processDeadline -lt $globalDeadline) { $processDeadline } else { $globalDeadline }

Write-Host ("Started '{0}' (PID {1}) with timeout {2}s (startup {3}s)" -f $resolvedExecutable, $process.Id, $effectiveTimeout, $effectiveStartupTimeout)
$process.BeginOutputReadLine()
$process.BeginErrorReadLine()

$startupFailed = $false
$observedReadyCount = 0
if ($ExpectedReadyCount -gt 0) {
    $startupDeadline = $startTime.AddSeconds($effectiveStartupTimeout)
    if ($startupDeadline -gt $deadline) {
        $startupDeadline = $deadline
    }

    while ($true) {
        $remainingStartup = Get-RemainingMilliseconds -Deadline $startupDeadline
        $remainingGlobal = Get-RemainingMilliseconds -Deadline $globalDeadline
        if ($remainingStartup -le 0 -or $remainingGlobal -le 0) {
            break
        }

        if (Test-Path $sharedDir) {
            $readyFiles = Get-ChildItem -Path $sharedDir -Filter $ReadyFilePattern -ErrorAction SilentlyContinue
            foreach ($entry in $readyFiles) {
                if ($baselineReadyNames.Contains($entry.Name)) {
                    continue
                }
                $readySeenNames.Add($entry.Name) | Out-Null
            }
            $observedReadyCount = $readySeenNames.Count
            if ($observedReadyCount -ge $ExpectedReadyCount) {
                break
            }
        }

        $sleepMs = [Math]::Min(200, [Math]::Max(1, [Math]::Min($remainingStartup, $remainingGlobal)))
        Start-Sleep -Milliseconds $sleepMs
    }

    if ($observedReadyCount -lt $ExpectedReadyCount) {
        $startupFailed = $true
        Write-Warning ("Startup readiness threshold not met (expected {0}, observed {1})" -f $ExpectedReadyCount, $observedReadyCount)
    }
    else {
        Write-Host ("Observed {0} ready markers matching '{1}'" -f $observedReadyCount, $ReadyFilePattern)
    }
}

$timedOut = $false
$exitCode = $null
$terminationFailed = $false

if ($startupFailed) {
    Write-Warning "Terminating process PID $($process.Id) due to startup readiness failure"
    if (-not $process.HasExited) {
        if (-not (ForceTerminateProcess -Target $process -Reason "startup readiness failure" -WaitAfterKillMs $CleanupDelayMilliseconds -Deadline $globalDeadline)) {
            $terminationFailed = $true
        }
    }
}
else {
    $remainingMs = Get-RemainingMilliseconds -Deadline $deadline
    if ($remainingMs -le 0) {
        $timedOut = $true
    }
    else {
        $timedOut = -not $process.WaitForExit($remainingMs)
    }

    if ($timedOut) {
        Write-Warning "Timeout reached - terminating process PID $($process.Id)"
        if (-not $process.HasExited) {
            if (-not (ForceTerminateProcess -Target $process -Reason "timeout exceeded" -WaitAfterKillMs $CleanupDelayMilliseconds -Deadline $globalDeadline)) {
                $terminationFailed = $true
            }
        }
    }
    else {
        $process.WaitForExit()
        $exitCode = $process.ExitCode
        Write-Host "Process exited with code $exitCode"
        Close-ProcessStreams -Target $process
    }
}

# Ensure process has exited before cleaning up streams
if (-not $process.HasExited) {
    $waitEnsure = Get-RemainingMilliseconds -Deadline $globalDeadline
    if ($waitEnsure -le 0 -or -not $process.WaitForExit($waitEnsure)) {
        Write-Warning ("Process PID {0} still running after termination attempts." -f $process.Id)
    }
}
Close-ProcessStreams -Target $process

for ($attempt = 0; $attempt -lt $MaxCleanupIterations; $attempt++) {
    if (Get-RemainingMilliseconds -Deadline $globalDeadline -le 0) {
        Write-Warning "Global timeout reached during cleanup loop."
        $timedOut = $true
        break
    }

    $remaining = Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ExecutablePath -and ($_.ExecutablePath -ieq $resolvedExecutable)
    }

    $targets = @()
    foreach ($proc in $remaining) {
        $targets += $proc
    }

    if ($targets.Count -eq 0) {
        break
    }

    foreach ($proc in $targets) {
        Write-Warning "Stopping lingering process at '$($proc.ExecutablePath)' (PID $($proc.ProcessId))"
        $stopped = $false
        try {
            $stopped = Kill-ProcessTree -ProcessId ([uint32]$proc.ProcessId) -WaitMilliseconds $CleanupDelayMilliseconds -Deadline $globalDeadline
        }
        catch {
            Write-Warning ("Unable to stop process PID {0}: {1}" -f $proc.ProcessId, $_)
        }

        if (-not $stopped) {
            Write-Warning "Process PID $($proc.ProcessId) still appears to be running after forced termination attempts."
        }
    }

    if ($CleanupDelayMilliseconds -gt 0 -and $attempt -lt ($MaxCleanupIterations - 1)) {
        $sleepCleanup = [Math]::Min($CleanupDelayMilliseconds, (Get-RemainingMilliseconds -Deadline $globalDeadline))
        if ($sleepCleanup -gt 0) {
            Start-Sleep -Milliseconds $sleepCleanup
        }
    }
}

$stillRunning = Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ExecutablePath -and ($_.ExecutablePath -ieq $resolvedExecutable)
}
if ($stillRunning) {
    Write-Warning "Some processes for '$resolvedExecutable' are still running after cleanup:"
    foreach ($proc in $stillRunning) {
        Write-Warning ("  PID {0}, CommandLine={1}" -f $proc.ProcessId, $proc.CommandLine)
    }
    $timedOut = $true
}

if ($ownsSharedDir -and -not $KeepSharedDir) {
    Remove-SharedDirectory -Path $sharedDir
    [System.Environment]::SetEnvironmentVariable("SINTRA_TEST_SHARED_DIR", $null, "Process")
}
elseif ($KeepSharedDir) {
    Write-Host "Keeping shared directory at '$sharedDir'"
}

$process.Dispose()

if ($startupFailed) {
    exit 125
}
elseif ($timedOut) {
    exit 124
}

if ($exitCode -ne $null) {
    exit $exitCode
}

exit 1


