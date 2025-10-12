# Variable Buffer Alignment Notes

Sintra's variable buffer payloads now honor the natural alignment of the stored
value type. Writers pad each payload so that the placement-new construction used
inside `variable_buffer` always operates on properly aligned storage. Readers
validate the alignment when reconstructing containers, which allows address
sanitizers configured with `-fsanitize=alignment` to flag regressions.

Because padding is now part of the serialized layout, both the writing and
reading sides must run with a version that understands the aligned format. Mixed
versions (old reader/new writer or vice versa) can disagree on the payload span
and will eventually desynchronize the ring buffer. Plan migrations accordingly
(e.g., with a coordinated rollout or protocol negotiation).
