## Proposal from tester
Run: squad-4a2acf71-3fd2-4713-a004-53bff331c30d

PROPOSAL: 把 blocking-open harness 改成生产形状：during-open 只置 pending abort/event，由 continuation 与最终 start gate消费；不要直接同步调用 coordinator `AbortRound()`。
