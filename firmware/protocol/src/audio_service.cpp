#include "rabbit/protocol/audio_service.hpp"
#include <cstring>
#include <algorithm>

namespace rabbit::protocol {
bool is_audio_request(const MessageType type) {
    return type >= MessageType::AudioStatus && type <= MessageType::AudioForgetDevice;
}
const AudioDevice* find_saved_speaker(const AudioSavedSpeakers& saved, const std::array<std::uint8_t, 6>& address) {
    for (std::size_t i = 0; i < saved.count; ++i) if (saved.devices[i].address == address) return &saved.devices[i];
    return nullptr;
}
bool remember_speaker(AudioSavedSpeakers& saved, const AudioDevice& device) {
    if (std::all_of(device.address.begin(), device.address.end(), [](std::uint8_t byte) { return byte == 0; })) return false;
    std::size_t index = 0;
    while (index < saved.count && saved.devices[index].address != device.address) ++index;
    if (index == kAudioSavedLimit) return false; // Never silently evict another speaker.
    const bool confirmed = index < saved.count && saved.devices[index].sound_confirmed;
    if (index == saved.count) ++saved.count;
    saved.devices[index] = device;
    saved.devices[index].name[kAudioNameBytes] = 0;
    saved.devices[index].sound_confirmed |= confirmed;
    saved.devices[index].rssi = 0;
    return true;
}
bool remove_saved_speaker(AudioSavedSpeakers& saved, const std::array<std::uint8_t, 6>& address) {
    std::size_t index = 0;
    while (index < saved.count && saved.devices[index].address != address) ++index;
    if (index == saved.count) return false;
    for (std::size_t i = index + 1; i < saved.count; ++i) saved.devices[i - 1] = saved.devices[i];
    saved.devices[--saved.count] = {};
    return true;
}
std::size_t encode_saved_storage(const AudioSavedSpeakers& saved, std::uint8_t* bytes) {
    bytes[0] = 1; bytes[1] = saved.count;
    for (std::size_t i = 0; i < saved.count; ++i) {
        const auto& device = saved.devices[i];
        auto* record = bytes + 2 + i * 56;
        std::memcpy(record, device.address.data(), 6); record[6] = device.sound_confirmed;
        std::memcpy(record + 7, device.name.data(), 49); record[55] = 0;
    }
    return 2 + saved.count * 56U;
}
bool decode_saved_storage(const std::uint8_t* bytes, const std::size_t size, AudioSavedSpeakers& saved) {
    if (size < 2 || bytes[0] != 1 || bytes[1] > kAudioSavedLimit || size != 2 + bytes[1] * 56U) return false;
    // Validate everything first so a corrupt record cannot partially replace the list.
    for (std::size_t i = 0; i < bytes[1]; ++i) {
        const auto* record = bytes + 2 + i * 56;
        if (record[6] > 1 || record[55] != 0 || std::all_of(record, record + 6, [](std::uint8_t b) { return b == 0; })) return false;
        for (std::size_t j = 0; j < i; ++j) if (std::memcmp(record, bytes + 2 + j * 56, 6) == 0) return false;
    }
    saved = {}; saved.count = bytes[1];
    for (std::size_t i = 0; i < saved.count; ++i) {
        const auto* record = bytes + 2 + i * 56;
        std::memcpy(saved.devices[i].address.data(), record, 6);
        saved.devices[i].sound_confirmed = record[6];
        std::memcpy(saved.devices[i].name.data(), record + 7, 49);
    }
    return true;
}
void encode_audio_saved(const AudioSavedSpeakers& saved, const std::uint8_t offset, Frame& frame) {
    frame.type = MessageType::AudioSaved;
    const auto start = std::min(offset, saved.count);
    const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(saved.count - start, kAudioDevicesPerPage));
    frame.payload[0] = 1; frame.payload[1] = 0; frame.payload[2] = saved.count == kAudioSavedLimit;
    frame.payload[3] = saved.count; frame.payload[4] = start; frame.payload[5] = count;
    std::size_t cursor = 6;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& device = saved.devices[start + i];
        std::size_t length = 0;
        while (length < kAudioNameBytes && device.name[length]) ++length;
        frame.payload[cursor++] = static_cast<std::uint8_t>(length);
        std::memcpy(frame.payload.data() + cursor, device.address.data(), 6); cursor += 6;
        frame.payload[cursor++] = device.sound_confirmed;
        std::memcpy(frame.payload.data() + cursor, device.name.data(), length); cursor += length;
    }
    frame.payload_size = static_cast<std::uint16_t>(cursor);
}
bool decode_audio_saved(const Frame& frame, AudioDevicePage& page) {
    if (frame.type != MessageType::AudioSaved || frame.payload[3] > kAudioSavedLimit || frame.payload[1]) return false;
    Frame copy = frame; copy.type = MessageType::AudioDevices;
    if (!decode_audio_devices(copy, page)) return false;
    for (std::size_t i = 0; i < page.count; ++i) {
        if (page.devices[i].rssi < 0 || page.devices[i].rssi > 1) return false;
        page.devices[i].sound_confirmed = page.devices[i].rssi != 0; page.devices[i].rssi = 0;
    }
    return true;
}
void encode_audio_devices(const AudioDiscovery& discovery, const std::uint8_t offset, Frame& frame) {
    frame.type = MessageType::AudioDevices;
    const auto total = static_cast<std::uint8_t>(std::min<std::size_t>(discovery.count, kAudioDeviceLimit));
    const auto start = std::min(offset, total);
    const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(total - start, kAudioDevicesPerPage));
    frame.payload[0] = 1; frame.payload[1] = discovery.scanning; frame.payload[2] = discovery.full;
    frame.payload[3] = total; frame.payload[4] = start; frame.payload[5] = count;
    std::size_t cursor = 6;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& device = discovery.devices[start + index];
        std::size_t length = 0;
        while (length < kAudioNameBytes && device.name[length]) ++length;
        frame.payload[cursor++] = static_cast<std::uint8_t>(length);
        std::memcpy(frame.payload.data() + cursor, device.address.data(), 6); cursor += 6;
        frame.payload[cursor++] = static_cast<std::uint8_t>(device.rssi);
        std::memcpy(frame.payload.data() + cursor, device.name.data(), length); cursor += length;
    }
    frame.payload_size = static_cast<std::uint16_t>(cursor);
}
bool decode_audio_devices(const Frame& frame, AudioDevicePage& page) {
    if (frame.type != MessageType::AudioDevices || frame.payload_size < 6 || frame.payload_size > kMaxPayloadBytes ||
        frame.payload[0] != 1 || frame.payload[1] > 1 || frame.payload[2] > 1 || frame.payload[3] > kAudioDeviceLimit ||
        frame.payload[4] > frame.payload[3] || frame.payload[5] > kAudioDevicesPerPage ||
        frame.payload[4] + frame.payload[5] > frame.payload[3]) return false;
    page = {};
    page.scanning = frame.payload[1]; page.full = frame.payload[2];
    page.total = frame.payload[3]; page.offset = frame.payload[4]; page.count = frame.payload[5];
    std::size_t cursor = 6;
    for (std::size_t index = 0; index < page.count; ++index) {
        if (cursor + 8 > frame.payload_size) return false;
        const auto length = frame.payload[cursor++];
        if (length > kAudioNameBytes || cursor + 7 + length > frame.payload_size) return false;
        auto& device = page.devices[index];
        std::memcpy(device.address.data(), frame.payload.data() + cursor, 6); cursor += 6;
        device.rssi = static_cast<std::int8_t>(frame.payload[cursor++]);
        std::memcpy(device.name.data(), frame.payload.data() + cursor, length); cursor += length;
    }
    return cursor == frame.payload_size;
}
void encode_audio_status(const AudioStatus& status, Frame& frame) {
    std::size_t length = 0;
    while (length < kAudioNameBytes && status.name[length] != '\0') ++length;
    frame.type = MessageType::AudioStatus;
    frame.payload[0] = 1;
    frame.payload[1] = static_cast<std::uint8_t>(status.mode);
    frame.payload[2] = status.volume;
    frame.payload[3] = static_cast<std::uint8_t>(status.connection);
    frame.payload[4] = status.flags;
    frame.payload[5] = status.actual_volume;
    frame.payload[6] = status.last_error;
    frame.payload[7] = static_cast<std::uint8_t>(length);
    std::memcpy(frame.payload.data() + 8, status.address.data(), 6);
    std::memcpy(frame.payload.data() + 14, status.name.data(), length);
    frame.payload_size = static_cast<std::uint16_t>(14 + length);
}
bool decode_audio_status(const Frame& frame, AudioStatus& status) {
    if (frame.type != MessageType::AudioStatus || frame.payload_size < 14 || frame.payload[0] != 1 ||
        frame.payload[1] > 3 || frame.payload[2] > 100 || frame.payload[3] > 6 ||
        (frame.payload[5] > 100 && frame.payload[5] != 255) || frame.payload[7] > kAudioNameBytes ||
        frame.payload_size != 14U + frame.payload[7]) return false;
    status = {};
    status.mode = static_cast<AudioMode>(frame.payload[1]);
    status.volume = frame.payload[2];
    status.connection = static_cast<AudioConnection>(frame.payload[3]);
    status.flags = frame.payload[4];
    status.actual_volume = frame.payload[5];
    status.last_error = frame.payload[6];
    std::memcpy(status.address.data(), frame.payload.data() + 8, 6);
    std::memcpy(status.name.data(), frame.payload.data() + 14, frame.payload[7]);
    return true;
}
bool AudioService::handle(const Frame& request, const std::uint32_t active_session,
                          const bool running, Frame& response) {
    if (!is_audio_request(request.type)) return false;
    response = {};
    response.session_id = active_session == 0 ? request.session_id : active_session;
    response.request_id = request.request_id;
    const auto reject = [&](const ServiceError error) {
        response.type = MessageType::Reject;
        response.payload[0] = static_cast<std::uint8_t>(request.type);
        response.payload[1] = static_cast<std::uint8_t>(error);
        response.payload_size = 2;
        return true;
    };
    if (active_session == 0 || request.session_id != active_session) return reject(ServiceError::Session);
    if (request.payload_size > kMaxPayloadBytes) return reject(ServiceError::Payload);
    if (have_last_ && last_request_.session_id == request.session_id &&
        last_request_.request_id == request.request_id && request.type != MessageType::AudioStatus && request.type != MessageType::AudioDevices && request.type != MessageType::AudioSaved) {
        if (last_request_.type != request.type || last_request_.payload_size != request.payload_size ||
            std::memcmp(last_request_.payload.data(), request.payload.data(), request.payload_size) != 0) {
            return reject(ServiceError::Payload);
        }
        response = last_response_;
        return true;
    }
    const auto size = request.payload_size;
    char name[kAudioNameBytes + 1]{};
    switch (request.type) {
    case MessageType::AudioConfigure:
        if (size != 2 || request.payload[0] > 3 || request.payload[1] > 100) return reject(ServiceError::Payload);
        break;
    case MessageType::AudioPair:
        if (size == 0 || size > kAudioNameBytes) return reject(ServiceError::Payload);
        for (std::size_t index = 0; index < size; ++index) {
            if (request.payload[index] < 32 || request.payload[index] == 127) return reject(ServiceError::Payload);
        }
        std::memcpy(name, request.payload.data(), size);
        break;
    case MessageType::AudioDevices:
        if (size != 1 || request.payload[0] > kAudioDeviceLimit) return reject(ServiceError::Payload);
        break;
    case MessageType::AudioSaved:
        if (size != 1 || request.payload[0] > kAudioSavedLimit) return reject(ServiceError::Payload);
        break;
    case MessageType::AudioForgetDevice:
        if (size != 6 || std::all_of(request.payload.begin(), request.payload.begin() + 6, [](std::uint8_t b) { return b == 0; })) return reject(ServiceError::Payload);
        break;
    case MessageType::AudioConnect:
        if (size != 8 || request.payload[6] < 2 || request.payload[6] > 3 || request.payload[7] > 100 ||
            std::all_of(request.payload.begin(), request.payload.begin() + 6, [](std::uint8_t b) { return b == 0; }))
            return reject(ServiceError::Payload);
        break;
    default:
        if (size != 0) return reject(ServiceError::Payload);
        break;
    }
    if (running && (request.type == MessageType::AudioPair || request.type == MessageType::AudioTest ||
                    request.type == MessageType::AudioForget || request.type == MessageType::AudioScan ||
                    request.type == MessageType::AudioConnect || request.type == MessageType::AudioConfirm || request.type == MessageType::AudioForgetDevice)) return reject(ServiceError::Command);
    if ((request.type == MessageType::AudioPair || request.type == MessageType::AudioForget ||
         request.type == MessageType::AudioScan || request.type == MessageType::AudioConnect || request.type == MessageType::AudioConfirm || request.type == MessageType::AudioForgetDevice) &&
        !(backend_.status().flags & AudioSupported)) return reject(ServiceError::Unsupported);
    switch (request.type) {
    case MessageType::AudioConfigure:
        backend_.configure(static_cast<AudioMode>(request.payload[0]), request.payload[1]); break;
    case MessageType::AudioPair:
        if (!backend_.pair(name)) return reject(ServiceError::Command);
        break;
    case MessageType::AudioDisconnect: backend_.disconnect(); break;
    case MessageType::AudioTest:
        if (!backend_.test()) return reject(ServiceError::Command);
        break;
    case MessageType::AudioForget: backend_.forget(); break;
    case MessageType::AudioForgetDevice: {
        std::array<std::uint8_t, 6> address{};
        std::memcpy(address.data(), request.payload.data(), 6);
        if (!backend_.forget_device(address)) return reject(ServiceError::Command);
        break;
    }
    case MessageType::AudioScan:
        if (!backend_.scan()) return reject(ServiceError::Command);
        break;
    case MessageType::AudioScanStop: backend_.stop_scan(); break;
    case MessageType::AudioConnect: {
        std::array<std::uint8_t, 6> address{};
        std::memcpy(address.data(), request.payload.data(), address.size());
        if (!backend_.connect(address, static_cast<AudioMode>(request.payload[6]), request.payload[7])) return reject(ServiceError::Command);
        break;
    }
    case MessageType::AudioConfirm:
        if (!backend_.confirm()) return reject(ServiceError::Command);
        break;
    default: break;
    }
    auto value = backend_.status();
    if (running) value.flags |= AudioWorkoutRunning;
    if (request.type == MessageType::AudioDevices) encode_audio_devices(backend_.discovery(), request.payload[0], response);
    else if (request.type == MessageType::AudioSaved) encode_audio_saved(backend_.saved_speakers(), request.payload[0], response);
    else encode_audio_status(value, response);
    if (request.type != MessageType::AudioStatus && request.type != MessageType::AudioDevices && request.type != MessageType::AudioSaved) {
        last_request_ = request;
        last_response_ = response;
        have_last_ = true;
    }
    return true;
}
} // namespace rabbit::protocol
