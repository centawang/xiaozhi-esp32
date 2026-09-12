## Proposal from tester
Run: squad-4a2acf71-3fd2-4713-a004-53bff331c30d

PROPOSAL: 在所有会替换/取消 stroke 的异步入口先发布 generation-bound cancel fence，并让 `StartListeningAudio()` 在发送 start/启麦前再次检查，避免 blocking open 后短暂启动已取消的轮次。
