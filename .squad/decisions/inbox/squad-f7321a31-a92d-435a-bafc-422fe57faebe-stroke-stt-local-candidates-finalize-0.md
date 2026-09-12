## Proposal from lead
Run: squad-f7321a31-a92d-435a-bafc-422fe57faebe

PROPOSAL: 进入真机验收时分别验证 WebSocket 与 MQTT/UDP 的 hello、STT、TTS、LLM 都携带稳定且每 channel 可区分的 session ID；若生产 MQTT 服务不能保证，应在服务端增加可回显的 open request nonce，而不是放宽固件 fail-closed 路由。
