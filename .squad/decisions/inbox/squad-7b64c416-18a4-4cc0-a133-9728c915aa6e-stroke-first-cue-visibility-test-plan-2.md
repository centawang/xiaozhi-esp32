## Proposal from tester
Run: squad-7b64c416-18a4-4cc0-a133-9728c915aa6e

PROPOSAL: 首轮优先 arm-only callback 加 native LVGL/真机验证，不默认加入 `lv_refr_now`；只有普通调度仍失败时再考虑 refresh/flush-based arming。
