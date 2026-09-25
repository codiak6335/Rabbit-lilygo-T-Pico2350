#include <cinttypes>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
}

#include "rabbit/protocol/framing.hpp"
#include "rabbit/protocol/workout_service.hpp"

namespace {
constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kUartTxPin = 7;
constexpr int kUartRxPin = 6;
constexpr int kUartRtsPin = 4;
constexpr int kUartCtsPin = 5;
constexpr int kHttpBodyLimit = 256;
constexpr std::int64_t kResponseTimeoutUs = 1'500'000;
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
    std::printf("Rabbit test AP ready: SSID=%s address=%s password=%s\n", test_access_point_ssid,
                kTestAccessPointAddress, kTestAccessPointPassword);
}

bool load_wifi_credentials(wifi_config_t& configuration) {
    nvs_handle_t storage{};
    if (nvs_open("wifi", NVS_READONLY, &storage) != ESP_OK) return false;
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
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!wifi_station_configured) return;
        const auto result = esp_wifi_connect();
        if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
            std::printf("Rabbit Wi-Fi connect request failed: %ld\n", static_cast<long>(result));
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!wifi_station_configured) return;
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);
        std::printf("Rabbit Wi-Fi disconnected: reason=%u\n", static_cast<unsigned int>(event->reason));
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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_active = false;
        report_configured_network();
        static_cast<void>(esp_wifi_connect());
    }
}

void start_wifi() {
    if (esp_netif_create_default_wifi_ap() == nullptr || esp_netif_create_default_wifi_sta() == nullptr) {
        std::puts("Rabbit Wi-Fi network interfaces could not be created");
        return;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_test_access_point();
    wifi_station_configured = load_wifi_credentials(wifi_configuration);
    if (wifi_station_configured) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration));
    } else {
        std::puts("Rabbit Wi-Fi credentials are not provisioned; use the local test AP");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (wifi_station_configured) std::puts("Rabbit Wi-Fi connecting");
}

void write_u32(std::uint8_t* bytes, const std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
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
        uart_config_t config{};
        config.baud_rate = 115200;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.rx_flow_ctrl_thresh = 64;
        config.source_clk = UART_SCLK_DEFAULT;
        ESP_ERROR_CHECK(uart_param_config(kUart, &config));
        ESP_ERROR_CHECK(uart_set_pin(kUart, kUartTxPin, kUartRxPin, kUartRtsPin, kUartCtsPin));
        ESP_ERROR_CHECK(uart_driver_install(kUart, 1024, 1024, 0, nullptr, 0));
        session_id_ = esp_random() | 1U;
    }

    [[nodiscard]] bool prepare(const std::uint32_t distance_mm, const std::uint32_t target_ms,
                               const std::uint32_t interval_ms, const std::uint8_t flags) {
        std::uint8_t payload[rabbit::protocol::kPreparePayloadSize]{};
        payload[0] = flags;
        write_u32(payload + 1, distance_mm);
        write_u32(payload + 5, target_ms);
        write_u32(payload + 9, interval_ms);
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::PrepareBegin, payload, sizeof(payload), response);
    }

    [[nodiscard]] bool start() {
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::Start, nullptr, 0, response);
    }

    [[nodiscard]] bool stop() {
        rabbit::protocol::Frame response{};
        return call(rabbit::protocol::MessageType::Stop, nullptr, 0, response);
    }

    [[nodiscard]] bool status(rabbit::protocol::Frame& response) {
        return call(rabbit::protocol::MessageType::Status, nullptr, 0, response);
    }

private:
    [[nodiscard]] bool call(const rabbit::protocol::MessageType type, const std::uint8_t* payload,
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

bool read_body(httpd_req_t* request, char (&body)[kHttpBodyLimit + 1]) {
    if (request->content_len > kHttpBodyLimit) return false;
    int received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) return false;
        received += count;
    }
    body[received] = '\0';
    return true;
}

esp_err_t send_error(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, message);
}

