#include "rabbit/protocol/audio_service.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace rabbit::protocol;
namespace {
void check(bool pass, const char* message) { if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
struct Backend : AudioBackend {
    AudioStatus value{};
    AudioDiscovery found{};
    AudioSavedSpeakers saved{};
    int scans = 0, connects = 0, confirms = 0;
    bool allow_connect = false;
    int tests = 0, pairs = 0, configs = 0, forgets = 0, disconnects = 0;
    Backend() { value.flags = AudioSupported; std::strcpy(value.name.data(), "MEGABOOM 3"); }
    AudioStatus status() const override { return value; }
    void configure(AudioMode mode, std::uint8_t volume) override { value.mode = mode; value.volume = volume; ++configs; }
    bool pair(const char* name) override { std::strcpy(value.name.data(), name); ++pairs; return true; }
    void disconnect() override { ++disconnects; }
    bool test() override { ++tests; return true; }
    void forget() override { ++forgets; }
    const AudioDiscovery& discovery() const override { return found; }
    bool scan() override { found.scanning = true; ++scans; return true; }
    void stop_scan() override { found.scanning = false; }
    bool connect(const std::array<std::uint8_t, 6>&, AudioMode mode, std::uint8_t volume) override {
        if (!allow_connect) return false;
        value.mode = mode; value.volume = volume; ++connects; return true;
    }
    bool confirm() override { ++confirms; return true; }
    const AudioSavedSpeakers& saved_speakers() const override { return saved; }
    bool forget_device(const std::array<std::uint8_t, 6>& address) override { return remove_saved_speaker(saved, address); }
};
}
void test_saved_speakers() {
    AudioSavedSpeakers speakers{}, loaded{};
    AudioDevice boom{}, soundbar{};
    boom.address = {0xc0, 0x28, 0x8d, 0xdc, 0xc9, 0x3d}; std::strcpy(boom.name.data(), "MEGABOOM 3"); boom.sound_confirmed = true;
    soundbar.address = {0xe4, 0x7d, 0xbd, 0xb5, 0x9b, 0x70}; std::strcpy(soundbar.name.data(), "Samsung Soundbar"); soundbar.sound_confirmed = true;
    check(remember_speaker(speakers, boom) && remember_speaker(speakers, soundbar) && speakers.count == 2, "Selecting another speaker retains previous confirmed speaker");
    boom.sound_confirmed = false;
    check(remember_speaker(speakers, boom) && speakers.count == 2 && find_saved_speaker(speakers, boom.address)->sound_confirmed, "Reconnecting never loses previous sound confirmation or duplicates device");
    std::array<std::uint8_t, kAudioSavedStorageBytes> storage{};
    auto bytes = encode_saved_storage(speakers, storage.data());
    check(decode_saved_storage(storage.data(), bytes, loaded) && loaded.count == 2 &&
        find_saved_speaker(loaded, boom.address)->sound_confirmed && find_saved_speaker(loaded, soundbar.address), "Both devices and confirmation survive storage round trip");
    const auto preserved = loaded.devices[0].address;
    check(!decode_saved_storage(storage.data(), bytes - 1, loaded) && loaded.devices[0].address == preserved, "Truncated saved storage never partially replaces list");
    storage[8] = 2;
    check(!decode_saved_storage(storage.data(), bytes, loaded), "Invalid saved confirmation rejected");
    storage[8] = 1; std::memcpy(storage.data() + 58, storage.data() + 2, 6);
    check(!decode_saved_storage(storage.data(), bytes, loaded), "Duplicate stored addresses rejected");
    bytes = encode_saved_storage(speakers, storage.data()); storage[57] = 1;
    check(!decode_saved_storage(storage.data(), bytes, loaded), "Unterminated persistent speaker name rejected");
    check(remove_saved_speaker(speakers, boom.address) && speakers.count == 1 && find_saved_speaker(speakers, soundbar.address), "Forget removes only its addressed speaker");
    check(!remove_saved_speaker(speakers, boom.address) && speakers.count == 1, "Unknown Forget preserves other devices");
    for (std::uint8_t i = 1; i < kAudioSavedLimit; ++i) {
        boom.address[5] = i;
        std::memset(boom.name.data(), 'N', kAudioNameBytes); boom.name[kAudioNameBytes] = 0;
        check(remember_speaker(speakers, boom), "Saved registry fills to capacity");
    }
    boom.address[5] = 99;
    check(!remember_speaker(speakers, boom) && speakers.count == kAudioSavedLimit && find_saved_speaker(speakers, soundbar.address), "Full registry never silently evicts confirmed speaker");
    Frame frame{}; AudioDevicePage page{};
    for (std::uint8_t offset = 0; offset < kAudioSavedLimit; offset += kAudioDevicesPerPage) {
        encode_audio_saved(speakers, offset, frame);
        check(frame.payload_size <= 230 && decode_audio_saved(frame, page) && page.count == 4 && page.offset == offset && page.full, "Saved-device pages fit bounded UART frame");
        --frame.payload_size;
        check(!decode_audio_saved(frame, page), "Truncated saved page rejected");
    }
    Backend backend; backend.saved = speakers; AudioService service(backend);
    Frame request{}, response{}; request.type = MessageType::AudioSaved; request.session_id = 42; request.request_id = 1; request.payload_size = 1;
    check(service.handle(request, 42, true, response) && decode_audio_saved(response, page) && page.total == 8, "Saved list available during workout");
    request.payload[0] = 4;
    check(service.handle(request, 42, false, response) && decode_audio_saved(response, page) && page.offset == 4, "Read-only page requests are never deduplicated as mutations");
    request.payload[0] = 9;
    check(service.handle(request, 42, false, response) && response.type == MessageType::Reject, "Saved-list offset bounded");
    request.type = MessageType::AudioForgetDevice; request.payload_size = 6; request.request_id = 2;
    std::memcpy(request.payload.data(), soundbar.address.data(), 6);
    check(service.handle(request, 42, true, response) && response.type == MessageType::Reject && backend.saved.count == 8, "Addressed Forget blocked during workout");
    check(service.handle(request, 42, false, response) && response.type == MessageType::AudioStatus && backend.saved.count == 7, "Addressed Forget dispatches to saved registry");
    check(service.handle(request, 42, false, response) && response.type == MessageType::AudioStatus && backend.saved.count == 7, "Retried Forget returns original success without deleting another device");
}
int main() {
    test_saved_speakers();
    Backend backend;
    AudioService service(backend);
    Frame request{}, response{};
    request.type = MessageType::AudioTest; request.request_id = 1; request.session_id = 42;
    const auto rejected = [&](ServiceError error) {
        return response.type == MessageType::Reject && response.payload_size == 2 && response.payload[1] == static_cast<std::uint8_t>(error);
    };
    check(service.handle(request, 0, false, response) && rejected(ServiceError::Session), "Audio needs Hello session");
    check(service.handle(request, 99, false, response) && rejected(ServiceError::Session), "Reject old session");
    check(service.handle(request, 42, true, response) && rejected(ServiceError::Command) && backend.tests == 0, "Do not test in workout");
    check(service.handle(request, 42, false, response) && response.type == MessageType::AudioStatus && backend.tests == 1, "Test acknowledged by status");
    check(service.handle(request, 42, true, response) && backend.tests == 1, "Lost response retry never restarts beep test");
    request.type = MessageType::AudioForget;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Reused request ID cannot execute another action");
    request.request_id = 2;
    check(service.handle(request, 42, true, response) && rejected(ServiceError::Command) && backend.forgets == 0, "No flash key mutation in workout");
    request.type = MessageType::AudioConfigure; request.payload_size = 2;
    request.payload[0] = 3; request.payload[1] = 50;
    check(service.handle(request, 42, true, response) && backend.configs == 1 && backend.value.volume == 50, "Volume changes allowed in workout");
    request.payload[1] = 0;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Changed payload on retry rejected");
    request.request_id = 3;
    check(service.handle(request, 42, false, response) && backend.value.volume == 0, "Volume zero mutes");
    request.request_id = 4; request.payload[1] = 101;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Volume range bound");
    request.payload[1] = 100; request.payload[0] = 4;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Mode range bound");
    request.payload[0] = 2;
    check(service.handle(request, 42, false, response) && backend.value.volume == 100, "Volume maximum accepted");
    request.type = MessageType::AudioPair; request.request_id = 5; request.payload_size = 0;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Empty target rejected");
    request.payload_size = 49;
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "Bound speaker name");
    request.payload_size = 3; request.payload[0] = 'A'; request.payload[1] = 0; request.payload[2] = 'B';
    check(service.handle(request, 42, false, response) && rejected(ServiceError::Payload), "No embedded nulls");
    request.payload_size = 10; std::memcpy(request.payload.data(), "MEGABOOM 3", 10);
    check(service.handle(request, 42, true, response) && rejected(ServiceError::Command) && backend.pairs == 0, "No pairing in workout");
    check(service.handle(request, 42, false, response) && backend.pairs == 1, "Pair exact name");
    check(service.handle(request, 42, false, response) && backend.pairs == 1, "Retry does not rescan");
    request.type = MessageType::AudioStatus; request.payload_size = 0; request.request_id = 6;
    check(service.handle(request, 42, true, response), "Status allowed in workout");
    AudioStatus status{};
    check(decode_audio_status(response, status) && status.volume == 100 && std::strcmp(status.name.data(), "MEGABOOM 3") == 0, "Status codec round trip");
    EncodedFrame wire{};
    check(encode(response, wire), "Audio status framing");
    FrameDecoder decoder;
    Frame decoded{};
    DecodeError error{};
    bool received = false;
    for (std::size_t i = 0; i < wire.size; ++i) received = decoder.push(wire.bytes[i], decoded, error) || received;
    check(received && decode_audio_status(decoded, status), "CRC/COBS audio round trip");
    response.payload[0] = 2;
    check(!decode_audio_status(response, status), "Reject unknown audio status version");
    response.payload[0] = 1; --response.payload_size;
    check(!decode_audio_status(response, status), "Reject truncated status name");
    response.payload_size = 14; response.payload[7] = 0; response.payload[5] = 101;
    check(!decode_audio_status(response, status), "Reject invalid actual volume");
    std::memset(backend.value.name.data(), 'X', kAudioNameBytes); backend.value.name[kAudioNameBytes] = 0;
    encode_audio_status(backend.value, response);
    check(response.payload_size == 62 && decode_audio_status(response, status) && status.name[kAudioNameBytes] == 0, "Maximum name stays terminated");
    request.session_id = 7; request.type = MessageType::AudioTest; request.request_id = 1;
    check(service.handle(request, 7, false, response) && backend.tests == 2, "New session resets retry identity");
    backend.value.flags = 0; request.request_id = 2;
    request.type = MessageType::AudioPair; request.payload_size = 10;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Unsupported), "Unavailable radio rejected");
    request.type = MessageType::AudioTest; request.payload_size = 0;
    backend.value.flags = AudioBuzzerAvailable;
    check(service.handle(request, 7, false, response) && backend.tests == 3, "Buzzer test still works if radio fails");
    backend.value.connection = AudioConnection::Scanning;
    check(service.blocks_start(), "Pairing blocks workout start");
    backend.value.connection = AudioConnection::Connecting;
    check(service.blocks_start(), "Key negotiation blocks workout start");
    backend.value.connection = AudioConnection::Streaming;
    backend.value.flags = AudioTestActive;
    check(service.blocks_start(), "Diagnostic beeps cannot overlap workout start");
    backend.value.flags = AudioSettingsPending;
    check(!service.blocks_start(), "Deferred settings never block connected workout start");
    backend.value.flags = AudioSupported;
    request.session_id = 7; request.request_id = 20; request.type = MessageType::AudioScan; request.payload_size = 0;
    check(service.handle(request, 7, true, response) && rejected(ServiceError::Command) && backend.scans == 0, "No discovery in workout");
    check(service.handle(request, 7, false, response) && backend.scans == 1 && service.blocks_start(), "Separate discovery blocks workout start even while streaming");
    check(service.handle(request, 7, false, response) && backend.scans == 1, "Retried scan never clears device results");
    request.type = MessageType::AudioScanStop; request.request_id = 21;
    check(service.handle(request, 7, true, response) && !service.blocks_start(), "Scan cancel available during workout");
    request.type = MessageType::AudioConnect; request.request_id = 22; request.payload_size = 8;
    request.payload.fill(0); request.payload[6] = 2; request.payload[7] = 80;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Payload), "Zero speaker address rejected");
    request.payload[0] = 0xc0;
    const auto old_volume = backend.value.volume;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Command) && backend.value.volume == old_volume, "Unknown device never changes output settings");
    backend.allow_connect = true;
    check(service.handle(request, 7, true, response) && rejected(ServiceError::Command) && backend.connects == 0, "Connect denied during workout");
    request.payload[6] = 1;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Payload), "Connect requires Bluetooth output mode");
    request.payload[6] = 3; request.payload[7] = 101;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Payload), "Connect volume bounds checked atomically");
    request.payload[7] = 49;
    check(service.handle(request, 7, false, response) && backend.connects == 1 && backend.value.volume == 49, "Selected address connection accepted");
    check(service.handle(request, 7, false, response) && backend.connects == 1, "Retry does not restart selected connection");
    request.type = MessageType::AudioConfirm; request.request_id = 23; request.payload_size = 0;
    check(service.handle(request, 7, true, response) && rejected(ServiceError::Command) && backend.confirms == 0, "Sound confirmation denied during workout");
    check(service.handle(request, 7, false, response) && backend.confirms == 1, "Explicit sound confirmation routed");
    request.type = MessageType::AudioStatus; request.request_id = 24;
    check(service.handle(request, 7, true, response) && decode_audio_status(response, status) && (status.flags & AudioWorkoutRunning), "UI receives workout busy state");
    backend.found.count = kAudioDeviceLimit;
    backend.found.full = true;
    for (std::size_t i = 0; i < kAudioDeviceLimit; ++i) {
        auto& device = backend.found.devices[i]; device.address[5] = static_cast<std::uint8_t>(i + 1);
        device.rssi = -70; std::memset(device.name.data(), 'N', kAudioNameBytes);
    }
    request.type = MessageType::AudioDevices; request.payload_size = 1; request.payload[0] = 0;
    AudioDevicePage page{};
    check(service.handle(request, 7, false, response) && decode_audio_devices(response, page) && response.payload_size == 230 &&
        page.count == 4 && page.total == 16 && page.full && page.devices[0].rssi == -70, "Maximum discovery page fits UART payload");
    check(encode(response, wire), "Largest discovery page frames successfully");
    for (std::uint8_t offset = 0; offset < kAudioDeviceLimit; offset += kAudioDevicesPerPage) {
        request.payload[0] = offset;
        check(service.handle(request, 7, false, response) && decode_audio_devices(response, page) && page.offset == offset &&
            page.devices[0].address[5] == offset + 1, "Discovery pagination never conflates duplicate names");
        --response.payload_size;
        check(!decode_audio_devices(response, page), "Truncated discovery names rejected");
    }
    request.payload[0] = 16;
    check(service.handle(request, 7, false, response) && decode_audio_devices(response, page) && page.count == 0, "Empty trailing discovery page accepted");
    request.payload[0] = 17;
    check(service.handle(request, 7, false, response) && rejected(ServiceError::Payload), "Discovery offset bounded");
    encode_audio_devices(backend.found, 0, response); response.payload[5] = 5;
    check(!decode_audio_devices(response, page), "Discovery record count bounded");
    request.type = MessageType::Start;
    check(!service.handle(request, 7, false, response), "Workout messages stay in WorkoutService");
    rabbit::core::PoolProfile pool{};
    WorkoutService workout(pool);
    request.type = MessageType::Hello; request.request_id = 10;
    check(workout.handle(request, 0, response) && response.type == MessageType::Ack, "Legacy Hello remains supported");
    request.type = MessageType::Status; request.request_id = 11;
    check(workout.handle(request, 0, response) && response.type == MessageType::Status, "Legacy Status response unchanged");
    request.type = MessageType::AudioPair;
    check(workout.handle(request, 0, response) && rejected(ServiceError::Unsupported), "Old workout dispatcher never silently acknowledges audio");
    std::puts("audio protocol tests passed");
}
