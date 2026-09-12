#include "protocol_selection.h"

#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

ProtocolTransport Select(bool mqtt, bool websocket, bool prefer_websocket_for_stroke_voice) {
    ProtocolSelectionInput input;
    input.has_mqtt_config = mqtt;
    input.has_websocket_config = websocket;
    input.prefer_websocket_for_stroke_voice = prefer_websocket_for_stroke_voice;
    return SelectProtocolTransport(input);
}

}  // namespace

int main() {
    Expect(Select(true, true, true) == ProtocolTransport::Websocket,
           "both configs with stroke preference must select websocket, not mqtt");
    Expect(Select(true, true, false) == ProtocolTransport::Mqtt,
           "both configs without stroke preference keep mqtt-first");
    Expect(Select(true, false, true) == ProtocolTransport::Mqtt,
           "mqtt-only with stroke preference stays mqtt");
    Expect(Select(true, false, false) == ProtocolTransport::Mqtt,
           "mqtt-only without stroke preference stays mqtt");
    Expect(Select(false, true, true) == ProtocolTransport::Websocket,
           "websocket-only with stroke preference stays websocket");
    Expect(Select(false, true, false) == ProtocolTransport::Websocket,
           "websocket-only without stroke preference stays websocket");
    Expect(Select(false, false, true) == ProtocolTransport::Mqtt,
           "no config with stroke preference keeps mqtt fallback");
    Expect(Select(false, false, false) == ProtocolTransport::Mqtt,
           "no config without stroke preference keeps mqtt fallback");

    Expect(ProtocolTransportName(ProtocolTransport::Mqtt) != nullptr &&
               ProtocolTransportName(ProtocolTransport::Mqtt)[0] == 'm',
           "mqtt transport name");
    Expect(ProtocolTransportName(ProtocolTransport::Websocket) != nullptr &&
               ProtocolTransportName(ProtocolTransport::Websocket)[0] == 'w',
           "websocket transport name");

    if (failures != 0) {
        std::cerr << "protocol_selection_harness: FAIL (" << failures << ")\n";
        return 1;
    }
    std::cout << "protocol_selection_harness: PASS\n";
    return 0;
}
