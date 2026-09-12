## Proposal from tester
Run: squad-686e0c79-1e1e-4b57-9be0-e7f82e0582b5

PROPOSAL: 在所有会取消或替换 stroke round 的异步 source 上，于 event/schedule 前发布对应 generation fence；优先修复 OnAudioChannelClosed，并对 open 完成到 bind 之间的 close 做代际关联。
