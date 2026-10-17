#include "discord_provision.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "discord_security.h"

static const char *TAG = "dc_provision";

#define DC_PROVISION_TOKEN_KEY "bot_token"
#define DC_PROVISION_TOKEN_MAX 128
#define DC_PROVISION_SSID_KEY "wifi_ssid"
#define DC_PROVISION_PASS_KEY "wifi_pass"
#define DC_PROVISION_SSID_MAX 34
#define DC_PROVISION_PASS_MAX 66
#define DC_PROVISION_BACKOFF_BASE_MS 500u
#define DC_PROVISION_BACKOFF_MAX_MS 30000u

static esp_err_t stored_token_present(void)
{
    nvs_handle_t handle;
    size_t len = 0;
    esp_err_t err;

    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(DC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, DC_PROVISION_TOKEN_KEY, NULL, &len);
    nvs_close(handle);

    if (err == ESP_OK && len == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    return err;
}

static esp_err_t store_token(const char *token)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(DC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, DC_PROVISION_TOKEN_KEY, token);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static uint32_t next_backoff_ms(uint32_t current_ms)
{
    uint32_t next = current_ms * 2u;

    if (next < current_ms || next > DC_PROVISION_BACKOFF_MAX_MS) {
        return DC_PROVISION_BACKOFF_MAX_MS;
    }

    return next;
}

static size_t read_console_line(char *buf, size_t cap)
{
    size_t pos = 0;

    if (cap <= 1) {
        return 0;
    }

    clearerr(stdin);
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            vTaskDelay(pdMS_TO_TICKS(20));
            int next = fgetc(stdin);
            if (next != EOF && next != '\n' && next != '\r') {
                ungetc(next, stdin);
            }
            return pos;
        }

        if (c == '\b' || c == 0x7f) {
            if (pos > 0) {
                pos--;
            }
            continue;
        }

        if (pos < cap - 1) {
            buf[pos++] = (char)c;
        } else {
            while (c != '\r' && c != '\n') {
                c = fgetc(stdin);
                if (c == EOF) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            ESP_LOGW(TAG, "line longer than the input buffer, rejected");
            return SIZE_MAX;
        }
    }
}

static size_t prompt_for_token(char *buf, size_t cap)
{
    printf("provision: paste the bot token and press Enter\n");
    fflush(stdout);

    return read_console_line(buf, cap);
}

esp_err_t dc_provision_ensure_token(void)
{
    uint32_t backoff_ms = DC_PROVISION_BACKOFF_BASE_MS;
    char line[DC_PROVISION_TOKEN_MAX];
    esp_err_t present;

    present = stored_token_present();
    if (present == ESP_OK) {
        return ESP_OK;
    }

    if (present != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "stored token unreadable (0x%x), treating as absent",
                 (unsigned int)present);
    }
    ESP_LOGI(TAG, "serial provisioning required, no usable token in NVS");

    for (;;) {
        DcSecStatus status;
        esp_err_t err;
        size_t len;

        len = prompt_for_token(line, sizeof(line));
        if (len == 0u || len == SIZE_MAX) {
            continue;
        }

        status = dc_token_looks_valid(line, len);
        if (status != DC_SEC_OK) {
            ESP_LOGW(TAG, "token shape rejected (%d), not stored", (int)status);
            memset(line, 0, sizeof(line));
            continue;
        }

        err = store_token(line);
        memset(line, 0, sizeof(line));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "token store failed (0x%x), retrying in %u ms",
                     (unsigned int)err, (unsigned int)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = next_backoff_ms(backoff_ms);
            continue;
        }

        ESP_LOGI(TAG, "bot token stored, restarting");
        esp_restart();
    }
}

static esp_err_t stored_wifi_present(char *ssid, size_t ssid_cap,
                                     char *pass, size_t pass_cap)
{
    nvs_handle_t handle;
    size_t ssid_len = 0;
    size_t pass_len = 0;
    esp_err_t err;

    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(DC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, DC_PROVISION_SSID_KEY, NULL,
                      &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, DC_PROVISION_PASS_KEY, NULL,
                          &pass_len);
    }
    if (err != ESP_OK || ssid_len < 2u || pass_len < 1u ||
        ssid_len > ssid_cap || pass_len > pass_cap) {
        nvs_close(handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    err = nvs_get_str(handle, DC_PROVISION_SSID_KEY, ssid,
                      &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, DC_PROVISION_PASS_KEY, pass,
                          &pass_len);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t store_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(DC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, DC_PROVISION_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, DC_PROVISION_PASS_KEY, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t dc_provision_ensure_wifi(char *ssid, size_t ssid_cap,
                                   char *pass, size_t pass_cap)
{
    uint32_t backoff_ms = DC_PROVISION_BACKOFF_BASE_MS;
    char ssid_line[DC_PROVISION_SSID_MAX];
    char pass_line[DC_PROVISION_PASS_MAX];
    esp_err_t present;

    if (ssid == NULL || pass == NULL || ssid_cap == 0u ||
        pass_cap == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    present = stored_wifi_present(ssid, ssid_cap, pass, pass_cap);
    if (present == ESP_OK) {
        return ESP_OK;
    }

    if (present != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "stored wi-fi unreadable (0x%x), treating as absent",
                 (unsigned int)present);
    }
    ESP_LOGI(TAG,
             "serial provisioning required, no usable wi-fi in NVS");

    for (;;) {
        size_t ssid_len;
        size_t pass_len;
        esp_err_t err;

        printf("provision: enter the Wi-Fi SSID\n");
        fflush(stdout);
        ssid_len = read_console_line(ssid_line, sizeof(ssid_line));
        if (ssid_len == 0u || ssid_len == SIZE_MAX) {
            continue;
        }

        printf("provision: enter the Wi-Fi password, "
               "empty for an open network\n");
        fflush(stdout);
        pass_len = read_console_line(pass_line, sizeof(pass_line));
        if (pass_len == SIZE_MAX) {
            continue;
        }

        err = store_wifi(ssid_line, pass_line);
        memset(ssid_line, 0, sizeof(ssid_line));
        memset(pass_line, 0, sizeof(pass_line));
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "wi-fi store failed (0x%x), retrying in %u ms",
                     (unsigned int)err, (unsigned int)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = next_backoff_ms(backoff_ms);
            continue;
        }

        ESP_LOGI(TAG, "wi-fi credentials stored, restarting");
        esp_restart();
    }
}
