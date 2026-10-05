#include "wifi_provision.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

extern "C" {
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
}

namespace rabbit::esp32 {
namespace {
constexpr char kNamespace[] = "rabbit_net";

bool save_credentials(char* line) {
    if (std::strncmp(line, "WIFI\t", 5) != 0) return false;
    char* ssid = line + 5;
    char* separator = std::strchr(ssid, '\t');
    if (separator == nullptr) return false;
    *separator = '\0';
    char* password = separator + 1;
    const auto ssid_length = std::strlen(ssid);
    const auto password_length = std::strlen(password);
    if (ssid_length == 0 || ssid_length >= 32 || password_length < 8 || password_length >= 64) return false;

    nvs_handle_t storage{};
    if (nvs_open(kNamespace, NVS_READWRITE, &storage) != ESP_OK) return false;
    const auto result = nvs_set_str(storage, "ssid", ssid) == ESP_OK &&
        nvs_set_str(storage, "password", password) == ESP_OK && nvs_commit(storage) == ESP_OK;
    nvs_close(storage);
    return result;
}

void provisioning_task(void*) {
    usb_serial_jtag_driver_config_t usb_config{};
    usb_config.rx_buffer_size = 256;
    usb_config.tx_buffer_size = 256;
    const auto result = usb_serial_jtag_driver_install(&usb_config);
    if (result != ESP_OK) {
        std::printf("Rabbit Wi-Fi provisioning USB setup failed: %s\n", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, nullptr, _IONBF, 0);
    std::puts("Rabbit Wi-Fi provisioning ready");

    char line[128]{};
    std::size_t length = 0;
    while (true) {
        char character{};
        if (read(STDIN_FILENO, &character, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (character == '\r') continue;
        if (character != '\n') {
            if (length + 1 < sizeof(line)) line[length++] = character;
            else length = 0;
            continue;
        }
        line[length] = '\0';
        if (std::strcmp(line, "PING") == 0) {
            std::puts("Rabbit Wi-Fi provisioning PONG");
        } else if (save_credentials(line)) {
            std::memset(line, 0, sizeof(line));
            std::puts("Rabbit Wi-Fi credentials saved; restarting");
            std::fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            std::puts("Rabbit Wi-Fi provisioning command invalid");
        }
        std::memset(line, 0, sizeof(line));
        length = 0;
    }
}
}  // namespace

void start_wifi_provisioning() {
    if (xTaskCreate(provisioning_task, "wifi_provision", 4096, nullptr, 3, nullptr) != pdPASS) {
        std::puts("Rabbit Wi-Fi provisioning task could not start");
    }
}

}  // namespace rabbit::esp32
