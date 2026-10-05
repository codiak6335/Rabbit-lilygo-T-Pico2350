#pragma once
#include <algorithm>
#include <cstdint>

namespace rabbit::rp2350 {
// Portable PCM generator so long intervals can be verified without a radio.
class SpeakerSignal {
public:
    void reset(const std::uint32_t sample_rate) {
        sample_rate_ = sample_rate;
        beep_phase_ = 0; keep_alive_phase_ = 0; cycle_sample_ = 0;
    }

    std::int16_t next(const bool beep, const std::uint8_t volume, const bool remote_volume) {
        const auto beep_index = beep_phase_ * 50 / sample_rate_;
        const auto keep_alive_index = keep_alive_phase_ * 50 / sample_rate_;
        const auto position = cycle_sample_;
        beep_phase_ = (beep_phase_ + 882) % sample_rate_;
        keep_alive_phase_ = (keep_alive_phase_ + 80) % sample_rate_;
        if (++cycle_sample_ == sample_rate_ * 30) cycle_sample_ = 0;

        if (volume == 0) return 0;
        const int gain = remote_volume ? 100 : volume;
        if (beep) return static_cast<std::int16_t>(kSine[beep_index] * gain / 100);
        if (position >= sample_rate_) return 0;

        // One second of 80 Hz every 30 seconds, 1/128 of the alarm amplitude.
        // Zero-filled A2DP packets alone do not reliably reset speaker idle timers.
        // Fade both ends over 20 ms to avoid a click resembling a workout cue.
        const auto fade_samples = sample_rate_ / 50;
        const auto envelope = std::min({position, sample_rate_ - position - 1, fade_samples});
        const auto quiet = kSine[keep_alive_index] * gain / (100 * 128);
        return static_cast<std::int16_t>(quiet * static_cast<int>(envelope) / static_cast<int>(fade_samples));
    }

private:
    std::uint32_t sample_rate_ = 44100;
    std::uint32_t beep_phase_ = 0, keep_alive_phase_ = 0, cycle_sample_ = 0;
    static constexpr std::int16_t kSine[] = {
        0, 4107, 8149, 12062, 15786, 19260, 22431, 25247, 27666, 29648,
        31163, 32187, 32702, 32702, 32187, 31163, 29648, 27666, 25247, 22431,
        19260, 15786, 12062, 8149, 4107, 0, -4107, -8149, -12062, -15786,
        -19260, -22431, -25247, -27666, -29648, -31163, -32187, -32702, -32702,
        -32187, -31163, -29648, -27666, -25247, -22431, -19260, -15786, -12062, -8149, -4107
    };
};
} // namespace rabbit::rp2350
