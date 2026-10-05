#include "audio_api.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "cJSON.h"

namespace rabbit::esp32 {
namespace {
using namespace protocol;
AudioCall audio_call = nullptr;
constexpr const char* modes[] = {"off", "buzzer", "bluetooth", "both"};
constexpr const char* connections[] = {"disabled", "idle", "scanning", "connecting", "connected", "streaming", "error"};
esp_err_t error(httpd_req_t* request, const char* status, const char* json) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, json);
}
esp_err_t send_status(httpd_req_t* request, const Frame& frame) {
    AudioStatus value{};
    if (!decode_audio_status(frame, value)) return error(request, "502 Bad Gateway", "{\"error\":\"Invalid audio response\"}");
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    char address[18]{};
    std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", value.address[0], value.address[1],
                  value.address[2], value.address[3], value.address[4], value.address[5]);
    cJSON_AddStringToObject(root, "mode", modes[static_cast<unsigned>(value.mode)]);
    cJSON_AddNumberToObject(root, "volume", value.volume);
    cJSON_AddStringToObject(root, "connection", connections[static_cast<unsigned>(value.connection)]);
    cJSON_AddStringToObject(root, "name", value.name.data());
    cJSON_AddStringToObject(root, "address", address);
    cJSON_AddBoolToObject(root, "supported", value.flags & AudioSupported);
    cJSON_AddBoolToObject(root, "buzzerAvailable", value.flags & AudioBuzzerAvailable);
    cJSON_AddBoolToObject(root, "paired", value.flags & AudioPaired);
    cJSON_AddBoolToObject(root, "volumeConfirmed", value.flags & AudioVolumeConfirmed);
    cJSON_AddBoolToObject(root, "testActive", value.flags & AudioTestActive);
    cJSON_AddBoolToObject(root, "settingsPending", value.flags & AudioSettingsPending);
    cJSON_AddBoolToObject(root, "soundConfirmed", value.flags & AudioSoundConfirmed);
    cJSON_AddBoolToObject(root, "workoutRunning", value.flags & AudioWorkoutRunning);
    if (value.actual_volume == 255) cJSON_AddNullToObject(root, "actualVolume");
    else cJSON_AddNumberToObject(root, "actualVolume", value.actual_volume);
    cJSON_AddNumberToObject(root, "lastError", value.last_error);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return result;
}
esp_err_t send_devices(httpd_req_t* request, const Frame& frame) {
    AudioDevicePage page{};
    if (!decode_audio_devices(frame, page)) return error(request, "502 Bad Gateway", "{\"error\":\"Invalid discovery response\"}");
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "scanning", page.scanning);
    cJSON_AddBoolToObject(root, "full", page.full);
    cJSON_AddNumberToObject(root, "total", page.total);
    cJSON* devices = cJSON_AddArrayToObject(root, "devices");
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto& device = page.devices[i];
        cJSON* item = cJSON_CreateObject();
        char address[18]{};
        std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", device.address[0], device.address[1],
            device.address[2], device.address[3], device.address[4], device.address[5]);
        cJSON_AddStringToObject(item, "address", address);
        cJSON_AddStringToObject(item, "name", device.name.data());
        if (device.rssi) cJSON_AddNumberToObject(item, "rssi", device.rssi);
        else cJSON_AddNullToObject(item, "rssi");
        cJSON_AddItemToArray(devices, item);
    }
    if (page.offset + page.count < page.total) cJSON_AddNumberToObject(root, "next", page.offset + page.count);
    else cJSON_AddNullToObject(root, "next");
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return result;
}
esp_err_t send_saved(httpd_req_t* request, const Frame& frame) {
    AudioDevicePage page{};
    if (!decode_audio_saved(frame, page)) return error(request, "502 Bad Gateway", "{\"error\":\"Invalid saved-speaker response\"}");
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "total", page.total);
    cJSON_AddBoolToObject(root, "full", page.full);
    cJSON* devices = cJSON_AddArrayToObject(root, "devices");
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto& device = page.devices[i]; cJSON* item = cJSON_CreateObject(); char address[18]{};
        std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", device.address[0], device.address[1],
            device.address[2], device.address[3], device.address[4], device.address[5]);
        cJSON_AddStringToObject(item, "address", address); cJSON_AddStringToObject(item, "name", device.name.data());
        cJSON_AddBoolToObject(item, "soundConfirmed", device.sound_confirmed); cJSON_AddItemToArray(devices, item);
    }
    if (page.offset + page.count < page.total) cJSON_AddNumberToObject(root, "next", page.offset + page.count);
    else cJSON_AddNullToObject(root, "next");
    char* json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_sendstr(request, json); cJSON_free(json); return result;
}
esp_err_t dispatch(httpd_req_t* request, MessageType type, const std::uint8_t* payload = nullptr, std::uint16_t size = 0) {
    Frame response{};
    if (!audio_call(type, payload, size, response)) {
        if (response.type == MessageType::Reject && response.payload_size == 2) {
            const auto reason = static_cast<ServiceError>(response.payload[1]);
            if (reason == ServiceError::Payload) return error(request, "400 Bad Request", "{\"error\":\"Invalid audio settings\"}");
            if (reason == ServiceError::Command) return error(request, "409 Conflict", "{\"error\":\"Audio action unavailable. Stop the workout, select a scanned or saved speaker, and wait for any connection or test to finish.\"}");
            if (reason == ServiceError::Unsupported) return error(request, "501 Not Implemented", "{\"error\":\"Bluetooth audio is unavailable on this RP2350\"}");
        }
        return error(request, "503 Service Unavailable", "{\"error\":\"RP2350 audio is unreachable\"}");
    }
    if (type == MessageType::AudioSaved) return send_saved(request, response);
    return type == MessageType::AudioDevices ? send_devices(request, response) : send_status(request, response);
}
cJSON* read_json(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len > 256) return nullptr;
    char body[257]{};
    int received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) return nullptr;
        received += count;
    }
    return cJSON_ParseWithLengthOpts(body, static_cast<std::size_t>(received) + 1, nullptr, true);
}
esp_err_t status_handler(httpd_req_t* request) { return dispatch(request, MessageType::AudioStatus); }
esp_err_t config_handler(httpd_req_t* request) {
    cJSON* root = read_json(request);
    const auto* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const auto* volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    std::uint8_t mode_index = 4;
    if (cJSON_IsString(mode)) for (std::uint8_t i = 0; i < 4; ++i) if (std::strcmp(mode->valuestring, modes[i]) == 0) mode_index = i;
    const bool valid = cJSON_IsObject(root) && mode_index < 4 && cJSON_IsNumber(volume) &&
        std::isfinite(volume->valuedouble) && volume->valuedouble >= 0 && volume->valuedouble <= 100 &&
        std::floor(volume->valuedouble) == volume->valuedouble;
    std::uint8_t payload[2]{mode_index, valid ? static_cast<std::uint8_t>(volume->valuedouble) : std::uint8_t{0}};
    cJSON_Delete(root);
    if (!valid) return error(request, "400 Bad Request", "{\"error\":\"Use mode off, buzzer, bluetooth, or both and integer volume 0..100\"}");
    return dispatch(request, MessageType::AudioConfigure, payload, sizeof(payload));
}
esp_err_t pair_handler(httpd_req_t* request) {
    char name[kAudioNameBytes + 1] = "MEGABOOM 3";
    if (request->content_len != 0) {
        cJSON* root = read_json(request);
        const auto* item = cJSON_GetObjectItemCaseSensitive(root, "name");
        bool valid = cJSON_IsObject(root) && cJSON_IsString(item) &&
            std::strlen(item->valuestring) > 0 && std::strlen(item->valuestring) <= kAudioNameBytes;
        if (valid) {
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(item->valuestring); *p; ++p) {
                if (*p < 32 || *p == 127) valid = false;
            }
        }
        if (valid) std::strcpy(name, item->valuestring);
        cJSON_Delete(root);
        if (!valid) return error(request, "400 Bad Request", "{\"error\":\"Speaker name must be 1..48 bytes without control characters\"}");
    }
    return dispatch(request, MessageType::AudioPair, reinterpret_cast<const std::uint8_t*>(name), static_cast<std::uint16_t>(std::strlen(name)));
}
esp_err_t disconnect_handler(httpd_req_t* request) {
    if (request->content_len != 0) return error(request, "400 Bad Request", "{\"error\":\"Send an empty body\"}");
    return dispatch(request, MessageType::AudioDisconnect);
}
esp_err_t test_handler(httpd_req_t* request) {
    if (request->content_len != 0) return error(request, "400 Bad Request", "{\"error\":\"Send an empty body\"}");
    return dispatch(request, MessageType::AudioTest);
}
esp_err_t forget_handler(httpd_req_t* request) {
    if (request->content_len != 0) {
        cJSON* root = read_json(request); const auto* item = cJSON_GetObjectItemCaseSensitive(root, "address");
        std::uint8_t address[6]{};
        bool valid = cJSON_IsObject(root) && cJSON_IsString(item) && std::strlen(item->valuestring) == 17;
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (valid) for (std::size_t i = 0; i < 6; ++i) {
            const int high = hex(item->valuestring[i * 3]), low = hex(item->valuestring[i * 3 + 1]);
            if (high < 0 || low < 0 || (i < 5 && item->valuestring[i * 3 + 2] != ':')) { valid = false; break; }
            address[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
        cJSON_Delete(root);
        if (!valid) return error(request, "400 Bad Request", "{\"error\":\"Use a saved Bluetooth address\"}");
        return dispatch(request, MessageType::AudioForgetDevice, address, sizeof(address));
    }
    return dispatch(request, MessageType::AudioForget);
}
esp_err_t scan_handler(httpd_req_t* request) {
    if (request->content_len) return error(request, "400 Bad Request", "{\"error\":\"Send an empty body\"}");
    return dispatch(request, MessageType::AudioScan);
}
esp_err_t scan_stop_handler(httpd_req_t* request) {
    if (request->content_len) return error(request, "400 Bad Request", "{\"error\":\"Send an empty body\"}");
    return dispatch(request, MessageType::AudioScanStop);
}
esp_err_t devices_handler(httpd_req_t* request) {
    const bool saved = std::strncmp(request->uri, "/api/audio/saved", 16) == 0;
    std::uint8_t offset = 0;
    char query[160]{}, value[8]{};
    if (httpd_req_get_url_query_len(request) >= sizeof(query)) return error(request, "400 Bad Request", "{\"error\":\"Query too long\"}");
    const auto query_result = httpd_req_get_url_query_str(request, query, sizeof(query));
    const auto offset_result = query_result == ESP_OK ? httpd_query_key_value(query, "offset", value, sizeof(value)) : ESP_ERR_NOT_FOUND;
    if (offset_result == ESP_ERR_HTTPD_RESULT_TRUNC) return error(request, "400 Bad Request", "{\"error\":\"Invalid device offset\"}");
    if (offset_result == ESP_OK) {
        unsigned number = 0;
        if (!value[0]) return error(request, "400 Bad Request", "{\"error\":\"Invalid device offset\"}");
        for (const char* p = value; *p; ++p) {
            if (*p < '0' || *p > '9') return error(request, "400 Bad Request", "{\"error\":\"Invalid device offset\"}");
            number = number * 10 + static_cast<unsigned>(*p - '0');
        }
        if (number > (saved ? kAudioSavedLimit : kAudioDeviceLimit)) return error(request, "400 Bad Request", "{\"error\":\"Invalid device offset\"}");
        offset = static_cast<std::uint8_t>(number);
    }
    return dispatch(request, saved ? MessageType::AudioSaved : MessageType::AudioDevices, &offset, 1);
}
esp_err_t connect_handler(httpd_req_t* request) {
    cJSON* root = read_json(request);
    const auto* address = cJSON_GetObjectItemCaseSensitive(root, "address");
    const auto* mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const auto* volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    bool valid = cJSON_IsObject(root) && cJSON_IsString(address) && std::strlen(address->valuestring) == 17 &&
        cJSON_IsString(mode) && cJSON_IsNumber(volume) && std::isfinite(volume->valuedouble) &&
        volume->valuedouble >= 0 && volume->valuedouble <= 100 && std::floor(volume->valuedouble) == volume->valuedouble;
    std::uint8_t payload[8]{};
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (valid) {
        for (std::size_t i = 0; i < 6; ++i) {
            const int high = hex(address->valuestring[i * 3]), low = hex(address->valuestring[i * 3 + 1]);
            if (high < 0 || low < 0 || (i < 5 && address->valuestring[i * 3 + 2] != ':')) { valid = false; break; }
            payload[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
        if (std::strcmp(mode->valuestring, "bluetooth") == 0) payload[6] = 2;
        else if (std::strcmp(mode->valuestring, "both") == 0) payload[6] = 3;
        else valid = false;
        payload[7] = static_cast<std::uint8_t>(volume->valuedouble);
    }
    cJSON_Delete(root);
    if (!valid) return error(request, "400 Bad Request", "{\"error\":\"Select a speaker address, mode bluetooth or both, and integer volume 0..100\"}");
    return dispatch(request, MessageType::AudioConnect, payload, sizeof(payload));
}
esp_err_t confirm_handler(httpd_req_t* request) {
    if (request->content_len) return error(request, "400 Bad Request", "{\"error\":\"Send an empty body\"}");
    return dispatch(request, MessageType::AudioConfirm);
}
}
bool register_audio_api(httpd_handle_t server, AudioCall call) {
    audio_call = call;
    const struct { const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); } routes[] = {
        {"/api/audio/status", HTTP_GET, status_handler}, {"/api/audio/config", HTTP_POST, config_handler},
        {"/api/audio/pair", HTTP_POST, pair_handler}, {"/api/audio/disconnect", HTTP_POST, disconnect_handler},
        {"/api/audio/test", HTTP_POST, test_handler}, {"/api/audio/forget", HTTP_POST, forget_handler},
        {"/api/audio/scan", HTTP_POST, scan_handler}, {"/api/audio/scan-stop", HTTP_POST, scan_stop_handler},
        {"/api/audio/devices", HTTP_GET, devices_handler}, {"/api/audio/connect", HTTP_POST, connect_handler},
        {"/api/audio/confirm", HTTP_POST, confirm_handler}, {"/api/audio/saved", HTTP_GET, devices_handler}
    };
    for (const auto& route : routes) {
        httpd_uri_t entry{};
        entry.uri = route.uri; entry.method = route.method; entry.handler = route.handler;
        if (httpd_register_uri_handler(server, &entry) != ESP_OK) return false;
    }
    return true;
}
} // namespace rabbit::esp32
