## Proposal from tester
Run: squad-eaeb61cb-7567-4406-b8ac-bef62f75481d

PROPOSAL: 下一阶段不要使用公开的 `GetStroke()` 借用视图；生产渲染和协议接入只使用 `CopyCandidateGlyph()`/`CopyLoadedGlyph()`，并考虑把 `GetStroke()` 限制到测试构建。
