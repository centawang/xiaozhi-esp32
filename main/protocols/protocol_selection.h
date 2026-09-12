#ifndef PROTOCOL_SELECTION_H
#define PROTOCOL_SELECTION_H

// Narrow, host-testable transport choice used by Application::InitializeProtocol.
// Does not inspect endpoints, tokens, or session identity.

enum class ProtocolTransport {
    Mqtt,
    Websocket,
};

struct ProtocolSelectionInput {
    bool has_mqtt_config = false;
    bool has_websocket_config = false;
    bool prefer_websocket_for_stroke_voice = false;
};

inline ProtocolTransport SelectProtocolTransport(const ProtocolSelectionInput& input) {
    if (input.prefer_websocket_for_stroke_voice && input.has_websocket_config) {
        return ProtocolTransport::Websocket;
    }
    if (input.has_mqtt_config) {
        return ProtocolTransport::Mqtt;
    }
    if (input.has_websocket_config) {
        return ProtocolTransport::Websocket;
    }
    return ProtocolTransport::Mqtt;
}

inline const char* ProtocolTransportName(ProtocolTransport transport) {
    switch (transport) {
        case ProtocolTransport::Websocket:
            return "websocket";
        case ProtocolTransport::Mqtt:
            return "mqtt";
    }
    return "mqtt";
}

#endif  // PROTOCOL_SELECTION_H
