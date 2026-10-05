#pragma once
#include "rabbit/protocol/workout_service.hpp"

namespace rabbit::protocol {
constexpr std::size_t kAudioNameBytes = 48;
enum class AudioMode : std::uint8_t { Off, Buzzer, Bluetooth, Both };
enum class AudioConnection : std::uint8_t { Disabled, Idle, Scanning, Connecting, Connected, Streaming, Error };
enum AudioFlags : std::uint8_t {
    AudioSupported = 1, AudioBuzzerAvailable = 2, AudioPaired = 4,
    AudioVolumeConfirmed = 8, AudioTestActive = 16, AudioSettingsPending = 32,
    AudioSoundConfirmed = 64, AudioWorkoutRunning = 128
};
constexpr std::size_t kAudioDeviceLimit = 16;
constexpr std::size_t kAudioDevicesPerPage = 4;
struct AudioDevice {
    std::array<std::uint8_t, 6> address{};
    std::int8_t rssi{0}; // zero means unavailable
    std::array<char, kAudioNameBytes + 1> name{};
    bool sound_confirmed{false};
};
constexpr std::size_t kAudioSavedLimit = 8;
constexpr std::size_t kAudioSavedStorageBytes = 2 + kAudioSavedLimit * 56;
struct AudioDevicePage;
struct AudioSavedSpeakers {
    std::uint8_t count{0};
    std::array<AudioDevice, kAudioSavedLimit> devices{};
};
const AudioDevice* find_saved_speaker(const AudioSavedSpeakers& saved, const std::array<std::uint8_t, 6>& address);
bool remember_speaker(AudioSavedSpeakers& saved, const AudioDevice& device);
bool remove_saved_speaker(AudioSavedSpeakers& saved, const std::array<std::uint8_t, 6>& address);
std::size_t encode_saved_storage(const AudioSavedSpeakers& saved, std::uint8_t* bytes);
bool decode_saved_storage(const std::uint8_t* bytes, std::size_t size, AudioSavedSpeakers& saved);
void encode_audio_saved(const AudioSavedSpeakers& saved, std::uint8_t offset, Frame& frame);
bool decode_audio_saved(const Frame& frame, AudioDevicePage& page);
struct AudioDiscovery {
    bool scanning{false}, full{false};
    std::uint8_t count{0};
    std::array<AudioDevice, kAudioDeviceLimit> devices{};
};
struct AudioDevicePage {
    bool scanning{false}, full{false};
    std::uint8_t total{0}, offset{0}, count{0};
    std::array<AudioDevice, kAudioDevicesPerPage> devices{};
};
void encode_audio_devices(const AudioDiscovery& discovery, std::uint8_t offset, Frame& frame);
[[nodiscard]] bool decode_audio_devices(const Frame& frame, AudioDevicePage& page);
struct AudioStatus {
    AudioMode mode{AudioMode::Both};
    std::uint8_t volume{50};
    AudioConnection connection{AudioConnection::Idle};
    std::uint8_t flags{0};
    std::uint8_t actual_volume{255}; // 0..100, 255 means unknown
    std::uint8_t last_error{0};
    std::array<std::uint8_t, 6> address{};
    std::array<char, kAudioNameBytes + 1> name{};
};
[[nodiscard]] bool is_audio_request(MessageType type);
void encode_audio_status(const AudioStatus& status, Frame& frame);
[[nodiscard]] bool decode_audio_status(const Frame& frame, AudioStatus& status);

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual AudioStatus status() const = 0;
    virtual void configure(AudioMode mode, std::uint8_t volume) = 0;
    virtual bool pair(const char* name) = 0;
    virtual void disconnect() = 0;
    virtual bool test() = 0;
    virtual void forget() = 0;
    virtual const AudioDiscovery& discovery() const = 0;
    virtual bool scan() = 0;
    virtual void stop_scan() = 0;
    virtual bool connect(const std::array<std::uint8_t, 6>& address, AudioMode mode, std::uint8_t volume) = 0;
    virtual bool confirm() = 0;
    virtual const AudioSavedSpeakers& saved_speakers() const = 0;
    virtual bool forget_device(const std::array<std::uint8_t, 6>& address) = 0;
};
class AudioService {
public:
    explicit AudioService(AudioBackend& backend) : backend_(backend) {}
    // Returns false for ordinary workout messages; these stay with WorkoutService.
    bool handle(const Frame& request, std::uint32_t active_session, bool running, Frame& response);
    bool blocks_start() const {
        const auto value = backend_.status();
        return backend_.discovery().scanning || value.connection == AudioConnection::Scanning || value.connection == AudioConnection::Connecting ||
            (value.flags & AudioTestActive) != 0;
    }
private:
    AudioBackend& backend_;
    Frame last_request_{};
    Frame last_response_{};
    bool have_last_{false};
};
} // namespace rabbit::protocol
