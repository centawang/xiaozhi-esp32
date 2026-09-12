## Proposal from tester
Run: squad-686e0c79-1e1e-4b57-9be0-e7f82e0582b5

PROPOSAL: 扩展 production-shape harness，加入 channel close、network disconnect/error、ResetProtocol/replace round，并显式模拟 STATE_CHANGED 先于 scheduled abort 的顺序。
