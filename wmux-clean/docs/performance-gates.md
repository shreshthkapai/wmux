# Performance Gates

Step 16 changes the benchmark suite from informational output into a release
gate. Run it only from an optimized build with the complete workload set:

```powershell
cargo run -p wmux-bench --release -- --suite full --gate
```

The command exits nonzero if a required scenario is absent or any gate fails.

| Requirement | Enforced evidence |
| --- | --- |
| No detached-output backlog | `detach-backlog` must finish with zero queued chunks |
| Input-to-render below one frame | `idle-input-render` p95 must remain below 16.67 ms |
| Resize independent of history | 100,000-line resize p95 must remain below 5 ms |
| No blank layout frame | `split-storm` must emit no ED2 or ED3 clear sequence |
| Damage-proportional rendering | one-cell output must be less than one quarter of a full scene |
| Bounded noisy/slow-client memory | detach and multi-client peak live memory must remain below 256 MiB |

Parser throughput for both Codex and Claude fixtures must remain above 40 MB/s.
The thresholds are intentionally conservative enough for shared CI runners but
strict enough to catch architectural regressions. Machine-specific benchmark
baselines remain useful for optimization work and are recorded separately in
`performance.md`.

The performance command is not the only gate. Server tests enforce fixed pane
and client queue capacities, round-robin pane progress, control-event progress
under output pressure, and coalescing for slow clients. The conformance workflow
runs those tests before the release benchmark gate.
