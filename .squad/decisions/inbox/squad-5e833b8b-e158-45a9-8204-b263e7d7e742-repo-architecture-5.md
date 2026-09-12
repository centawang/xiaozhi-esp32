## Proposal from lead
Run: squad-5e833b8b-e158-45a9-8204-b263e7d7e742

PROPOSAL：所有网络输入使用版本化、严格限额的解析器；所有 LVGL 点击回调通过 `Application::Schedule()` 回主任务，动画由 LVGL timer/animation 驱动。
