## Proposal from tester
Run: squad-eaeb61cb-7567-4406-b8ac-bef62f75481d

PROPOSAL: 允许进入远端 STT 集成阶段，但只依赖 controller 的 owned-store、状态转换、显式单调时钟和 Copy* 数据接口，以及 view 的生命周期契约；新增一个主任务调度且内部持 display lock 的公开 UI session 入口，禁止网络 callback 直接操作 LVGL。
