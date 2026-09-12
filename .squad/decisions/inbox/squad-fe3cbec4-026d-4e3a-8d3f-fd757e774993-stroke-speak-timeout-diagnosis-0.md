## Proposal from backend
Run: squad-fe3cbec4-026d-4e3a-8d3f-fd757e774993

PROPOSAL: Treat H1 (manual listen never stopped, so public STT never finalizes) as the first code change to falsify: use the same AutoStop/VAD STT path as ordinary chat, intercept type=stt, then FinishStrokeListening. Keep ManualStop only if a captured server actually emits STT without stop.
