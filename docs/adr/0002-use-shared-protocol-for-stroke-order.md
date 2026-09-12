---
status: accepted
---

# Carry stroke-order interaction in the shared Protocol

Candidate presentation, selection, stroke-data delivery, cancellation, and errors will use a dedicated `type: "stroke"` message family in the transport-neutral `Protocol` layer. Both WebSocket and MQTT control paths must implement the same semantics, and a device advertises the `stroke_order` hello capability only when the build and board support the feature.

The versioned exchange is staged: the server sends bounded candidates with a request ID, the device selects one character, and the server returns only that character's stroke data. Selection, retry, data, cancellation, and error messages are idempotent within the active request; cancelled, superseded, or otherwise stale responses are discarded. Unknown versions or actions fail only the current stroke request and do not close the normal conversation transport.

MCP was rejected because its current property model supports only scalar bool, int, and string values and its tool-call lifecycle does not naturally model an asynchronous screen selection. The existing `custom` message was rejected because it has no stable product semantics and currently only renders payloads as system text. The feature entry is enabled only after both device and server have confirmed support; already loaded data remains playable after a later disconnect.

## Amendment — local-first CoreS3 prototype

The local prototype does **not** send or require `type=stroke` and does not advertise `features.stroke_order` until the networked path is implemented. After an explicit 笔划-mode listen round, the device reuses ordinary STT text, stops listening, and resolves candidates from the local SOB1 set. `type=stroke` remains the transport-neutral contract for the later server-delivered candidates/data exchange on both WebSocket and MQTT.
