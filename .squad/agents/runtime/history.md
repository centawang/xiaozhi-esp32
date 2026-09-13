# Runtime: project knowledge

单片机设备开发

<!-- pi-squad:d89a000f483e01f72cc01bad248d4feeeffa5c2dfd305960b98002751bbe3776 -->
## 2026-09-13T05:00:31.859Z — stroke-interaction-improvements-visual-finalize

Recovery successful. Required report written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/9f3b0a92-175c-4ba1-9706-36ede85a3f13/stroke-interaction-improvements-visual-finalize.md, referencing the complete persisted report at /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/51873f6e-8a8f-46d3-acc5-955f0aa82271/stroke-interaction-improvements-best-effort.md. Final artifact is visual-only/no sound, 12 files, with no Application/audio/other-board/CMake/default-assets logical diff. Persisted validation records shared 132/132 and isolated 132/132 host tests PASS (no skips), and feature-on/off CoreS3 clean builds PASS. Read-only recovery inspection confirmed all 12 source files unchanged against the validation manifest and no active idf.py/ninja process. Recomputed SHA-256 values match: patch 23fd6980cdfcff85910ab7ce590bc88f311d647bbb500551dfbc9b9a0352198a; feature-on merged 6602c2ea033927dc75e1d0c4009f9ae76253e3d7459372b05a437cee1d16cc53; stroke_cat 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. No source edits, builds/tests, deletions, staging, commits, pushes, or flashing performed during recovery. Independent review and hardware validation remain required.

- The historical best-effort report filename contains the final visual-only implementation and validation report; it does not indicate retained audio feedback.
- All 12 artifact source files currently match the frozen validation manifest; the patch, feature-on merged image, and catalog hashes were independently recomputed during recovery.

<!-- pi-squad:e209a8798d02bbc170e517c82971544ee358af70c2c22d82b44491c2fcede399 -->
## 2026-09-13T13:01:02.494Z — stroke-step-phase-local-finalize

Recovery successful. Required report written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/733858a0-ac1b-4881-a416-2807ec8525fe/stroke-step-phase-local-finalize.md, referencing the complete persisted report at /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/829ecba6-c040-4b85-a570-bb660600e10c/stroke-step-phase-local-fix.md. Confirmed phase-local cue/gap StepForward behavior; persisted exact red-before/green-after regressions; focused22/22 and shared/isolated137/137 PASS, no skips; pinned-lock CoreS3 on/off clean builds PASS; exact12 visual-only paths and no protected logical diff. Recomputed all12 source hashes: unchanged. Recomputed patch SHA 07e267b0033890fa04027e358dede492f9ab1e96d211552542390e7b74bab48c and feature-on merged SHA 8997d0e6ec9d98e36c4862a8e4f4aabd6c8b307742b0fabac4a6a4cb1b9e48c9: both match. Process inspection found no active build process. No source edits, builds/tests, deletions, staging, commits, pushes or flashing performed during recovery. Independent review and hardware validation remain required.

- StepForward now crosses exactly one presentation phase: cue to same-stroke full/gap (or final completion), and gap to next cue at zero progress without completing it.
- All 12 artifact source files matched the frozen validation manifest during recovery; patch and feature-on merged image hashes were independently recomputed and matched.
- Final build candidates use the pinned unchanged dependency lock; earlier dependency-drift outputs are explicitly excluded from delivery.
