## Proposal from lead
Run: squad-049a3597-3c22-437f-a050-3612f0ecdcc1

PROPOSAL: Keep feedback best-effort and fail-silent when audio is stopped/unavailable, the nonblocking mutex cannot be acquired, or the bounded pending/playback queues are full; UI actions must never wait or retry indefinitely.
