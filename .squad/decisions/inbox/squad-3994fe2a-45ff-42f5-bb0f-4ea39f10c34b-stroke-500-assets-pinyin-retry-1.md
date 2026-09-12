## Proposal from backend
Run: squad-3994fe2a-45ff-42f5-bb0f-4ea39f10c34b

Require PSRAM-capable owned copies for both SOB1 and SPY1 on device. Do not silently fall back to internal DRAM if SPIRAM malloc fails; hide the entry or degrade to exact-only instead.
