#include <cinttypes>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" {
#include "driver/uart.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
}

#include "rabbit/protocol/framing.hpp"
#include "rabbit/protocol/plan_codec.hpp"
#include "rabbit/protocol/workout_service.hpp"
#include "deckscript_bridge.hpp"
#include "lcd_status.hpp"
#include "web_ui.hpp"
#include "audio_api.hpp"
#include "wifi_provision.hpp"

namespace {
constexpr uart_port_t kUart = UART_NUM_0;
constexpr int kUartTxPin = 16;
constexpr int kUartRxPin = 17;
constexpr int kUartRtsPin = UART_PIN_NO_CHANGE;
constexpr int kUartCtsPin = UART_PIN_NO_CHANGE;
constexpr int kHttpBodyLimit = 256;
constexpr int kMaxPlanBodyBytes = 12 * 1024;
constexpr std::int64_t kResponseTimeoutUs = 1'500'000;
// Keep the direct-connect AP as a fallback while testing the provisioned station.
constexpr bool kTestApOnly = false;
constexpr char kTestAccessPointPassword[] = "rabbit-test-2026";
constexpr char kTestAccessPointAddress[] = "192.168.4.1";
wifi_config_t wifi_configuration{};
char test_access_point_ssid[33]{};
bool wifi_scan_active = false;
bool wifi_scan_completed = false;
bool wifi_station_configured = false;

void configure_test_access_point() {
    std::uint8_t mac[6]{};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    const int ssid_length = std::snprintf(
        test_access_point_ssid, sizeof(test_access_point_ssid), "Rabbit-Test-%02X%02X",
        static_cast<unsigned int>(mac[4]), static_cast<unsigned int>(mac[5]));
    if (ssid_length <= 0 || static_cast<std::size_t>(ssid_length) >= sizeof(test_access_point_ssid)) std::abort();

    wifi_config_t access_point{};
    std::memcpy(access_point.ap.ssid, test_access_point_ssid, static_cast<std::size_t>(ssid_length));
    std::memcpy(access_point.ap.password, kTestAccessPointPassword, sizeof(kTestAccessPointPassword) - 1U);
    access_point.ap.ssid_len = static_cast<std::uint8_t>(ssid_length);
    access_point.ap.channel = 1;
    access_point.ap.max_connection = 1;
    access_point.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
    std::printf("Rabbit test AP ready: SSID=%s address=%s\n", test_access_point_ssid,
                kTestAccessPointAddress);
}

bool load_wifi_credentials(wifi_config_t& configuration) {
    nvs_handle_t storage{};
    if (nvs_open("rabbit_net", NVS_READONLY, &storage) != ESP_OK) return false;
    std::size_t ssid_size = sizeof(configuration.sta.ssid);
    std::size_t password_size = sizeof(configuration.sta.password);
    const auto ssid_result = nvs_get_str(storage, "ssid", reinterpret_cast<char*>(configuration.sta.ssid), &ssid_size);
    const auto password_result = nvs_get_str(storage, "password", reinterpret_cast<char*>(configuration.sta.password), &password_size);
    nvs_close(storage);
    return ssid_result == ESP_OK && password_result == ESP_OK;
}

void scan_configured_network() {
    if (wifi_scan_active) return;
    wifi_scan_config_t scan{};
    scan.show_hidden = true;
    if (esp_wifi_scan_start(&scan, false) == ESP_OK) wifi_scan_active = true;
}

void report_configured_network() {
    std::array<wifi_ap_record_t, 16> records{};
    std::uint16_t count = static_cast<std::uint16_t>(records.size());
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        std::puts("Rabbit Wi-Fi scan failed");
        return;
    }
    for (std::uint16_t index = 0; index < count; ++index) {
        if (std::strcmp(reinterpret_cast<const char*>(records[index].ssid),
                        reinterpret_cast<const char*>(wifi_configuration.sta.ssid)) == 0) {
            std::printf("Rabbit Wi-Fi SSID visible: channel=%u rssi=%d\n",
                        static_cast<unsigned int>(records[index].primary), static_cast<int>(records[index].rssi));
            return;
        }
    }
    std::puts("Rabbit Wi-Fi SSID not visible in C6 scan");
}

