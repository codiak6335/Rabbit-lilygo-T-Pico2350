#include "rabbit/rp2350/speaker_signal.hpp"
#include <cstdio>
#include <cstdlib>

using rabbit::rp2350::SpeakerSignal;
namespace {
void check(const bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void check_long_gap(const std::uint32_t rate, const bool remote_volume) {
    SpeakerSignal signal, alarm;
    signal.reset(rate); alarm.reset(rate);
    // Eleven minutes, with actual workout cues five and ten minutes apart.
    // Compare overlapping maintenance pulses with an independent full alarm stream.
    for (std::uint32_t second = 0; second < 660; ++second) {
        std::uint32_t activity = 0;
        int maintenance_peak = 0;
        std::int16_t first = 0, last = 0;
        for (std::uint32_t sample = 0; sample < rate; ++sample) {
            const bool beep = second % 300 == 0 && sample < rate * 150 / 1000;
            const auto value = signal.next(beep, 49, remote_volume);
            const auto reference = alarm.next(true, 49, remote_volume);
            if (beep) check(value == reference, "Keep-alive must never alter or delay a workout beep");
            else if (second % 30 != 0) check(value == 0, "Silence between maintenance bursts");
            else maintenance_peak = std::max(maintenance_peak, std::abs(static_cast<int>(value)));
            if (value) ++activity;
            if (sample == 0) first = value;
            last = value;
        }
        if (second % 30 == 0) {
            check(activity > rate / 2, "Nonzero audio must continue throughout every long inter-rep gap");
            check(maintenance_peak > 100 && maintenance_peak <= 256, "Quiet maintenance audio survives integer rounding and remains below 1% full scale");
            check(first == 0 && last == 0, "Keep-alive starts and ends without a click");
        } else check(activity == 0, "Keep-alive creates no extra workout cues");
    }
}
void check_mute_and_reset() {
    SpeakerSignal signal;
    signal.reset(44100);
    for (std::uint32_t i = 0; i < 44100 * 31; ++i) {
        check(signal.next(i % 2 == 0, 0, true) == 0, "Volume zero mutes alarms and keep-alive");
    }
    signal.reset(48000);
    std::uint32_t activity = 0;
    for (std::uint32_t i = 0; i < 48000; ++i) if (signal.next(false, 49, true)) ++activity;
    check(activity > 24000, "A reconnected stream restarts keep-alive immediately");
}
} // namespace
int main() {
    check_long_gap(44100, true);
    check_long_gap(48000, false);
    check_mute_and_reset();
    std::puts("speaker signal: long intervals, beep priority, quiet envelope, volume and reconnect passed");
}
