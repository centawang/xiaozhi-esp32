## Proposal from tester
Run: squad-20b4fe92-1249-4fb0-aad6-7726d083ba08

PROPOSAL: 将 AudioService 的 enable/running/disable bool 后置条件绑定到 `AudioEngine::IsVoiceProcessingEnabled()` 与本层 event，而不是只信任本层 event。