void wifi_event_handler(void*, const esp_event_base_t event_base, const std::int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const auto* event = static_cast<const wifi_event_ap_staconnected_t*>(event_data);
        std::printf("Rabbit test AP client connected: aid=%u\n", static_cast<unsigned int>(event->aid));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const auto* event = static_cast<const wifi_event_ap_stadisconnected_t*>(event_data);
        std::printf("Rabbit test AP client disconnected: aid=%u\n", static_cast<unsigned int>(event->aid));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!wifi_station_configured) return;
        const auto result = esp_wifi_connect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
            std::printf("Rabbit Wi-Fi connect request failed: %ld\n", static_cast<long>(result));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!wifi_station_configured) return;
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
        std::printf("Rabbit Wi-Fi disconnected: reason=%u\n", static_cast<unsigned int>(event->reason));
        rabbit::esp32::show_lcd_status("RECONNECTING", kTestAccessPointAddress);
        if (event->reason == WIFI_REASON_NO_AP_FOUND && !wifi_scan_completed) {
            wifi_scan_completed = true;
            scan_configured_network();
            return;
        }
        const auto result = esp_wifi_connect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
            std::printf("Rabbit Wi-Fi reconnect failed: %ld\n", static_cast<long>(result));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        std::printf("Rabbit Wi-Fi connected: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        char address[16]{};
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
        rabbit::esp32::show_lcd_status("BEAVER", address);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_active = false;
        report_configured_network();
        static_cast<void>(esp_wifi_connect());
    }
}

void start_wifi() {
    auto* const ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == nullptr || (!kTestApOnly && esp_netif_create_default_wifi_sta() == nullptr)) {
        std::puts("Rabbit Wi-Fi network interfaces could not be created");
        return;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    if (!kTestApOnly) {
        // The browser is interactive; avoid DTIM-delayed packets on the STA link.
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(kTestApOnly ? WIFI_MODE_AP : WIFI_MODE_APSTA));
    configure_test_access_point();
    if (!kTestApOnly) {
        wifi_station_configured = load_wifi_credentials(wifi_configuration);
        if (wifi_station_configured) {
            std::printf("Rabbit Wi-Fi credentials loaded for SSID=%s\n", wifi_configuration.sta.ssid);
            rabbit::esp32::show_lcd_status("CONNECTING", kTestAccessPointAddress);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
        } else {
            rabbit::esp32::show_lcd_status("SETUP WIFI", kTestAccessPointAddress);
            std::puts("Rabbit Wi-Fi credentials are not provisioned; use the local test AP");
        }
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (kTestApOnly) {
        esp_netif_ip_info_t ip_info{};
        ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info));
        char address[16]{};
        std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.ip));
        rabbit::esp32::show_lcd_status("RABBIT AP", address);
        std::printf("Rabbit AP-only test mode: http://%s/; saved Wi-Fi credentials unchanged\n", address);
    }
    if (wifi_station_configured) std::puts("Rabbit Wi-Fi connecting");
}

void write_u32(std::uint8_t* bytes, const std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void write_u16(std::uint8_t* bytes, const std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

class UartClient {
public:
    void initialise() {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) std::abort();
        uart_config_t config{};
        config.baud_rate = 115200;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.rx_flow_ctrl_thresh = 64;
        config.source_clk = UART_SCLK_DEFAULT;
        ESP_ERROR_CHECK(uart_driver_install(kUart, 1024, 1024, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(kUart, &config));
        ESP_ERROR_CHECK(uart_set_pin(kUart, kUartTxPin, kUartRxPin, kUartRtsPin, kUartCtsPin));
        session_id_ = esp_random() | 1U;
    }

    [[nodiscard]] bool prepare(const std::uint32_t distance_mm, const std::uint32_t target_ms,
                               const std::uint32_t interval_ms, const std::uint8_t flags,
                               const std::uint16_t repetitions = 1, const std::uint32_t final_target_ms = 0,
                               const std::uint16_t surge_permille = 0) {
        std::uint8_t payload[rabbit::protocol::kPrepareSetPayloadSize]{};
        payload[0] = flags;
        write_u32(payload + 1, distance_mm);
        write_u32(payload + 5, target_ms);
        write_u32(payload + 9, interval_ms);
        write_u16(payload + 13, repetitions);
        write_u32(payload + 15, final_target_ms == 0 ? target_ms : final_target_ms);
        write_u16(payload + 19, surge_permille);
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::PrepareBegin, payload, sizeof(payload), response);
    }

    [[nodiscard]] bool prepare_plan(const std::uint8_t* data, const std::uint16_t size) {
        if (data == nullptr || size == 0U || size > rabbit::protocol::kMaxPlanBytes) return false;
        const auto transfer_id = esp_random() | 1U;
        std::uint8_t begin[10]{};
        write_u32(begin, transfer_id);
        write_u16(begin + 4, size);
        write_u32(begin + 6, rabbit::protocol::crc32(data, size));
        rabbit::protocol::Frame response{};
        if (!call(rabbit::protocol::MessageType::PrepareBegin, begin, sizeof(begin), response)) return false;
        for (std::uint16_t offset = 0; offset < size;) {
            std::uint8_t chunk[246]{};
            const auto count = static_cast<std::uint16_t>(std::min<std::uint16_t>(240U, size - offset));
            write_u32(chunk, transfer_id);
            write_u16(chunk + 4, offset);
            std::memcpy(chunk + 6, data + offset, count);
            if (!call(rabbit::protocol::MessageType::PrepareChunk, chunk,
                      static_cast<std::uint16_t>(count + 6U), response)) return false;
            offset = static_cast<std::uint16_t>(offset + count);
        }
        std::uint8_t commit[4]{};
        write_u32(commit, transfer_id);
        return call(rabbit::protocol::MessageType::PrepareCommit, commit, sizeof(commit), response);
    }

    [[nodiscard]] bool start() {
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::Start, nullptr, 0, response);
    }

    [[nodiscard]] bool stop() {
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::Stop, nullptr, 0, response);
    }

    [[nodiscard]] bool cancel() {
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::Cancel, nullptr, 0, response);
    }

    [[nodiscard]] bool status(rabbit::protocol::Frame& response) {
        return call(rabbit::protocol::MessageType::Status, nullptr, 0, response);
    }

    [[nodiscard]] bool audio(rabbit::protocol::MessageType type, const std::uint8_t* payload,
                             std::uint16_t size, rabbit::protocol::Frame& response) {
        return call(type, payload, size, response);
    }

