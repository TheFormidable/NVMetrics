Reduce manual fan-lowering latency while preserving safety checks

Lowering from 100% to 30% in 0.14.6 took roughly eight seconds per five-point step. Every telemetry probe reopened the driver and repeated a 1.2-second warm-up. This change keeps a private read-only RUSD reader for the manual session and correlates each request with a new response. All four critical sensor timestamps must advance before another sample can be returned. The reader closes on session termination or any transport/freshness fault; there is no fallback to cached data.

The five-point steps, three tachometer samples, existing stabilization delays, temperature/power limits and independent guard/rescue remain intact. Refused reductions retain the selected target and report the measured blocking value. Eco and Reposo remain independent.

Validation: 101 Python tests, 10 C suites, two Swift suites and existing shell tests; native Intel app/runtime/package build and extraction/signature verification. Includes reader reuse, stale/duplicate replies, mismatched request IDs, timeouts, process exit, cleanup, UI refusal details and migration from 0.14.6. Real-device read-only performance measurements are recorded in AUDITORIA-v0147.md.

Physical transition timing of this version still needs supervised validation after installation. No installation or hardware-control test of 0.14.7 was performed. The existing UID authorization and local ad-hoc signing limitations remain; this is not a claim of a production-qualified public release.
