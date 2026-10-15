# Fork optimization integration

The local optimization branch and its uncommitted changes were integrated after upstream
`0afa6a607` on 2026-10-15. The original work remains on
`backup/main-before-sync-20261015` for comparison.

The integration retains upstream's newer shader interfaces, cache formats, unwind cache,
compute pipeline preparation, watched private imports, descriptor pool recycling, and native
multisample depth support. Older local ports of those features must not overwrite them.

## Runtime switches

| Feature | Default | Control |
| --- | --- | --- |
| Queue-0 draw commit worker | Off | `APS5_PIPELINED_DRAWS=32` enables a bounded FIFO |
| DMA packets on the same worker | On when draw pipelining is active | `APS5_NO_PIPELINE_DMA=1` disables it |
| Adaptive draw-cache bypass | On | `APS5_NO_DRAW_CACHE_BYPASS=1` disables it |
| Draw validation stamps | On | `APS5_NO_DRAW_STAMP_VALIDATE=1` uses full validation |
| Registered readable-range memo | On for coverage queries without piece output | `APS5_NO_REGISTERED_COVER_MEMO=1` disables it |
| Open render-pass target proofs | On | `APS5_NO_TARGET_PROOF=1` forces refreshes |
| GPU-only allocation imports | Off | `APS5_IMPORT_GPU_ONLY=1` includes GPU-accessible registrations |
| Cross-draw input snapshot reuse | Off | `APS5_REUSE_DRAW_SNAPSHOTS=1` is an experimental opt-in |
| PlayGo without chunk metadata | Strict validation | `APS5_PLAYGO_ASSUME_INSTALLED=1` treats requested chunks as locally installed when the dump has no chunk table or scenario; an existing table still validates IDs |

Shader-memory capture uses pooled pages and compact word masks. Draw variants record the
user-data registers they actually consume, allowing shape and stage reuse. Target proofs
also check CPU writes and image/shadow invalidations. Descriptor pools retain upstream's
completion-serial checks, including its fix for allocations outside an active recorder.

The shader archive tool is retained as an optional build target. These changes require
rebuilding both the relinker and `libs`, then refreshing a game's runtime libraries. Older
converted executables may require relinking when upstream changes process metadata.

## Validation

`agc_optimization_PipelinedDma` checks that a pipelined copy observes its preceding fill and
that neither packet drains the worker. `agc_optimization_DrawWriteRanges` checks the write
ranges used to order asynchronous draws. Both are registered with CTest and have timeouts.
Run the existing cache, shader-memory, import-watch, target, and descriptor-pool tests as
well. Passing these checks does not establish gameplay correctness or an FPS improvement.

The full Release build, the optional archive tool/tests, and 26 selected regressions passed
with the launcher's Profile settings (`APS5_PIPELINED_DRAWS=32`, GPU-only imports enabled,
adaptive draw-cache bypass disabled). This was a targeted regression run; it does not replace
the earlier full-suite report, which contained unrelated failures and timeouts.