private:
    [[nodiscard]] bool call(const rabbit::protocol::MessageType type, const std::uint8_t* payload,
                            const std::uint16_t payload_size, rabbit::protocol::Frame& response) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        const bool result = call_locked(type, payload, payload_size, response);
        xSemaphoreGive(mutex_);
        return result;
    }

    [[nodiscard]] bool call_locked(const rabbit::protocol::MessageType type, const std::uint8_t* payload,
                                   const std::uint16_t payload_size, rabbit::protocol::Frame& response) {
        const auto request_id = ++request_id_;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!session_ready_ && !hello()) continue;
            rabbit::protocol::Frame request{};
            request.type = type;
            request.session_id = session_id_;
            request.request_id = request_id;
            request.payload_size = payload_size;
            if (payload_size != 0U) std::memcpy(request.payload.data(), payload, payload_size);
            if (!send(request) || !receive(request_id, response)) {
                session_ready_ = false;
                continue;
            }
            if (response.type == rabbit::protocol::MessageType::Reject) {
                if (response.payload_size >= 2U &&
                    response.payload[1] == static_cast<std::uint8_t>(rabbit::protocol::ServiceError::Session)) {
                    session_ready_ = false;
                    continue;
                }
                return false;
            }
            if (rabbit::protocol::is_audio_request(type)) return response.type ==
                ((type == rabbit::protocol::MessageType::AudioDevices || type == rabbit::protocol::MessageType::AudioSaved) ? type : rabbit::protocol::MessageType::AudioStatus);
            return type == rabbit::protocol::MessageType::Status ? response.type == rabbit::protocol::MessageType::Status :
                response.type == rabbit::protocol::MessageType::Ack;
        }
        return false;
    }

    [[nodiscard]] bool hello() {
        rabbit::protocol::Frame request{};
        rabbit::protocol::Frame response{};
        request.type = rabbit::protocol::MessageType::Hello;
        request.session_id = session_id_;
        request.request_id = ++request_id_;
        if (!send(request) || !receive(request.request_id, response) || response.type != rabbit::protocol::MessageType::Ack) {
            return false;
        }
        session_ready_ = true;
        return true;
    }

    [[nodiscard]] bool send(const rabbit::protocol::Frame& request) {
        rabbit::protocol::EncodedFrame encoded{};
        if (!rabbit::protocol::encode(request, encoded)) return false;
        const int written = uart_write_bytes(kUart, reinterpret_cast<const char*>(encoded.bytes.data()), encoded.size);
        const bool transmitted = written == static_cast<int>(encoded.size) &&
            uart_wait_tx_done(kUart, pdMS_TO_TICKS(100)) == ESP_OK;
        std::printf("Rabbit UART tx type=%u bytes=%u written=%d complete=%s\n",
                    static_cast<unsigned int>(request.type), static_cast<unsigned int>(encoded.size), written,
                    transmitted ? "yes" : "no");
        return transmitted;
    }

    [[nodiscard]] bool receive(const std::uint32_t expected_request_id, rabbit::protocol::Frame& response) {
        const auto deadline = esp_timer_get_time() + kResponseTimeoutUs;
        while (esp_timer_get_time() < deadline) {
            std::uint8_t bytes[64]{};
            const int count = uart_read_bytes(kUart, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
            for (int index = 0; index < count; ++index) {
                rabbit::protocol::Frame decoded{};
                rabbit::protocol::DecodeError error{};
                if (decoder_.push(bytes[index], decoded, error) && decoded.session_id == session_id_ &&
                    decoded.request_id == expected_request_id) {
                    response = decoded;
                    return true;
                }
            }
        }
        return false;
    }

    rabbit::protocol::FrameDecoder decoder_{};
    std::uint32_t session_id_{1};
    std::uint32_t request_id_{0};
    bool session_ready_{false};
    SemaphoreHandle_t mutex_{nullptr};
};

UartClient transport{};

