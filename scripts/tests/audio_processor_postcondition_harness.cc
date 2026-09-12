#include "audio/audio_processor_postcondition.h"

#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeVoiceEngine {
public:
    bool enabled = false;
    bool enable_takes_effect = true;
    bool disable_takes_effect = true;
    mutable int query_count = 0;

    void EnableVoiceProcessing(bool enable) {
        if (enable) {
            if (enable_takes_effect) {
                enabled = true;
            }
            return;
        }
        if (disable_takes_effect) {
            enabled = false;
        }
    }

    bool IsVoiceProcessingEnabled() const {
        ++query_count;
        return enabled;
    }
};

class AudioServicePostconditionDriver {
public:
    bool service_stopped = false;
    bool engine_initialized = true;
    bool engine_present = true;
    bool service_event = false;
    FakeVoiceEngine engine;

    bool IsAudioProcessorRunning() const {
        const bool engine_enabled = engine_present && engine.IsVoiceProcessingEnabled();
        return AudioProcessorIsRunning(service_stopped, engine_initialized, engine_present,
                                       engine_enabled, service_event);
    }

    bool EnableVoiceProcessing(bool enable) {
        if (!enable) {
            bool engine_enabled = false;
            if (engine_initialized && engine_present) {
                engine.EnableVoiceProcessing(false);
                engine_enabled = engine.IsVoiceProcessingEnabled();
            }
            if (VoiceProcessingDisableKeepServiceEvent(engine_enabled)) {
                if (!service_stopped) {
                    service_event = true;
                }
                return false;
            }
            service_event = false;
            return VoiceProcessingDisableSucceeded(engine_enabled, service_event);
        }

        if (service_stopped || !engine_initialized || !engine_present) {
            service_event = false;
            return false;
        }

        engine.EnableVoiceProcessing(true);
        if (!VoiceProcessingEnableMayPublishEvent(service_stopped,
                                                  engine.IsVoiceProcessingEnabled())) {
            engine.EnableVoiceProcessing(false);
            service_event = false;
            return false;
        }
        service_event = true;
        return IsAudioProcessorRunning();
    }
};

}  // namespace

int main() {
    Expect(!AudioProcessorIsRunning(true, true, true, true, true),
           "Stop() wake bit is never a running postcondition");
    Expect(!AudioProcessorIsRunning(false, false, true, true, true),
           "uninitialized engine is not running");
    Expect(!AudioProcessorIsRunning(false, true, false, true, true),
           "missing engine is not running");
    Expect(AudioProcessorIsRunning(false, true, true, true, true),
           "live service + initialized engine + engine enabled + event is running");

    AudioServicePostconditionDriver refused_enable;
    refused_enable.engine.enable_takes_effect = false;
    Expect(!refused_enable.EnableVoiceProcessing(true), "engine false after enable returns false");
    Expect(!refused_enable.engine.IsVoiceProcessingEnabled(),
           "refused enable leaves the engine disabled");
    Expect(!refused_enable.service_event, "engine false must not publish the service event");
    Expect(!AudioProcessorIsRunning(false, true, true,
                                    refused_enable.engine.IsVoiceProcessingEnabled(), true),
           "engine false + attempted service event is not running");
    Expect(!refused_enable.IsAudioProcessorRunning(),
           "service running query stays false after refused enable");
    Expect(refused_enable.engine.query_count > 0,
           "refused-enable path queries IsVoiceProcessingEnabled()");

    AudioServicePostconditionDriver event_false;
    event_false.engine.EnableVoiceProcessing(true);
    event_false.service_event = false;
    Expect(event_false.engine.IsVoiceProcessingEnabled(), "engine true fixture is enabled");
    Expect(!event_false.IsAudioProcessorRunning(),
           "engine true but service event false is not running");

    AudioServicePostconditionDriver stopped;
    stopped.service_stopped = true;
    stopped.engine.EnableVoiceProcessing(true);
    stopped.service_event = true;
    Expect(stopped.engine.IsVoiceProcessingEnabled() && stopped.service_event,
           "stopped fixture keeps engine and wake event");
    Expect(!stopped.IsAudioProcessorRunning(), "stopped service is not running");
    Expect(!stopped.EnableVoiceProcessing(true), "enable while stopped clears and fails");
    Expect(!stopped.service_event, "enable while stopped does not keep a success event");

    AudioServicePostconditionDriver disable_stuck;
    Expect(disable_stuck.EnableVoiceProcessing(true) && disable_stuck.IsAudioProcessorRunning(),
           "successful enable publishes event and reports running");
    disable_stuck.engine.disable_takes_effect = false;
    Expect(!disable_stuck.EnableVoiceProcessing(false),
           "disable returns false when the engine stays enabled");
    Expect(disable_stuck.engine.IsVoiceProcessingEnabled(),
           "stuck disable leaves the engine enabled");
    Expect(disable_stuck.service_event, "stuck disable keeps the service event");
    Expect(disable_stuck.IsAudioProcessorRunning(),
           "stuck disable keeps a diagnosable running state");
    Expect(!VoiceProcessingDisableSucceeded(disable_stuck.engine.IsVoiceProcessingEnabled(),
                                            disable_stuck.service_event),
           "stuck disable is not a successful stop");

    AudioServicePostconditionDriver disable_ok;
    Expect(disable_ok.EnableVoiceProcessing(true), "disable-ok fixture enables");
    Expect(disable_ok.EnableVoiceProcessing(false) && !disable_ok.IsAudioProcessorRunning() &&
               !disable_ok.engine.IsVoiceProcessingEnabled() && !disable_ok.service_event,
           "successful disable requires engine disabled and event cleared");

    if (failures != 0) {
        std::cerr << "audio_processor_postcondition_harness: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio_processor_postcondition_harness: PASS\n";
    return 0;
}
