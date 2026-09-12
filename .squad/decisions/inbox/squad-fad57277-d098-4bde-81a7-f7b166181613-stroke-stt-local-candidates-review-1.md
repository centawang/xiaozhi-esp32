## Proposal from tester
Run: squad-fad57277-d098-4bde-81a7-f7b166181613

PROPOSAL: 用单一原子发布的 session 快照或受同一锁保护的 generation 状态，并把 expected generation 显式传入 StartListening/ContinueOpenAudioChannel；仅在 generation 仍匹配时启动麦克风。对服务端不回传 round ID 的限制，应采用可证明的旧轮隔离/排空策略。