const char* value_for(const char* body, const char* key) {
    char quoted_key[48]{};
    const int written = std::snprintf(quoted_key, sizeof(quoted_key), "\"%s\"", key);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(quoted_key)) return nullptr;
    const char* value = std::strstr(body, quoted_key);
    if (value == nullptr) return nullptr;
    value = std::strchr(value + written, ':');
    return value == nullptr ? nullptr : value + 1;
}

bool json_u32(const char* body, const char* key, const std::uint32_t fallback, std::uint32_t& result) {
    const char* value = value_for(body, key);
    if (value == nullptr) {
        result = fallback;
        return true;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || parsed > UINT32_MAX) return false;
    result = static_cast<std::uint32_t>(parsed);
    return true;
}

bool json_true(const char* body, const char* key) {
    const char* value = value_for(body, key);
    return value != nullptr && std::strncmp(value, "true", 4) == 0;
}

int hex_digit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool query_value(const char* query, const char* key, char* output, const std::size_t capacity) {
    if (httpd_query_key_value(query, key, output, capacity) != ESP_OK) return false;
    std::size_t read = 0;
    std::size_t write = 0;
    while (output[read] != '\0') {
        if (output[read] == '%' && output[read + 1] != '\0' && output[read + 2] != '\0') {
            const int high = hex_digit(output[read + 1]);
            const int low = hex_digit(output[read + 2]);
            if (high < 0 || low < 0) return false;
            output[write++] = static_cast<char>((high << 4) | low);
            read += 3;
        } else {
            output[write++] = output[read] == '+' ? ' ' : output[read];
            ++read;
        }
    }
    output[write] = '\0';
    return true;
}

bool parse_positive_u32(const char* text, std::uint32_t& output) {
    if (text[0] < '0' || text[0] > '9') return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > UINT32_MAX) return false;
    output = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_time_ms(const char* text, std::uint32_t& output) {
    const char* seconds = std::strchr(text, ':');
    std::uint32_t minutes = 0;
    if (seconds != nullptr) {
        char minutes_text[12]{};
        const auto length = static_cast<std::size_t>(seconds - text);
        if (length == 0 || length >= sizeof(minutes_text)) return false;
        std::memcpy(minutes_text, text, length);
        if (!parse_positive_u32(minutes_text, minutes) && std::strcmp(minutes_text, "0") != 0) return false;
        ++seconds;
    } else {
        seconds = text;
    }
    if (*seconds < '0' || *seconds > '9') return false;
    char* end = nullptr;
    const double parsed_seconds = std::strtod(seconds, &end);
    if (*end != '\0' || !std::isfinite(parsed_seconds) || parsed_seconds < 0.0 ||
        (std::strchr(text, ':') != nullptr && parsed_seconds >= 60.0)) return false;
    const double total_ms = (static_cast<double>(minutes) * 60.0 + parsed_seconds) * 1000.0;
    if (total_ms < 1.0 || total_ms > 86'400'000.0) return false;
    output = static_cast<std::uint32_t>(std::lround(total_ms));
    return true;
}

esp_err_t send_error(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, message);
}

