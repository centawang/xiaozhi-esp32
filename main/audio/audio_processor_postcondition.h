#ifndef AUDIO_PROCESSOR_POSTCONDITION_H
#define AUDIO_PROCESSOR_POSTCONDITION_H

/**
 * Host-testable AudioService voice-processor postconditions.
 *
 * Stop() stores service_stopped_ first, then sets AS_EVENT_AUDIO_PROCESSOR_RUNNING
 * only to wake the input task. That wake bit is not a running claim. Capture is
 * running only when the service is live, the engine exists and reports
 * IsVoiceProcessingEnabled(), and this layer's event is set.
 *
 * Enable may publish the service event only after the engine reports enabled.
 * If the engine stays disabled, enable must disable/clear and return false.
 *
 * Disable succeeds only when the engine is actually disabled and the service
 * event is cleared. If the engine remains enabled, disable must not report
 * success and must keep the service event set so engine and event stay
 * diagnosable and consistent. Callers that ignore the bool remain safe because
 * IsAudioProcessorRunning() stays honest rather than claiming a stopped state.
 */
inline constexpr bool AudioProcessorIsRunning(bool service_stopped, bool engine_initialized,
                                              bool engine_present,
                                              bool engine_voice_processing_enabled,
                                              bool service_processor_event) {
    return !service_stopped && engine_initialized && engine_present &&
           engine_voice_processing_enabled && service_processor_event;
}

inline constexpr bool VoiceProcessingEnableMayPublishEvent(bool service_stopped,
                                                           bool engine_voice_processing_enabled) {
    return !service_stopped && engine_voice_processing_enabled;
}

inline constexpr bool VoiceProcessingDisableKeepServiceEvent(bool engine_voice_processing_enabled) {
    return engine_voice_processing_enabled;
}

inline constexpr bool VoiceProcessingDisableSucceeded(bool engine_voice_processing_enabled,
                                                      bool service_processor_event) {
    return !engine_voice_processing_enabled && !service_processor_event;
}

#endif  // AUDIO_PROCESSOR_POSTCONDITION_H
