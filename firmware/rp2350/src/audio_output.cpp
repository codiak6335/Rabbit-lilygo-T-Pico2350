#include "rabbit/rp2350/audio_output.hpp"
#include "rabbit/rp2350/board_config.hpp"
#include "rabbit/rp2350/speaker_signal.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "classic/btstack_sbc_bluedroid.h"

#if !PICO_CYW43_ARCH_POLL
#error "AudioOutput requires foreground CYW43 polling, not background IRQ callbacks"
#endif

namespace rabbit::rp2350 {
namespace {
using namespace protocol;
constexpr std::uint32_t kSettingsTag = 0x52415531; // RAU1, separate from BTstack link keys
constexpr std::size_t kSettingsBytes = 59;
constexpr std::uint32_t kSavedSpeakersTag = 0x52415331; // RAS1
AudioSavedSpeakers saved{};
bool saved_dirty = false;
std::array<std::uint8_t, kAudioSavedStorageBytes> saved_storage{};
std::array<bool, kAudioSavedLimit> saved_name_attempted{};
bd_addr_t saved_name_address{};
bool saved_name_pending = false;
AudioStatus current{};
AudioDiscovery found{};
AudioStatus previous{};
bool selection_pending = false, test_completed = false;
std::uint64_t discovery_deadline_us = 0, selection_deadline_us = 0;
std::array<std::uint8_t, kAudioDeviceLimit> page_modes{};
std::array<std::uint16_t, kAudioDeviceLimit> clock_offsets{};
std::array<bool, kAudioDeviceLimit> name_attempted{};
int name_index = -1;
bool initialised = false, ready = false, reconnect = true, pairing = false, scanning = false;
bool verifying_incoming = false;
bd_addr_t incoming_address{};
bd_addr_t volume_address{};
std::uint64_t incoming_deadline_us = 0;
bool raw_beep = false;
bool workout_running = false, beep_on = false, streaming = false, packet_pending = false;
bool volume_pending = false, volume_inflight = false, remote_volume = false;
std::uint16_t a2dp_cid = 0, avrcp_cid = 0;
std::uint8_t local_seid = 0;
std::uint32_t sample_rate = 44100, timestamp = 0;
SpeakerSignal speaker_signal{};
std::uint64_t next_connect_us = 0, next_volume_us = 0, save_after_us = 0, test_start_us = 0;
std::uint32_t last_audio_ms = 0, samples_due = 0, sample_fraction = 0;
std::uint16_t packet_bytes = 0;
std::uint8_t packet_frames = 0;
std::array<std::uint8_t, 1030> media{};
std::array<std::int16_t, 512> pcm{};
btstack_timer_source_t audio_timer{};
btstack_packet_callback_registration_t hci_registration{};
btstack_sbc_encoder_bluedroid_t encoder_state{};
const btstack_sbc_encoder_t* encoder = nullptr;
std::uint8_t codec_caps[] = {static_cast<std::uint8_t>((AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO), 0xff, 2, 53};
std::uint8_t codec_config[4]{};
std::uint8_t a2dp_record[200]{}, avrcp_record[200]{}, avrcp_target_record[200]{};
bool bluetooth_enabled() { return current.mode == AudioMode::Bluetooth || current.mode == AudioMode::Both; }
bool buzzer_enabled() { return current.mode == AudioMode::Buzzer || current.mode == AudioMode::Both; }
void dirty() {
    current.flags |= AudioSettingsPending;
    save_after_us = time_us_64() + 750000;
}
void remember_current() {
    AudioDevice device{};
    device.address = current.address; device.name = current.name;
    device.sound_confirmed = (current.flags & AudioSoundConfirmed) != 0;
    const auto* known = find_saved_speaker(saved, device.address);
    if (known && !known->name[0] && std::strcmp(device.name.data(), "Bluetooth speaker") == 0) device.name.fill(0);
    if (remember_speaker(saved, device)) saved_dirty = true;
}
void save_settings() {
    const btstack_tlv_t* tlv = nullptr;
    void* context = nullptr;
    btstack_tlv_get_instance(&tlv, &context);
    if (tlv == nullptr) return;
    std::uint8_t data[kSettingsBytes]{};
    data[0] = 2; data[1] = static_cast<std::uint8_t>(current.mode); data[2] = current.volume;
    data[3] = ((current.flags & AudioPaired) ? 1 : 0) | ((current.flags & AudioSoundConfirmed) ? 2 : 0);
    if (current.flags & AudioPaired) std::memcpy(data + 4, current.address.data(), 6);
    std::memcpy(data + 10, current.name.data(), current.name.size());
    bool saved_ok = true;
    if (saved_dirty) {
        const auto bytes = encode_saved_storage(saved, saved_storage.data());
        saved_ok = tlv->store_tag(context, kSavedSpeakersTag, saved_storage.data(), static_cast<std::uint32_t>(bytes)) == 0;
        if (saved_ok) saved_dirty = false;
    }
    if (saved_ok && tlv->store_tag(context, kSettingsTag, data, sizeof(data)) == 0) {
        current.flags &= static_cast<std::uint8_t>(~AudioSettingsPending);
    } else {
        save_after_us = time_us_64() + 5000000;
        std::puts("audio: settings save failed; retrying when idle");
    }
}
void load_saved_speakers(const btstack_tlv_t* tlv, void* context) {
    const auto bytes = tlv->get_tag(context, kSavedSpeakersTag, saved_storage.data(), saved_storage.size());
    const bool migrated = !decode_saved_storage(saved_storage.data(), bytes, saved);
    if (current.flags & AudioPaired) {
        const auto* known = find_saved_speaker(saved, current.address);
        if (known && known->sound_confirmed) current.flags |= AudioSoundConfirmed;
        remember_current();
    }
    if (migrated) {
        // Recover earlier associations from existing link keys during this upgrade.
        // Keys contain no name or audible-confirmation metadata; resolve names later.
        btstack_link_key_iterator_t iterator{};
        if (gap_link_key_iterator_init(&iterator)) {
            bd_addr_t address{}; link_key_t key{}; link_key_type_t type{};
            while (gap_link_key_iterator_get_next(&iterator, address, key, &type)) {
                AudioDevice device{}; std::memcpy(device.address.data(), address, 6);
                if (!find_saved_speaker(saved, device.address) && remember_speaker(saved, device)) saved_dirty = true;
            }
            gap_link_key_iterator_done(&iterator);
        }
    }
    if (saved_dirty) dirty();
}
void load_settings() {
    const btstack_tlv_t* tlv = nullptr;
    void* context = nullptr;
    btstack_tlv_get_instance(&tlv, &context);
    std::uint8_t data[kSettingsBytes]{};
    if (tlv == nullptr) return;
    if (tlv->get_tag(context, kSettingsTag, data, sizeof(data)) == sizeof(data) &&
        (data[0] == 1 || data[0] == 2) && data[1] <= 3 && data[2] <= 100 && data[3] <= (data[0] == 1 ? 1 : 3) && data[58] == 0) {
        current.mode = static_cast<AudioMode>(data[1]);
        current.volume = data[2];
        if ((data[3] & 1) && std::any_of(data + 4, data + 10, [](std::uint8_t byte) { return byte != 0; })) {
            current.flags |= AudioPaired;
            if (data[3] & 2) current.flags |= AudioSoundConfirmed;
        }
        std::memcpy(current.address.data(), data + 4, 6);
        std::memcpy(current.name.data(), data + 10, current.name.size());
    }
    load_saved_speakers(tlv, context);
}
void set_error(const std::uint8_t error) {
    current.last_error = error;
    current.connection = AudioConnection::Error;
    next_connect_us = time_us_64() + 5000000;
    std::printf("audio: Bluetooth error 0x%02x\n", error);
}
void connect_peer() {
    current.connection = AudioConnection::Connecting;
    current.last_error = 0;
    const auto result = a2dp_source_establish_stream(current.address.data(), &a2dp_cid);
    if (result != ERROR_CODE_SUCCESS) { a2dp_cid = 0; set_error(result); }
    else std::printf("audio: connecting to %s\n", bd_addr_to_str(current.address.data()));
}
void stop_stream() {
    streaming = false; packet_pending = false; packet_bytes = 0; packet_frames = 0;
    samples_due = 0;
    btstack_run_loop_remove_timer(&audio_timer);
    current.flags &= static_cast<std::uint8_t>(~AudioTestActive);
    test_completed = false;
}
void stop_discovery() {
    if (found.scanning && scanning) { gap_inquiry_stop(); scanning = false; }
    found.scanning = false;
    name_index = -1;
}
void resolve_discovered_name() {
    if (!found.scanning || scanning || name_index >= 0) return;
    for (std::size_t i = 0; i < found.count; ++i) {
        if (found.devices[i].name[0] || name_attempted[i]) continue;
        name_attempted[i] = true;
        if (gap_remote_name_request(found.devices[i].address.data(), page_modes[i], clock_offsets[i] | 0x8000) == ERROR_CODE_SUCCESS) {
            name_index = static_cast<int>(i);
            return;
        }
    }
    // One bounded inquiry, followed by remote names for devices lacking an EIR name.
    found.scanning = false;
}
void record_discovered_device(std::uint8_t* event) {
    const auto device_class = gap_event_inquiry_result_get_class_of_device(event);
    if (device_class && ((device_class >> 8) & 0x1f) != 4) return; // Audio/video class
    bd_addr_t address{};
    gap_event_inquiry_result_get_bd_addr(event, address);
    std::size_t index = 0;
    while (index < found.count && std::memcmp(address, found.devices[index].address.data(), 6) != 0) ++index;
    if (index == found.devices.size()) { found.full = true; return; }
    if (index == found.count) {
        ++found.count;
        std::memcpy(found.devices[index].address.data(), address, 6);
    }
    auto& device = found.devices[index];
    if (gap_event_inquiry_result_get_rssi_available(event)) device.rssi = gap_event_inquiry_result_get_rssi(event);
    page_modes[index] = gap_event_inquiry_result_get_page_scan_repetition_mode(event);
    clock_offsets[index] = gap_event_inquiry_result_get_clock_offset(event);
    if (gap_event_inquiry_result_get_name_available(event)) {
        const auto length = std::min<std::size_t>(gap_event_inquiry_result_get_name_len(event), kAudioNameBytes);
        std::memcpy(device.name.data(), gap_event_inquiry_result_get_name(event), length);
        device.name[length] = 0;
    }
}
void send_packet() {
    if (!streaming || !packet_pending || packet_frames == 0) return;
    media[0] = packet_frames;
    const auto result = a2dp_source_stream_send_media_payload_rtp(a2dp_cid, local_seid, 0, timestamp,
        media.data(), static_cast<std::uint16_t>(packet_bytes + 1));
    if (result != ERROR_CODE_SUCCESS) { set_error(result); stop_stream(); return; }
    timestamp += static_cast<std::uint32_t>(packet_frames) * encoder->num_audio_frames(&encoder_state);
    packet_frames = 0; packet_bytes = 0; packet_pending = false;
}
void audio_tick(btstack_timer_source_t* timer) {
    if (!streaming || encoder == nullptr) return;
    btstack_run_loop_set_timer(timer, 10);
    btstack_run_loop_add_timer(timer);
    const auto now = btstack_run_loop_get_time_ms();
    // Bound recovery after delayed foreground service; never encode an unbounded backlog.
    const auto elapsed = std::min<std::uint32_t>(now - last_audio_ms, 20);
    last_audio_ms = now;
    const auto sample_units = elapsed * sample_rate + sample_fraction;
    sample_fraction = sample_units % 1000;
    samples_due = std::min<std::uint32_t>(samples_due + sample_units / 1000, sample_rate / 50);
    if (packet_pending) return;
    const auto frame_samples = encoder->num_audio_frames(&encoder_state);
    auto frame_bytes = encoder->sbc_buffer_length(&encoder_state);
    // Bluedroid reports zero bytes until the first frame has actually been encoded.
    // Prime it with silence before checking the negotiated packet capacity.
    if (frame_bytes == 0 && frame_samples != 0 && frame_samples <= pcm.size() / 2) {
        pcm.fill(0);
        encoder->encode_signed_16(&encoder_state, pcm.data(), media.data() + 1);
        frame_bytes = encoder->sbc_buffer_length(&encoder_state);
    }
    const auto limit = std::min<int>(a2dp_max_media_payload_size(a2dp_cid, local_seid) - 1, static_cast<int>(media.size() - 1));
    if (frame_samples == 0 || frame_samples > pcm.size() / 2 || frame_bytes <= 0 || frame_bytes > limit) {
        std::printf("audio: invalid media shape frames=%u bytes=%u limit=%d seid=%u rate=%lu\n", frame_samples, frame_bytes, limit, local_seid, static_cast<unsigned long>(sample_rate));
        set_error(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE); stop_stream(); return;
    }
    while (samples_due >= frame_samples && packet_frames < 15 && packet_bytes + frame_bytes <= limit) {
        for (std::size_t sample = 0; sample < frame_samples; ++sample) {
            const auto value = speaker_signal.next(beep_on, current.volume, remote_volume);
            pcm[sample * 2] = value; pcm[sample * 2 + 1] = value;
        }
        encoder->encode_signed_16(&encoder_state, pcm.data(), media.data() + 1 + packet_bytes);
        packet_bytes = static_cast<std::uint16_t>(packet_bytes + frame_bytes);
        ++packet_frames;
        samples_due -= frame_samples;
    }
    // Keep A2DP flowing between cues; the PCM generator adds quiet maintenance pulses.
    if (packet_frames != 0) {
        packet_pending = true;
        a2dp_source_stream_endpoint_request_can_send_now(a2dp_cid, local_seid);
    }
}
void hci_event(std::uint8_t type, std::uint16_t, std::uint8_t* event, std::uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t address{};
    switch (hci_event_packet_get_type(event)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(event) == HCI_STATE_WORKING) {
            ready = true;
            load_settings();
            std::puts("audio: Bluetooth ready");
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        hci_event_pin_code_request_get_bd_addr(event, address);
        gap_pin_code_negative(address);
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(event, address);
        if (!workout_running && bluetooth_enabled() && (pairing || (current.flags & AudioPaired)) &&
            std::memcmp(address, current.address.data(), 6) == 0) gap_ssp_confirmation_response(address);
        else gap_ssp_confirmation_negative(address);
        break;
    case GAP_EVENT_INQUIRY_RESULT:
        if (found.scanning) { record_discovered_device(event); break; }
        if (scanning && gap_event_inquiry_result_get_name_available(event) &&
            gap_event_inquiry_result_get_name_len(event) == std::strlen(current.name.data()) &&
            std::memcmp(gap_event_inquiry_result_get_name(event), current.name.data(), std::strlen(current.name.data())) == 0) {
            gap_event_inquiry_result_get_bd_addr(event, address);
            std::memcpy(current.address.data(), address, 6);
            scanning = false;
            gap_inquiry_stop();
            connect_peer();
        }
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        scanning = false;
        if (found.scanning) { resolve_discovered_name(); break; }
        if (pairing && a2dp_cid == 0) next_connect_us = time_us_64() + 100000;
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        hci_event_remote_name_request_complete_get_bd_addr(event, address);
        if (saved_name_pending && std::memcmp(address, saved_name_address, 6) == 0) {
            saved_name_pending = false;
            std::array<std::uint8_t, 6> peer{}; std::memcpy(peer.data(), address, 6);
            const auto* known = find_saved_speaker(saved, peer);
            if (known && hci_event_remote_name_request_complete_get_status(event) == ERROR_CODE_SUCCESS) {
                AudioDevice device = *known;
                std::strncpy(device.name.data(), hci_event_remote_name_request_complete_get_remote_name(event), kAudioNameBytes);
                device.name[kAudioNameBytes] = 0;
                remember_speaker(saved, device); saved_dirty = true;
                if (current.address == peer) current.name = device.name;
                dirty();
            }
            break;
        }
        if (found.scanning && name_index >= 0 &&
            std::memcmp(address, found.devices[static_cast<std::size_t>(name_index)].address.data(), 6) == 0) {
            if (hci_event_remote_name_request_complete_get_status(event) == ERROR_CODE_SUCCESS) {
                auto& name = found.devices[static_cast<std::size_t>(name_index)].name;
                std::strncpy(name.data(), hci_event_remote_name_request_complete_get_remote_name(event), kAudioNameBytes);
                name[kAudioNameBytes] = 0;
            }
            name_index = -1; resolve_discovered_name(); break;
        }
        if (!verifying_incoming || std::memcmp(address, incoming_address, 6) != 0) break;
        verifying_incoming = false;
        if (pairing && reconnect && bluetooth_enabled() && !workout_running &&
            hci_event_remote_name_request_complete_get_status(event) == ERROR_CODE_SUCCESS &&
            std::strncmp(hci_event_remote_name_request_complete_get_remote_name(event), current.name.data(), current.name.size()) == 0) {
            std::memcpy(current.address.data(), address, 6);
            current.connection = AudioConnection::Connected;
            std::printf("audio: verified incoming speaker %s\n", bd_addr_to_str(address));
            if (avrcp_cid && std::memcmp(volume_address, address, 6) == 0) {
                volume_pending = true; next_volume_us = time_us_64() + 1000000;
            }
            if (a2dp_cid) {
                const auto result = a2dp_source_start_stream(a2dp_cid, local_seid);
                if (result != ERROR_CODE_SUCCESS) set_error(result);
            } else connect_peer();
        } else if (a2dp_cid) {
            std::puts("audio: incoming peer name did not match");
            a2dp_source_disconnect(a2dp_cid);
        }
        break;
    default: break;
    }
}
void a2dp_event(std::uint8_t type, std::uint16_t, std::uint8_t* event, std::uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_A2DP_META) return;
    switch (hci_event_a2dp_meta_get_subevent_code(event)) {
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        if (a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(event) != 2) {
            set_error(ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE); return;
        }
        sample_rate = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(event);
        encoder = btstack_sbc_encoder_bluedroid_init_instance(&encoder_state);
        encoder->configure(&encoder_state, SBC_MODE_STANDARD,
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(event),
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(event),
            static_cast<btstack_sbc_allocation_method_t>(a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(event) - 1),
            static_cast<std::uint16_t>(sample_rate), a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(event),
            SBC_CHANNEL_MODE_STEREO);
        break;
    }
    case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
        const auto error = a2dp_subevent_stream_established_get_status(event);
        if (error != ERROR_CODE_SUCCESS) { a2dp_cid = 0; set_error(error); break; }
        bd_addr_t address{};
        a2dp_subevent_stream_established_get_bd_addr(event, address);
        const auto event_cid = a2dp_subevent_stream_established_get_a2dp_cid(event);
        if (pairing && reconnect && bluetooth_enabled() && !workout_running && !verifying_incoming &&
            !std::any_of(current.address.begin(), current.address.end(), [](std::uint8_t byte) { return byte != 0; })) {
            std::memcpy(incoming_address, address, 6);
            a2dp_cid = event_cid;
            if (scanning) { gap_inquiry_stop(); scanning = false; }
            verifying_incoming = true;
            incoming_deadline_us = time_us_64() + 7000000;
            current.connection = AudioConnection::Connecting;
            std::printf("audio: verifying incoming peer %s\n", bd_addr_to_str(address));
            const auto result = gap_remote_name_request(address, 2, 0);
            if (result != ERROR_CODE_SUCCESS) {
                verifying_incoming = false;
                a2dp_source_disconnect(event_cid);
                set_error(static_cast<std::uint8_t>(result));
            }
            break;
        }
        if (!bluetooth_enabled() || !reconnect ||
            !std::any_of(current.address.begin(), current.address.end(), [](std::uint8_t byte) { return byte != 0; }) ||
            std::memcmp(address, current.address.data(), 6) != 0) {
            std::printf("audio: ignoring unsolicited peer %s\n", bd_addr_to_str(address));
            a2dp_source_disconnect(event_cid);
            break;
        }
        a2dp_cid = event_cid;
        current.connection = AudioConnection::Connected;
        const auto result = a2dp_source_start_stream(a2dp_cid, local_seid);
        if (result != ERROR_CODE_SUCCESS) set_error(result);
        break;
    }
    case A2DP_SUBEVENT_STREAM_STARTED:
        if (!bluetooth_enabled() || !reconnect || a2dp_cid == 0 ||
            a2dp_subevent_stream_started_get_a2dp_cid(event) != a2dp_cid ||
            !std::any_of(current.address.begin(), current.address.end(), [](std::uint8_t byte) { return byte != 0; })) {
            a2dp_source_disconnect(a2dp_subevent_stream_started_get_a2dp_cid(event)); break;
        }
        current.connection = AudioConnection::Streaming;
        current.last_error = 0;
        current.flags |= AudioPaired;
        remember_current();
        pairing = false;
        selection_pending = false;
        dirty();
        streaming = true; packet_pending = false; packet_bytes = 0; packet_frames = 0;
        timestamp = 0; samples_due = 0; sample_fraction = 0;
        speaker_signal.reset(sample_rate);
        last_audio_ms = btstack_run_loop_get_time_ms();
        btstack_run_loop_set_timer_handler(&audio_timer, audio_tick);
        btstack_run_loop_set_timer(&audio_timer, 10);
        btstack_run_loop_add_timer(&audio_timer);
        if (!avrcp_cid) avrcp_connect(current.address.data(), &avrcp_cid);
        std::puts("audio: speaker streaming");
        break;
    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW: send_packet(); break;
    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        stop_stream(); current.connection = AudioConnection::Connected; break;
    case A2DP_SUBEVENT_STREAM_RELEASED:
        stop_stream(); break;
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        if (a2dp_subevent_signaling_connection_released_get_a2dp_cid(event) != a2dp_cid) break;
        stop_stream(); a2dp_cid = 0; verifying_incoming = false;
        if (avrcp_cid) avrcp_disconnect(avrcp_cid);
        remote_volume = false; volume_inflight = false; volume_pending = false;
        current.flags &= static_cast<std::uint8_t>(~AudioVolumeConfirmed);
        current.actual_volume = 255;
        current.connection = bluetooth_enabled() ? AudioConnection::Idle : AudioConnection::Disabled;
        next_connect_us = time_us_64() + 5000000;
        break;
    default: break;
    }
}
void avrcp_event(std::uint8_t type, std::uint16_t, std::uint8_t* event, std::uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_AVRCP_META) return;
    switch (hci_event_avrcp_meta_get_subevent_code(event)) {
    case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
        if (avrcp_subevent_connection_established_get_status(event) == ERROR_CODE_SUCCESS) {
            bd_addr_t address{};
            avrcp_subevent_connection_established_get_bd_addr(event, address);
            std::memcpy(volume_address, address, 6);
            std::printf("audio: volume-control peer %s\n", bd_addr_to_str(address));
            avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(event);
            volume_pending = std::memcmp(address, current.address.data(), 6) == 0;
            next_volume_us = time_us_64() + 1000000;
            if (pairing && reconnect && !verifying_incoming && a2dp_cid == 0 &&
                !std::any_of(current.address.begin(), current.address.end(), [](std::uint8_t byte) { return byte != 0; })) {
                std::memcpy(incoming_address, address, 6);
                verifying_incoming = true;
                incoming_deadline_us = time_us_64() + 7000000;
                if (scanning) { gap_inquiry_stop(); scanning = false; }
                current.connection = AudioConnection::Connecting;
                if (gap_remote_name_request(address, 2, 0) != ERROR_CODE_SUCCESS) verifying_incoming = false;
            }
        } else avrcp_cid = 0;
        break;
    case AVRCP_SUBEVENT_CONNECTION_RELEASED:
        avrcp_cid = 0; remote_volume = false; volume_inflight = false;
        current.actual_volume = 255;
        current.flags &= static_cast<std::uint8_t>(~AudioVolumeConfirmed);
        break;
    case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
        if (!avrcp_cid || std::memcmp(volume_address, current.address.data(), 6) != 0) break;
        current.actual_volume = static_cast<std::uint8_t>((avrcp_subevent_notification_volume_changed_get_absolute_volume(event) * 100U + 63U) / 127U);
        if (remote_volume && !volume_pending && !volume_inflight && current.volume != current.actual_volume) {
            current.volume = current.actual_volume;
            dirty();
        }
        break;
    case AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE: {
        if (!avrcp_cid || std::memcmp(volume_address, current.address.data(), 6) != 0) break;
        volume_inflight = false;
        const auto command = avrcp_subevent_set_absolute_volume_response_get_command_type(event);
        if (command == AVRCP_CTYPE_RESPONSE_ACCEPTED) {
            remote_volume = true;
            const bool first_confirmation = !(current.flags & AudioVolumeConfirmed);
            current.flags |= AudioVolumeConfirmed;
            const auto raw = avrcp_subevent_set_absolute_volume_response_get_absolute_volume(event);
            current.actual_volume = static_cast<std::uint8_t>((raw * 100U + 63U) / 127U);
            const auto desired = static_cast<std::uint8_t>((current.volume * 127U + 50U) / 100U);
            volume_pending = raw != desired;
            next_volume_us = time_us_64() + 500000;
            std::printf("audio: speaker volume %u%%\n", current.actual_volume);
            if (first_confirmation) avrcp_controller_enable_notification(avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
        } else {
            volume_pending = false; remote_volume = false;
            current.flags &= static_cast<std::uint8_t>(~AudioVolumeConfirmed);
        }
        break;
    }
    case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
        if (avrcp_cid) avrcp_target_play_status(avrcp_cid, 0, 0,
            streaming ? AVRCP_PLAYBACK_STATUS_PLAYING : AVRCP_PLAYBACK_STATUS_STOPPED);
        break;
    default: break;
    }
}
} // namespace

