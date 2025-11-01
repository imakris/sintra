# Processing Fence Test Trace (WIP)

Environment: `SINTRA_TRACE_BARRIER=1`

```
[barrier_deferral] barrier=processing-fence fiid=144115188075859980 needs_ack=1 awaiting_outbound=0 awaiting_processing=1
[ack_response_recv] barrier=processing-fence type=2 success=1 responder=288230376151711745 observed=2072
[ack_response_recv] barrier=processing-fence type=2 success=1 responder=216172782113783809 observed=2072
[wait_for_delivery_fence] target_added stream=req pid=288230376151711745 target=1271
[wait_for_delivery_fence] pending stream=req observed=955 target=1327
[wait_for_delivery_fence] wait complete (external thread)
[wait_for_delivery_fence] final stream=req observed=1439 target=1327
[worker] handler_ran=false
```

Interpretation: acknowledgements arrive once the RPC request stream advances, but the broadcast `Work_message` handler (`handler_ran`) never executes before the barrier finalises.