esp_err_t status_handler(httpd_req_t* request) {
    rabbit::protocol::Frame response{};
    if (!transport.status(response) || response.payload_size < 28U) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 transport unavailable\"}");
    }
    const auto state = response.payload[0];
    const auto running = (response.payload[1] & 0x01U) != 0U;
    const auto continuous = (response.payload[1] & 0x02U) != 0U;
    const auto deckscript = (response.payload[1] & 0x04U) != 0U;
    const auto metres = (response.payload[1] & 0x08U) != 0U;
    const auto entry = read_u16(response.payload.data() + 2);
    const auto repetitions = read_u16(response.payload.data() + 18);
    const auto distance_mm = read_u32(response.payload.data() + 20);
    const auto target_ms = read_u32(response.payload.data() + 24);
    const auto distance = metres ? (distance_mm + 500U) / 1000U :
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(distance_mm) * 10U + 4572U) / 9144U);
    const auto remaining_seconds = (read_u32(response.payload.data() + 10) + 999U) / 1000U;
    std::uint16_t swim_index = 0;
    std::uint16_t swim_count = 0;
    std::uint8_t kind = 0xffU;
    char plan_name[65]{};
    char entry_label[81]{};
    if (deckscript) {
        if (response.payload_size < 46U) {
            return send_error(request, "503 Service Unavailable", "{\"error\":\"Incomplete DeckScript status\"}");
        }
        kind = response.payload[28];
        swim_index = read_u16(response.payload.data() + 40);
        swim_count = read_u16(response.payload.data() + 42);
        std::size_t offset = 44;
        const auto name_size = response.payload[offset++];
        if (name_size == 0U || name_size >= sizeof(plan_name) || offset + name_size >= response.payload_size) {
            return send_error(request, "503 Service Unavailable", "{\"error\":\"Invalid DeckScript status name\"}");
        }
        std::memcpy(plan_name, response.payload.data() + offset, name_size);
        offset += name_size;
        const auto label_size = response.payload[offset++];
        if (label_size >= sizeof(entry_label) || offset + label_size != response.payload_size) {
            return send_error(request, "503 Service Unavailable", "{\"error\":\"Invalid DeckScript status label\"}");
        }
        std::memcpy(entry_label, response.payload.data() + offset, label_size);
    }
    const auto current_rep = deckscript ? swim_index : continuous ? read_u32(response.payload.data() + 14) :
        repetitions == 0U ? 0U : static_cast<std::uint32_t>(entry < repetitions ? entry + 1U : repetitions);
    char countdown[24]{};
    std::snprintf(countdown, sizeof(countdown), "%" PRIu32 ":%02" PRIu32,
                  remaining_seconds / 60U, remaining_seconds % 60U);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(cJSON_CreateObject(), &cJSON_Delete);
    if (!body) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    if (cJSON_AddBoolToObject(body.get(), "running", running) == nullptr ||
        cJSON_AddBoolToObject(body.get(), "stopped", state == 5U) == nullptr ||
        cJSON_AddBoolToObject(body.get(), "prepped", state == 1U || state == 5U) == nullptr ||
        cJSON_AddBoolToObject(body.get(), "complete", state == 6U) == nullptr ||
        cJSON_AddStringToObject(body.get(), "mode", deckscript ? "workout" : continuous ? "sprint" : "pace") == nullptr ||
        cJSON_AddNumberToObject(body.get(), "entry", entry) == nullptr ||
        cJSON_AddNumberToObject(body.get(), "progressPermille", read_u16(response.payload.data() + 4)) == nullptr ||
        cJSON_AddNumberToObject(body.get(), "sequence", read_u32(response.payload.data() + 6)) == nullptr) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    }
    cJSON* const details = cJSON_AddObjectToObject(body.get(), "setDetails");
    if (details == nullptr) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    if (cJSON_AddNumberToObject(details, "distance", distance) == nullptr ||
        cJSON_AddNumberToObject(details, "repetitions", continuous ? 0U : deckscript ? swim_count : repetitions) == nullptr ||
        cJSON_AddNumberToObject(details, "currentRep", current_rep) == nullptr ||
        cJSON_AddNumberToObject(details, "totalTargetDurationSeconds", static_cast<double>(target_ms) / 1000.0) == nullptr ||
        cJSON_AddStringToObject(details, "timeUntilNextRepText", countdown) == nullptr) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    }
    if (deckscript) {
        cJSON* const workout = cJSON_AddObjectToObject(body.get(), "workout");
        if (workout == nullptr) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
        if (cJSON_AddStringToObject(workout, "name", plan_name) == nullptr ||
            cJSON_AddNumberToObject(workout, "entryIndex", entry) == nullptr ||
            cJSON_AddNumberToObject(workout, "entryCount", repetitions) == nullptr ||
            cJSON_AddBoolToObject(workout, "continuous", continuous) == nullptr) {
            return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
        }
        if (kind <= 2U) {
            cJSON* const item = cJSON_AddObjectToObject(workout, "entry");
            if (item == nullptr) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
            if (cJSON_AddStringToObject(item, "label", entry_label) == nullptr) {
                return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
            }
            if (kind == 0U) {
                if (cJSON_AddStringToObject(item, "kind", "swim") == nullptr ||
                    cJSON_AddNumberToObject(item, "distance", distance) == nullptr ||
                    cJSON_AddNumberToObject(item, "targetSeconds", static_cast<double>(target_ms) / 1000.0) == nullptr ||
                    cJSON_AddNumberToObject(item, "intervalSeconds",
                                            static_cast<double>(read_u32(response.payload.data() + 30)) / 1000.0) == nullptr ||
                    cJSON_AddNumberToObject(item, "swimIndex", swim_index) == nullptr ||
                    cJSON_AddNumberToObject(item, "swimCount", continuous ? 0U : swim_count) == nullptr) {
                    return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
                }
                const auto strategy = response.payload[29];
                if (cJSON_AddStringToObject(item, "strategy", strategy == 1U ? "negativeSplit" :
                                            strategy == 2U ? "surge" : "even") == nullptr ||
                    cJSON_AddNumberToObject(item, "splitDeltaSeconds",
                                            static_cast<double>(read_u32(response.payload.data() + 34)) / 1000.0) == nullptr ||
                    cJSON_AddNumberToObject(item, "variation",
                                            static_cast<double>(read_u16(response.payload.data() + 38)) / 1000.0) == nullptr) {
                    return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
                }
            } else if (kind == 1U) {
                if (cJSON_AddStringToObject(item, "kind", "rest") == nullptr ||
                    cJSON_AddNumberToObject(item, "seconds", static_cast<double>(target_ms) / 1000.0) == nullptr) {
                    return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
                }
            } else {
                if (cJSON_AddStringToObject(item, "kind", "activity") == nullptr ||
                    cJSON_AddStringToObject(item, "activity", entry_label) == nullptr) {
                    return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
                }
            }
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> serialized(cJSON_PrintUnformatted(body.get()), &cJSON_free);
    if (!serialized) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, serialized.get());
}

