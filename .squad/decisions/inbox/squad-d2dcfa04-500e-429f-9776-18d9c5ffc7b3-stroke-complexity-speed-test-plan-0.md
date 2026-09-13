## Proposal from tester
Run: squad-d2dcfa04-500e-429f-9776-18d9c5ffc7b3

PROPOSAL: Adopt a 6000 ms whole-character credited target including gaps, with per-stroke duration `clamp(ceil((T-(N-1)G)/N), 301, 1000)` and saturate to 301 ms when gaps consume the target.
