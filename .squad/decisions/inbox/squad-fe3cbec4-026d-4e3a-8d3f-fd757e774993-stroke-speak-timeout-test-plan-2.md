## Proposal from tester
Run: squad-fe3cbec4-026d-4e3a-8d3f-fd757e774993

PROPOSAL: 让 StartListening 的诊断同时区分“未调用”和“调用但发送失败”；最可靠的最小实现是向上传递 `SendText()` 的 bool。
