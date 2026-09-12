## Proposal from tester
Run: squad-fad57277-d098-4bde-81a7-f7b166181613

PROPOSAL: 建立统一的 main-task `AbortStrokeRound(generation, reason)`，由用户取消、alert、power-save、asset suspend、surface delete、unexpected DeviceState、channel close 和 reboot 共用，并负责停止 pending/active listening、清上行残留和原子释放 overlay。
