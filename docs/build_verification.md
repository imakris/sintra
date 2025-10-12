# Build verification

The Release configuration was built locally to confirm that the project compiles after the
header adjustments.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Both commands completed successfully, producing the example programs and test binaries under
`build/`.
