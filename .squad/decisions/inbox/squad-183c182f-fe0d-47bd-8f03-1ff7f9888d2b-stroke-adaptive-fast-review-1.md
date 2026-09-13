## Proposal from rai
Run: squad-183c182f-fe0d-47bd-8f03-1ff7f9888d2b

PROPOSAL: 保持 LCD 实际 flush/逐笔可见性、动画中触摸响应、取消竞争和 100-session heap/timer/WDT 稳定性为发布前硬件门槛；若 301 ms 笔画在真机被 LVGL 合并而不可见，应重新拒绝并改用显示确认/节拍方案，而不是放宽 no-skip 断言。
