## Proposal from rai
Run: squad-2c3939cb-c923-4850-8cd5-b4060c72537d

PROPOSAL: 将 StepForward 改为 phase-local：若调用开始时在 gap，只进入下一笔 cue0 后立即返回；若调用开始时在 cue，只完成当前笔并进入 completed-contour/gap 状态后立即返回，禁止同一次动作既退出 gap 又完成新笔。
