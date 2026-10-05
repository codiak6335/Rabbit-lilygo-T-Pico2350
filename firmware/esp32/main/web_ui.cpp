#include "web_ui.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "cJSON.h"
#include "nvs.h"
}

namespace rabbit::esp32 {
namespace {

constexpr std::size_t kMaxSetsBytes = 8192;
constexpr char kSetsNamespace[] = "rabbit_data";
constexpr char kSetsKey[] = "sets";

#define RABBIT_ASSET(name) \
    extern const unsigned char name##_start[] asm("_binary_" #name "_start"); \
    extern const unsigned char name##_end[] asm("_binary_" #name "_end")

RABBIT_ASSET(rabbit_index_html);
RABBIT_ASSET(rabbit_styles_css);
RABBIT_ASSET(rabbit_deckscript_js);
RABBIT_ASSET(rabbit_navigation_js);
RABBIT_ASSET(rabbit_audio_js);
RABBIT_ASSET(rabbit_pools_json);
RABBIT_ASSET(rabbit_initial_sets_json);
RABBIT_ASSET(rabbit_deckscript_prompt);
RABBIT_ASSET(rabbit_favicon_ico);

#undef RABBIT_ASSET

esp_err_t send_asset(httpd_req_t* request, const unsigned char* begin, const unsigned char* end,
                     const char* content_type, bool null_terminated = true) {
    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto size = static_cast<std::size_t>(end - begin) - (null_terminated ? 1U : 0U);
    return httpd_resp_send(request, reinterpret_cast<const char*>(begin), static_cast<ssize_t>(size));
}

esp_err_t index_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_index_html_start, rabbit_index_html_end, "text/html; charset=utf-8");
}

esp_err_t css_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_styles_css_start, rabbit_styles_css_end, "text/css; charset=utf-8");
}

esp_err_t deckscript_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_deckscript_js_start, rabbit_deckscript_js_end, "application/javascript; charset=utf-8");
}

esp_err_t navigation_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_navigation_js_start, rabbit_navigation_js_end, "application/javascript; charset=utf-8");
}

esp_err_t audio_script_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_audio_js_start, rabbit_audio_js_end, "application/javascript; charset=utf-8");
}

esp_err_t pools_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_pools_json_start, rabbit_pools_json_end, "application/json");
}

esp_err_t prompt_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_deckscript_prompt_start, rabbit_deckscript_prompt_end, "text/plain; charset=utf-8");
}

esp_err_t favicon_handler(httpd_req_t* request) {
    return send_asset(request, rabbit_favicon_ico_start, rabbit_favicon_ico_end, "image/x-icon", false);
}

esp_err_t send_json_error(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, message);
}

bool valid_sets_json(const char* data, const std::size_t size) {
    cJSON* const root = cJSON_ParseWithLength(data, size);
    if (root == nullptr) return false;
    const cJSON* const sets = cJSON_GetObjectItemCaseSensitive(root, "sets");
    bool valid = cJSON_IsObject(root) && cJSON_IsObject(sets) && cJSON_GetArraySize(sets) <= 128;
    if (valid) {
        for (const cJSON* item = sets->child; item != nullptr; item = item->next) {
            if (!cJSON_IsObject(item) || item->string == nullptr || std::strlen(item->string) > 64U) {
                valid = false;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return valid;
}

esp_err_t sets_get_handler(httpd_req_t* request) {
    nvs_handle_t handle{};
    if (nvs_open(kSetsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return send_asset(request, rabbit_initial_sets_json_start, rabbit_initial_sets_json_end, "application/json");
    }
    std::size_t size = 0;
    const esp_err_t size_result = nvs_get_blob(handle, kSetsKey, nullptr, &size);
    if (size_result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return send_asset(request, rabbit_initial_sets_json_start, rabbit_initial_sets_json_end, "application/json");
    }
    if (size_result != ESP_OK || size == 0 || size > kMaxSetsBytes) {
        nvs_close(handle);
        return send_json_error(request, "500 Internal Server Error", "{\"error\":\"Saved sets are invalid\"}");
    }
    auto* const buffer = static_cast<char*>(std::malloc(size));
    if (buffer == nullptr) {
        nvs_close(handle);
        return send_json_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    }
    const esp_err_t result = nvs_get_blob(handle, kSetsKey, buffer, &size);
    nvs_close(handle);
    if (result != ESP_OK) {
        std::free(buffer);
        return send_json_error(request, "500 Internal Server Error", "{\"error\":\"Could not read saved sets\"}");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t sent = httpd_resp_send(request, buffer, static_cast<ssize_t>(size));
    std::free(buffer);
    return sent;
}

esp_err_t sets_post_handler(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len > static_cast<int>(kMaxSetsBytes)) {
        return send_json_error(request, "413 Payload Too Large", "{\"error\":\"Saved sets exceed 8 KiB\"}");
    }
    const auto size = static_cast<std::size_t>(request->content_len);
    auto* const buffer = static_cast<char*>(std::malloc(size));
    if (buffer == nullptr) return send_json_error(request, "503 Service Unavailable", "{\"error\":\"Out of memory\"}");
    std::size_t received = 0;
    while (received < size) {
        const int count = httpd_req_recv(request, buffer + received, size - received);
        if (count <= 0) {
            std::free(buffer);
            return send_json_error(request, "400 Bad Request", "{\"error\":\"Incomplete saved sets\"}");
        }
        received += static_cast<std::size_t>(count);
    }
    if (!valid_sets_json(buffer, size)) {
        std::free(buffer);
        return send_json_error(request, "400 Bad Request", "{\"error\":\"Invalid saved sets\"}");
    }
    nvs_handle_t handle{};
    esp_err_t result = nvs_open(kSetsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, kSetsKey, buffer, size);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    std::free(buffer);
    if (result != ESP_OK) {
        return send_json_error(request, "507 Insufficient Storage", "{\"error\":\"Could not persist saved sets\"}");
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t read_only_config_handler(httpd_req_t* request) {
    return send_json_error(request, "501 Not Implemented",
                           "{\"error\":\"Pool and Wi-Fi editing are not available on this controller yet\"}");
}

bool add_route(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t route{};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    return httpd_register_uri_handler(server, &route) == ESP_OK;
}

}  // namespace

bool register_web_ui(httpd_handle_t server) {
    return add_route(server, "/", HTTP_GET, index_handler) &&
        add_route(server, "/css/styles.css", HTTP_GET, css_handler) &&
        add_route(server, "/js/deckscript.js", HTTP_GET, deckscript_handler) &&
        add_route(server, "/js/navigation.js", HTTP_GET, navigation_handler) &&
        add_route(server, "/js/audio.js", HTTP_GET, audio_script_handler) &&
        add_route(server, "/db/pools.json", HTTP_GET, pools_handler) &&
        add_route(server, "/db/pools.json", HTTP_POST, read_only_config_handler) &&
        add_route(server, "/db/sets.json", HTTP_GET, sets_get_handler) &&
        add_route(server, "/db/sets.json", HTTP_POST, sets_post_handler) &&
        add_route(server, "/db/wifi.json", HTTP_GET, read_only_config_handler) &&
        add_route(server, "/db/wifi.json", HTTP_POST, read_only_config_handler) &&
        add_route(server, "/static/deckscript-chatgpt-prompt.txt", HTTP_GET, prompt_handler) &&
        add_route(server, "/favicon.ico", HTTP_GET, favicon_handler);
}

}  // namespace rabbit::esp32
