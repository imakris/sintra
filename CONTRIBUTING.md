# Contributing to Sintra

## Evolving capabilities

Sintra's public API is a product contract. A new capability should begin with
the user-visible behavior and ownership model it enables, including how
delivery, failure, teardown, and authorization are meant to work.

Before extending that contract, trace the need through the existing public
API, transport and dispatch internals, documentation, examples, and tests.
Establish why the behavior cannot be expressed by composing the mechanisms
Sintra already provides. Prefer composition at the consumer boundary when
existing primitives can preserve the required semantics.

When the transport already owns request provenance, consume that exact
request metadata at the product boundary. Do not copy it into a second generic
context API or accept an equivalent caller-supplied identity.

Keep one authoritative semantic path for each behavior. A convenience API
should not create a second addressing, delivery, lifecycle, or error model
beside an established mechanism. If an experiment shows that an existing
primitive is sufficient, adapt the consumer and remove the experimental path
rather than retaining parallel concepts.

Tests and documentation should describe the product contract, not just the
implementation. Cover the intended ownership and lifecycle boundaries,
including isolation from the wrong sender or receiver and behavior during
replacement, departure, or teardown where those cases apply.