void AudioOutput::initialise() {
    static_cast<void>(std::strcpy(current.name.data(), "MEGABOOM 3"));
    if (kAudioPin >= 0) current.flags |= AudioBuzzerAvailable;
    if (cyw43_arch_init() != 0) { current.connection = AudioConnection::Error; current.last_error = 255; return; }
    initialised = true;
    current.flags |= AudioSupported;
    l2cap_init(); sdp_init(); a2dp_source_init();
    a2dp_source_register_packet_handler(a2dp_event);
    auto* endpoint = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
        codec_caps, sizeof(codec_caps), codec_config, sizeof(codec_config));
    if (endpoint == nullptr) { current.flags &= static_cast<std::uint8_t>(~AudioSupported); set_error(255); return; }
    local_seid = avdtp_local_seid(endpoint);
    avdtp_set_preferred_sampling_frequency(endpoint, 44100);
    avdtp_source_register_delay_reporting_category(local_seid);
    a2dp_source_create_sdp_record(a2dp_record, sdp_create_service_record_handle(), AVDTP_SOURCE_FEATURE_MASK_PLAYER, nullptr, nullptr);
    sdp_register_service(a2dp_record);
    avrcp_init(); avrcp_register_packet_handler(avrcp_event);
    // BTstack dispatches both AVRCP directions on the same channel; initialise both.
    avrcp_target_init(); avrcp_target_register_packet_handler(avrcp_event);
    avrcp_controller_init(); avrcp_controller_register_packet_handler(avrcp_event);
    avrcp_target_create_sdp_record(avrcp_target_record, sdp_create_service_record_handle(), AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER, nullptr, nullptr);
    sdp_register_service(avrcp_target_record);
    avrcp_controller_create_sdp_record(avrcp_record, sdp_create_service_record_handle(), AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER, nullptr, nullptr);
    sdp_register_service(avrcp_record);
    hci_set_master_slave_policy(0);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_set_local_name("Rabbit speaker output");
    gap_discoverable_control(0);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(0);
    hci_registration.callback = hci_event;
    hci_add_event_handler(&hci_registration);
    hci_power_control(HCI_POWER_ON);
}
void AudioOutput::update(const core::Microseconds now_us, const bool beep, const bool running) {
    workout_running = running;
    raw_beep = beep;
    const auto now = static_cast<std::uint64_t>(now_us);
    bool test_beep = false;
    if (current.flags & AudioTestActive) {
        // UART can start a test after the caller captured this loop's timestamp.
        // Treat that first, slightly older timestamp as the start of the sequence.
        const auto elapsed = now >= test_start_us ? now - test_start_us : 0;
        if (elapsed >= 2150000 || running) {
            current.flags &= static_cast<std::uint8_t>(~AudioTestActive);
            test_completed = !running && streaming;
        }
        else test_beep = elapsed % 1000000 < 150000;
    }
    // The same gate drives both outputs; Bluetooth transports introduce speaker latency.
    beep_on = bluetooth_enabled() && (beep || test_beep);
    if (!initialised) return;
    cyw43_arch_poll();
    if (!ready) return;
    if (!running && !scanning && !found.scanning && !selection_pending && !verifying_incoming && !saved_name_pending) {
        for (std::size_t i = 0; i < saved.count; ++i) {
            if (saved.devices[i].name[0] || saved_name_attempted[i]) continue;
            if (gap_remote_name_request(saved.devices[i].address.data(), 2, 0) == ERROR_CODE_SUCCESS) {
                saved_name_attempted[i] = true; saved_name_pending = true;
                std::memcpy(saved_name_address, saved.devices[i].address.data(), 6);
            }
            break;
        }
    }
    if (found.scanning && (now >= discovery_deadline_us || running)) stop_discovery();
    if (selection_pending && now >= selection_deadline_us) {
        disconnect();
        set_error(ERROR_CODE_CONNECTION_TIMEOUT);
    }
    if (verifying_incoming && now >= incoming_deadline_us) {
        verifying_incoming = false;
        if (a2dp_cid) a2dp_source_disconnect(a2dp_cid);
        set_error(ERROR_CODE_CONNECTION_TIMEOUT);
    }
    if (bluetooth_enabled() && reconnect && !running && a2dp_cid == 0 && avrcp_cid == 0 && !scanning && !found.scanning && !verifying_incoming && now >= next_connect_us) {
        if (pairing && !std::any_of(current.address.begin(), current.address.end(), [](std::uint8_t b) { return b != 0; })) {
            const auto result = gap_inquiry_start(12);
            if (result == ERROR_CODE_SUCCESS) { scanning = true; current.connection = AudioConnection::Scanning; }
            else set_error(static_cast<std::uint8_t>(result));
        } else if (pairing || (current.flags & AudioPaired)) connect_peer();
    }
    if (avrcp_cid && volume_pending && now >= next_volume_us) {
        // An unanswered command is bounded too; allow retry after its timeout.
        if (volume_inflight) volume_inflight = false;
        const auto desired = static_cast<std::uint8_t>((current.volume * 127U + 50U) / 100U);
        const auto result = avrcp_controller_set_absolute_volume(avrcp_cid, desired);
        volume_inflight = result == ERROR_CODE_SUCCESS;
        next_volume_us = now + (volume_inflight ? 3000000 : 500000);
    }
    if (!running && !selection_pending && (current.flags & AudioSettingsPending) && now >= save_after_us) save_settings();
}
bool AudioOutput::buzzer_on() const {
    const auto elapsed = time_us_64() - test_start_us;
    return buzzer_enabled() && current.volume != 0 &&
        (raw_beep ||
         ((current.flags & AudioTestActive) && elapsed % 1000000 < 150000));
}
AudioStatus AudioOutput::status() const { return current; }
void AudioOutput::configure(const AudioMode mode, const std::uint8_t volume) {
    const bool was_enabled = bluetooth_enabled();
    if (current.mode != mode || current.volume != volume) dirty();
    current.mode = mode; current.volume = volume;
    if (avrcp_cid) { volume_pending = true; next_volume_us = time_us_64(); }
    if (!bluetooth_enabled()) {
        disconnect(); current.connection = AudioConnection::Disabled;
    } else if (!was_enabled) {
        reconnect = true; current.connection = AudioConnection::Idle; next_connect_us = 0;
    }
}
bool AudioOutput::pair(const char* name) {
    if (!ready || !bluetooth_enabled() || current.connection == AudioConnection::Connecting) return false;
    if (streaming && std::strcmp(name, current.name.data()) == 0) { reconnect = true; return true; }
    if ((current.flags & AudioPaired) && std::strcmp(name, current.name.data()) == 0) {
        disconnect(); reconnect = true; next_connect_us = 0; return true;
    }
    if (saved.count == kAudioSavedLimit) return false;
    const bool has_volume_peer = avrcp_cid != 0;
    disconnect();
    std::strncpy(current.name.data(), name, kAudioNameBytes);
    current.name[kAudioNameBytes] = '\0';
    current.address.fill(0);
    current.flags &= static_cast<std::uint8_t>(~(AudioPaired | AudioSoundConfirmed));
    pairing = true; reconnect = true; next_connect_us = 0;
    current.connection = AudioConnection::Scanning;
    if (has_volume_peer) {
        std::memcpy(incoming_address, volume_address, 6);
        verifying_incoming = true;
        incoming_deadline_us = time_us_64() + 7000000;
        current.connection = AudioConnection::Connecting;
        if (gap_remote_name_request(incoming_address, 2, 0) != ERROR_CODE_SUCCESS) verifying_incoming = false;
    }
    return true;
}
void AudioOutput::disconnect() {
    stop_discovery();
    reconnect = false; pairing = false;
    verifying_incoming = false;
    remote_volume = false; volume_pending = false; volume_inflight = false;
    current.actual_volume = 255;
    current.flags &= static_cast<std::uint8_t>(~AudioVolumeConfirmed);
    if (scanning) { gap_inquiry_stop(); scanning = false; }
    current.flags &= static_cast<std::uint8_t>(~AudioTestActive);
    stop_stream();
    if (a2dp_cid) a2dp_source_disconnect(a2dp_cid);
    if (avrcp_cid) avrcp_disconnect(avrcp_cid);
    if (selection_pending) {
        current = previous; selection_pending = false;
        current.actual_volume = 255;
        current.flags &= static_cast<std::uint8_t>(~(AudioVolumeConfirmed | AudioTestActive));
    }
    current.connection = bluetooth_enabled() ? AudioConnection::Idle : AudioConnection::Disabled;
}
bool AudioOutput::test() {
    if (current.mode == AudioMode::Off || current.volume == 0 ||
        (!streaming && !(buzzer_enabled() && kAudioPin >= 0))) return false;
    if (found.scanning || workout_running) return false;
    test_completed = false;
    test_start_us = time_us_64(); current.flags |= AudioTestActive; return true;
}
void AudioOutput::forget() {
    const auto address = selection_pending ? previous.address : current.address;
    forget_device(address);
}
const AudioSavedSpeakers& AudioOutput::saved_speakers() const { return saved; }
bool AudioOutput::forget_device(const std::array<std::uint8_t, 6>& address) {
    if (workout_running || !find_saved_speaker(saved, address)) return false;
    if (selection_pending && previous.address == address) disconnect();
    if (current.address == address) {
        disconnect();
        current.flags &= static_cast<std::uint8_t>(~(AudioPaired | AudioSoundConfirmed | AudioVolumeConfirmed));
        current.address.fill(0); current.name.fill(0); current.actual_volume = 255;
    }
    bd_addr_t peer{}; std::memcpy(peer, address.data(), 6);
    gap_drop_link_key_for_bd_addr(peer);
    remove_saved_speaker(saved, address); saved_name_attempted.fill(false);
    saved_dirty = true; dirty(); return true;
}
const AudioDiscovery& AudioOutput::discovery() const { return found; }
bool AudioOutput::scan() {
    if (!ready || workout_running || found.scanning || selection_pending || verifying_incoming || name_index >= 0 ||
        (current.flags & AudioTestActive)) return false;
    // An unavailable saved speaker must not monopolize setup through auto-reconnect.
    if (scanning || pairing || current.connection == AudioConnection::Connecting) disconnect();
    const auto result = gap_inquiry_start(16); // 20.48 seconds; user presses Scan when ready.
    if (result != ERROR_CODE_SUCCESS) return false;
    found = {}; found.scanning = true; scanning = true;
    name_attempted.fill(false);
    discovery_deadline_us = time_us_64() + 45000000;
    return true;
}
void AudioOutput::stop_scan() { stop_discovery(); }
bool AudioOutput::connect(const std::array<std::uint8_t, 6>& address, const AudioMode mode, const std::uint8_t volume) {
    if (!ready || workout_running || mode < AudioMode::Bluetooth || mode > AudioMode::Both || volume > 100 ||
        (current.flags & AudioTestActive) || selection_pending) return false;
    const AudioDevice* selected = nullptr;
    for (std::size_t i = 0; i < found.count; ++i) if (found.devices[i].address == address) selected = &found.devices[i];
    const auto* known = find_saved_speaker(saved, address);
    if (!selected && !known) return false; // Validate before any output/volume side effects.
    if (!known && saved.count == kAudioSavedLimit) return false;
    if (known && current.address == address && streaming) { configure(mode, volume); stop_scan(); reconnect = true; return true; }
    AudioStatus target = current;
    if (current.address != address || !(current.flags & AudioPaired)) {
        target.address = address;
        target.name = known ? known->name : selected->name;
        if (!target.name[0]) std::strcpy(target.name.data(), "Bluetooth speaker");
        target.flags &= static_cast<std::uint8_t>(~(AudioPaired | AudioSoundConfirmed));
        if (known) {
            target.flags |= AudioPaired;
            if (known->sound_confirmed) target.flags |= AudioSoundConfirmed;
        }
    }
    disconnect();
    previous = current;
    target.mode = mode; target.volume = volume; target.actual_volume = 255; target.last_error = 0;
    target.flags &= static_cast<std::uint8_t>(~(AudioVolumeConfirmed | AudioTestActive));
    target.connection = AudioConnection::Connecting;
    current = target;
    selection_pending = true; test_completed = false;
    selection_deadline_us = time_us_64() + 30000000;
    pairing = true; reconnect = true; next_connect_us = time_us_64() + 500000;
    return true;
}
bool AudioOutput::confirm() {
    if (!streaming || !bluetooth_enabled() || workout_running || found.scanning ||
        (current.flags & AudioTestActive) || !test_completed) return false;
    current.flags |= AudioSoundConfirmed;
    remember_current();
    dirty();
    return true;
}
} // namespace rabbit::rp2350
