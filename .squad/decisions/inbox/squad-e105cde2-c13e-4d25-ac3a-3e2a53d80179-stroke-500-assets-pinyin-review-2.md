## Proposal from tester
Run: squad-e105cde2-c13e-4d25-ac3a-3e2a53d80179

PROPOSAL: 设备 SPY1 parser 应与 Python load_all 语义一致，拒绝 char/group membership 或 rank 不一致的 CRC-valid blob，并补重算 CRC 后的 offset/overflow/unaligned corruption 测试。
