## Proposal from tester
Run: squad-24d374f4-8ab4-47ec-ac4d-7e6ff854e255

PROPOSAL: 在 `MarkListeningStarted` 和 `ShowListeningFromMain` 前验证 `AudioService::IsAudioProcessorRunning()`；失败必须终止当前 stroke round，不显示 Speak、不启动 speech timeout。
