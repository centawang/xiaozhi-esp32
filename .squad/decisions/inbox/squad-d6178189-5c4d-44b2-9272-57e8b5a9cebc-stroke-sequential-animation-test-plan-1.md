## Proposal from tester
Run: squad-d6178189-5c4d-44b2-9272-57e8b5a9cebc

PROPOSAL: Limit each admitted elapsed settlement to `min(real_delta, kTickMs)` (currently 33 ms), rebase the clock to `now`, retain only sub-millisecond remainder, and discard older whole-millisecond backlog.