void lcd_status_task(void*) {
    for (;;) {
        rabbit::protocol::Frame response{};
        rabbit::esp32::LcdWorkoutStatus display{};
        if (transport.status(response) && response.payload_size >= 28U) {
            display.state = response.payload[0];
            display.entry_index = read_u16(response.payload.data() + 2);
            display.entry_count = read_u16(response.payload.data() + 18);
            const auto distance_mm = read_u32(response.payload.data() + 20);
            display.metres = (response.payload[1] & 0x08U) != 0U;
            display.distance_yards = display.metres ? (distance_mm + 500U) / 1000U :
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(distance_mm) * 10U + 4572U) / 9144U);
            display.remaining_ms = read_u32(response.payload.data() + 10);
            display.progress_permille = read_u16(response.payload.data() + 4);
            display.cycle = read_u32(response.payload.data() + 14);
            display.continuous = (response.payload[1] & 0x02U) != 0U;
            display.deckscript = (response.payload[1] & 0x04U) != 0U;
            if (display.deckscript && response.payload_size >= 44U) {
                display.entry_kind = response.payload[28];
                display.swim_index = read_u16(response.payload.data() + 40);
                display.swim_count = read_u16(response.payload.data() + 42);
            }
        } else {
            display.state = 8U;
        }
        rabbit::esp32::show_lcd_workout(display);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t prepare_handler(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len > kMaxPlanBodyBytes) {
        return send_error(request, "413 Payload Too Large", "{\"error\":\"Workout payload exceeds 12 KiB\"}");
    }
    const auto body_size = static_cast<std::size_t>(request->content_len);
    std::unique_ptr<char, decltype(&std::free)> body(static_cast<char*>(std::malloc(body_size + 1U)), &std::free);
    if (!body) return send_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    std::size_t received = 0;
    while (received < body_size) {
        const int count = httpd_req_recv(request, body.get() + received, body_size - received);
        if (count <= 0) return send_error(request, "400 Bad Request", "{\"error\":\"Incomplete workout payload\"}");
        received += static_cast<std::size_t>(count);
    }
    body.get()[body_size] = '\0';
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(body.get(), body_size), &cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Invalid workout JSON\"}");
    }
    const cJSON* const packed = cJSON_GetObjectItemCaseSensitive(root.get(), "plan");
    if (packed != nullptr) {
        static rabbit::core::WorkoutPlan plan{};
        static std::array<std::uint8_t, rabbit::protocol::kMaxPlanBytes> encoded{};
        rabbit::core::PoolProfile pool{};
        rabbit::core::clear_workout_plan(plan);
        const char* error = nullptr;
        if (!rabbit::esp32::parse_deckscript_plan(packed, plan, pool, error)) {
            std::printf("Rabbit DeckScript rejected: %s\n", error == nullptr ? "invalid plan" : error);
            char message[160]{};
            std::snprintf(message, sizeof(message), "{\"error\":\"%s\"}",
                          error == nullptr ? "Invalid DeckScript plan" : error);
            return send_error(request, "422 Unprocessable Entity", message);
        }
        std::uint16_t size = 0;
        if (!rabbit::protocol::encode_plan(plan, pool, encoded.data(),
                                            static_cast<std::uint16_t>(encoded.size()), size)) {
            return send_error(request, "413 Payload Too Large", "{\"error\":\"Compiled plan exceeds transfer limit\"}");
        }
        root.reset();
        body.reset();
        if (!transport.prepare_plan(encoded.data(), size)) {
            return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected DeckScript plan\"}");
        }
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"ok\":true}");
    }
    if (body_size > kHttpBodyLimit) {
        return send_error(request, "413 Payload Too Large", "{\"error\":\"Simple workout payload exceeds 256 bytes\"}");
    }
    std::uint32_t distance_mm = 22'860;
    std::uint32_t target_ms = 30'000;
    std::uint32_t interval_ms = 30'000;
    if (!json_u32(body.get(), "distanceMm", distance_mm, distance_mm) ||
        !json_u32(body.get(), "targetMs", target_ms, target_ms) ||
        !json_u32(body.get(), "intervalMs", interval_ms, interval_ms)) {
        return send_error(request, "400 Bad Request", "{\"error\":\"invalid workout payload\"}");
    }
    std::uint8_t flags = 0;
    if (std::strstr(body.get(), "far-to-near") != nullptr) flags |= rabbit::protocol::kPrepareFlagFarToNear;
    if (json_true(body.get(), "audio")) flags |= rabbit::protocol::kPrepareFlagAudio;
    if (json_true(body.get(), "waitForInterval")) flags |= rabbit::protocol::kPrepareFlagWaitForInterval;
    if (!transport.prepare(distance_mm, target_ms, interval_ms, flags)) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected prepare\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t legacy_prepare_handler(httpd_req_t* request) {
    const auto query_size = httpd_req_get_url_query_len(request);
    if (query_size == 0 || query_size >= 768U) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Invalid set query\"}");
    }
    char query[768]{};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Invalid set query\"}");
    }
    char mode[16]{};
    char pool[80]{};
    char direction[12]{};
    char audio[12]{};
    char duration[32]{};
    char interval[32]{};
    char distance_text[16]{};
    char repetitions_text[16]{};
    char strategy[24]{};
    char variation[32]{};
    if (!query_value(query, "mode", mode, sizeof(mode)) ||
        (std::strcmp(mode, "pace") != 0 && std::strcmp(mode, "sprint") != 0) ||
        !query_value(query, "pool", pool, sizeof(pool)) || pool[0] == '\0' ||
        !query_value(query, "direction", direction, sizeof(direction)) ||
        (std::strcmp(direction, "Near") != 0 && std::strcmp(direction, "Far") != 0) ||
        !query_value(query, "audio", audio, sizeof(audio)) ||
        (std::strcmp(audio, "Yes") != 0 && std::strcmp(audio, "No") != 0) ||
        !query_value(query, "duration", duration, sizeof(duration)) ||
        !query_value(query, "interval", interval, sizeof(interval)) ||
        !query_value(query, "distance", distance_text, sizeof(distance_text)) ||
        !query_value(query, "strategy", strategy, sizeof(strategy))) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Invalid set fields\"}");
    }
    const bool sprint = std::strcmp(mode, "sprint") == 0;
    std::uint32_t distance_yards = 0;
    std::uint32_t target_ms = 0;
    std::uint32_t interval_ms = 0;
    std::uint32_t repetitions = 1;
    if (!parse_positive_u32(distance_text, distance_yards) || distance_yards % 25U != 0U ||
        distance_yards > 5000U || !parse_time_ms(duration, target_ms) ||
        !parse_time_ms(interval, interval_ms) || interval_ms < target_ms ||
        (!sprint && (!query_value(query, "repetitions", repetitions_text, sizeof(repetitions_text)) ||
                     !parse_positive_u32(repetitions_text, repetitions) || repetitions > rabbit::core::kMaxPlanEntries))) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Invalid distance, timing, or repetitions\"}");
    }
    std::uint32_t final_target_ms = target_ms;
    std::uint16_t surge_permille = 0;
    if (std::strcmp(strategy, "negative_split") == 0) {
        if (sprint || !query_value(query, "variation", variation, sizeof(variation)) ||
            !parse_time_ms(variation, final_target_ms) || final_target_ms >= target_ms) {
            return send_error(request, "400 Bad Request", "{\"error\":\"Invalid final-rep target\"}");
        }
    } else if (std::strcmp(strategy, "surge") == 0) {
        if (!query_value(query, "variation", variation, sizeof(variation))) {
            return send_error(request, "400 Bad Request", "{\"error\":\"Missing surge percentage\"}");
        }
        char* end = nullptr;
        const double percent = std::strtod(variation, &end);
        if (end == variation || *end != '\0' || !std::isfinite(percent) || percent < 0.1 || percent > 45.0) {
            return send_error(request, "400 Bad Request", "{\"error\":\"Surge must be 0.1 to 45 percent\"}");
        }
        surge_permille = static_cast<std::uint16_t>(std::lround(percent * 10.0));
    } else if (std::strcmp(strategy, "even") != 0) {
        return send_error(request, "400 Bad Request", "{\"error\":\"Unsupported pacing strategy\"}");
    }
    std::uint8_t flags = rabbit::protocol::kPrepareFlagWaitForInterval;
    if (std::strcmp(direction, "Far") == 0) flags |= rabbit::protocol::kPrepareFlagFarToNear;
    if (std::strcmp(audio, "Yes") == 0) flags |= rabbit::protocol::kPrepareFlagAudio;
    if (sprint) flags |= rabbit::protocol::kPrepareFlagContinuous;
    const auto distance_mm = static_cast<std::uint32_t>(static_cast<std::uint64_t>(distance_yards) * 9144U / 10U);
    if (!transport.prepare(distance_mm, target_ms, interval_ms, flags,
                           static_cast<std::uint16_t>(repetitions), final_target_ms, surge_permille)) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected set\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t start_handler(httpd_req_t* request) {
    if (!transport.start()) return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected start\"}");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t stop_handler(httpd_req_t* request) {
    if (!transport.stop()) return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected stop\"}");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t cancel_handler(httpd_req_t* request) {
    if (!transport.cancel()) return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected cancel\"}");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t test_page_handler(httpd_req_t* request) {
    std::puts("Rabbit HTTP GET /");
    static constexpr char page[] = R"html(<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Rabbit test</title><style>body{font:18px system-ui;margin:2rem;max-width:36rem}button{font:inherit;margin:.25rem;padding:.6rem}pre{white-space:pre-wrap}</style><h1>Rabbit manual test</h1><p>Prepare uses the default 25-yard, 30-second test. Start begins it; Stop is always available.</p><p><button id="status">Refresh status</button><button id="prepare">Prepare</button><button id="start">Start</button><button id="stop">Stop</button></p><pre id="result">Ready</pre><script>const result=document.querySelector('#result');async function call(path,options){try{const response=await fetch(path,options);const body=await response.text();result.textContent=response.status+' '+body}catch(error){result.textContent=error}}document.querySelector('#status').onclick=()=>call('/api/set-status');document.querySelector('#prepare').onclick=()=>call('/api/workout/prepare',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});document.querySelector('#start').onclick=()=>call('/api/workout/start',{method:'POST'});document.querySelector('#stop').onclick=()=>call('/api/workout/stop',{method:'POST'});</script>)html";
    httpd_resp_set_type(request, "text/html");
    const auto result = httpd_resp_send(request, page, sizeof(page) - 1U);
    std::printf("Rabbit HTTP GET / response: %s\n", esp_err_to_name(result));
    return result;
}