esp_err_t status_handler(httpd_req_t* request) {
    rabbit::protocol::Frame response{};
    if (!transport.status(response) || response.payload_size != 10U) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 transport unavailable\"}");
    }
    const auto state = response.payload[0];
    const auto running = (response.payload[1] & 0x01U) != 0U;
    char body[192]{};
    const int length = std::snprintf(
        body, sizeof(body),
        "{\"running\":%s,\"stopped\":%s,\"prepped\":%s,\"complete\":%s,\"entry\":%u,\"progressPermille\":%u,\"sequence\":%" PRIu32 "}",
        running ? "true" : "false", state == 5U ? "true" : "false", state == 1U ? "true" : "false",
        state == 6U ? "true" : "false", static_cast<unsigned int>(read_u16(response.payload.data() + 2)),
        static_cast<unsigned int>(read_u16(response.payload.data() + 4)), read_u32(response.payload.data() + 6));
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, length);
}

esp_err_t prepare_handler(httpd_req_t* request) {
    char body[kHttpBodyLimit + 1]{};
    std::uint32_t distance_mm = 22'860;
    std::uint32_t target_ms = 30'000;
    std::uint32_t interval_ms = 30'000;
    if (!read_body(request, body) || !json_u32(body, "distanceMm", distance_mm, distance_mm) ||
        !json_u32(body, "targetMs", target_ms, target_ms) || !json_u32(body, "intervalMs", interval_ms, interval_ms)) {
        return send_error(request, "400 Bad Request", "{\"error\":\"invalid workout payload\"}");
    }
    std::uint8_t flags = 0;
    if (std::strstr(body, "far-to-near") != nullptr) flags |= rabbit::protocol::kPrepareFlagFarToNear;
    if (json_true(body, "audio")) flags |= rabbit::protocol::kPrepareFlagAudio;
    if (json_true(body, "waitForInterval")) flags |= rabbit::protocol::kPrepareFlagWaitForInterval;
    if (!transport.prepare(distance_mm, target_ms, interval_ms, flags)) {
        return send_error(request, "503 Service Unavailable", "{\"error\":\"RP2350 rejected prepare\"}");
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

esp_err_t test_page_handler(httpd_req_t* request) {
    static constexpr char page[] = R"html(<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Rabbit test</title><style>body{font:18px system-ui;margin:2rem;max-width:36rem}button{font:inherit;margin:.25rem;padding:.6rem}pre{white-space:pre-wrap}</style><h1>Rabbit manual test</h1><p>Prepare uses the default 25-yard, 30-second test. Start begins it; Stop is always available.</p><p><button id="status">Refresh status</button><button id="prepare">Prepare</button><button id="start">Start</button><button id="stop">Stop</button></p><pre id="result">Ready</pre><script>const result=document.querySelector('#result');async function call(path,options){try{const response=await fetch(path,options);const body=await response.text();result.textContent=response.status+' '+body}catch(error){result.textContent=error}}document.querySelector('#status').onclick=()=>call('/api/set-status');document.querySelector('#prepare').onclick=()=>call('/api/workout/prepare',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});document.querySelector('#start').onclick=()=>call('/api/workout/start',{method:'POST'});document.querySelector('#stop').onclick=()=>call('/api/workout/stop',{method:'POST'});</script>)html";
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, page, sizeof(page) - 1U);
}

void start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 6144;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) return;
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
    httpd_uri_t test_page{};
    test_page.uri = "/";
    test_page.method = HTTP_GET;
    test_page.handler = test_page_handler;
    static_cast<void>(httpd_register_uri_handler(server, &test_page));
    static_cast<void>(httpd_register_uri_handler(server, &status));
    static_cast<void>(httpd_register_uri_handler(server, &prepare));
    static_cast<void>(httpd_register_uri_handler(server, &start));
    static_cast<void>(httpd_register_uri_handler(server, &workout_stop));
    static_cast<void>(httpd_register_uri_handler(server, &legacy_stop));
}

}  // namespace

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    transport.initialise();
    start_wifi();
    start_http_server();
}
