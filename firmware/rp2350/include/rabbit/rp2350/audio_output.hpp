#pragma once
#include "rabbit/protocol/audio_service.hpp"
namespace rabbit::rp2350 {
// One foreground-owned radio/output controller. BTstack callbacks run from update().
class AudioOutput final : public protocol::AudioBackend {
public:
    void initialise();
    void update(core::Microseconds now_us, bool beep, bool running);
    bool buzzer_on() const;
    protocol::AudioStatus status() const override;
    void configure(protocol::AudioMode mode, std::uint8_t volume) override;
    bool pair(const char* name) override;
    void disconnect() override;
    bool test() override;
    void forget() override;
    const protocol::AudioDiscovery& discovery() const override;
    bool scan() override;
    void stop_scan() override;
    bool connect(const std::array<std::uint8_t, 6>& address, protocol::AudioMode mode, std::uint8_t volume) override;
    bool confirm() override;
    const protocol::AudioSavedSpeakers& saved_speakers() const override;
    bool forget_device(const std::array<std::uint8_t, 6>& address) override;
};
} // namespace rabbit::rp2350
