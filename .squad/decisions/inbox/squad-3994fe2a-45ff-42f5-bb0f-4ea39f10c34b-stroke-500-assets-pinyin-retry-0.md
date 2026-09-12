## Proposal from backend
Run: squad-3994fe2a-45ff-42f5-bb0f-4ea39f10c34b

Keep the 1 MiB SOB1 and 64 KiB SPY1 format caps unchanged; pin the current 1,046,788-byte SOB1 in tests so any generation that would exceed 1 MiB fails closed.
