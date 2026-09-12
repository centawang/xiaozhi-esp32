## Proposal from frontend
Run: squad-b86a048b-b533-4217-a2c6-cd9c735b9909

Proposal: keep the 16-slot retired ring fail-closed on overflow (abort current stroke) rather than widening the ring or treating unknown old IDs as Drop, and require host longevity tests to EnsureIdle before any following generation-0 bind.