void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 40;
    config.stack_size = 6144;
    httpd_handle_t server = nullptr;
    const auto result = httpd_start(&server, &config);
    if (result != ESP_OK) {
        std::printf("Rabbit HTTP server failed to start: %s\n", esp_err_to_name(result));
        return;
    }
    httpd_uri_t status{};
    status.uri = "/api/set-status";
    status.method = HTTP_GET;
    status.handler = status_handler;
    httpd_uri_t prepare{};
    prepare.uri = "/api/workout/prepare";
    prepare.method = HTTP_POST;
    prepare.handler = prepare_handler;
    httpd_uri_t start{};
    start.uri = "/api/workout/start";
    start.method = HTTP_POST;
    start.handler = start_handler;
    httpd_uri_t workout_stop{};
    workout_stop.uri = "/api/workout/stop";
    workout_stop.method = HTTP_POST;
    workout_stop.handler = stop_handler;
    httpd_uri_t legacy_stop{};
    legacy_stop.uri = "/stop";
    legacy_stop.method = HTTP_POST;
    legacy_stop.handler = stop_handler;
    httpd_uri_t legacy_prepare{};
    legacy_prepare.uri = "/prep";
    legacy_prepare.method = HTTP_GET;
    legacy_prepare.handler = legacy_prepare_handler;
    httpd_uri_t legacy_start{};
    legacy_start.uri = "/start";
    legacy_start.method = HTTP_GET;
    legacy_start.handler = start_handler;
    httpd_uri_t legacy_sprint_start{};
    legacy_sprint_start.uri = "/startsprint";
    legacy_sprint_start.method = HTTP_GET;
    legacy_sprint_start.handler = start_handler;
    httpd_uri_t legacy_stop_get{};
    legacy_stop_get.uri = "/stop";
    legacy_stop_get.method = HTTP_GET;
    legacy_stop_get.handler = stop_handler;
    httpd_uri_t cancel_prep{};
    cancel_prep.uri = "/cancel-prep";
    cancel_prep.method = HTTP_GET;
    cancel_prep.handler = cancel_handler;
    httpd_uri_t test_page{};
    test_page.uri = "/test";
    test_page.method = HTTP_GET;
    test_page.handler = test_page_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &test_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &prepare));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &workout_stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &legacy_stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &legacy_prepare));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &legacy_start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &legacy_sprint_start));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &legacy_stop_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cancel_prep));
    ESP_ERROR_CHECK(rabbit::esp32::register_audio_api(server,
        [](rabbit::protocol::MessageType type, const std::uint8_t* payload, std::uint16_t size, rabbit::protocol::Frame& response) {
            return transport.audio(type, payload, size, response);
        }) ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(rabbit::esp32::register_web_ui(server) ? ESP_OK : ESP_FAIL);
    std::printf("Rabbit HTTP server ready: http://%s/\n", kTestAccessPointAddress);
}

}  // namespace

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    rabbit::esp32::initialise_lcd();
    rabbit::esp32::show_lcd_status("STARTING", "--");
    transport.initialise();
    rabbit::protocol::Frame startup_status{};
    const bool link_ready = transport.status(startup_status);
    std::printf("Rabbit RP2350 UART link: %s\n", link_ready ? "ready" : "unavailable");
    start_wifi();
    start_http_server();
    rabbit::esp32::start_wifi_provisioning();
    if (xTaskCreate(lcd_status_task, "rabbit_lcd_status", 4096, nullptr, 4, nullptr) != pdPASS) {
        std::puts("Rabbit LCD status task could not be started");
    }
}
