#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bot_config.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "discord_provision.h"
#include "discord_transport.h"

#define APP_TAG "bot"

#define APP_POLL_MS 100u
#define APP_SNTP_TIMEOUT_MS 15000u
#define APP_WIFI_TIMEOUT_MS 30000u

static char wifi_ssid[DC_PROVISION_SSID_LEN_MAX + 1u];
static char wifi_pass[DC_PROVISION_PASS_LEN_MAX + 1u];

unsigned dc_config_error_count(void);
unsigned dc_config_checks_run(void);
const char *dc_config_check_label(unsigned index);
unsigned dc_config_check_value(unsigned index);

static SemaphoreHandle_t wifi_sem;

static void on_got_ip(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    xSemaphoreGive(wifi_sem);
}

static void app_log_heap(const char *milestone)
{
    uint32_t free = esp_get_free_heap_size();
    size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_8BIT);
    uint32_t frag = 0u;

    if (free > 0u) {
        frag = 100u - (uint32_t)((uint64_t)largest * 100u /
                                     (uint64_t)free);
    }
    ESP_LOGI(APP_TAG, "%s heap %u largest %u frag %u%%",
             milestone, free, (unsigned)largest, frag);
}

static void app_log_config(void)
{
    for (unsigned i = 0u; i < dc_config_checks_run(); i++) {
        ESP_LOGE(APP_TAG, "%s = %u",
                 dc_config_check_label(i),
                 dc_config_check_value(i));
    }
}

static esp_err_t wifi_setup(void)
{
    wifi_config_t wifi_cfg;
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    err = esp_wifi_init(&wifi_init);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "esp_wifi_init failed: 0x%x", err);
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "esp_wifi_set_mode failed: 0x%x", err);
        return err;
    }
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strlcpy((char *)wifi_cfg.sta.ssid, wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, wifi_pass, sizeof(wifi_cfg.sta.password));
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "esp_wifi_set_config failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(APP_TAG, "Wi-Fi configured for SSID: %s", wifi_ssid);
    return ESP_OK;
}

static esp_err_t wifi_wait(void)
{
    static StaticSemaphore_t sem_buf;
    esp_event_handler_instance_t ip_instance = NULL;
    esp_err_t err;
    uint32_t waited;

    wifi_sem = xSemaphoreCreateBinaryStatic(&sem_buf);
    if (wifi_sem == NULL) {
        return ESP_FAIL;
    }
    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              on_got_ip, NULL,
                                              &ip_instance);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    for (waited = 0u; waited < APP_WIFI_TIMEOUT_MS;
         waited += APP_POLL_MS) {
        if (xSemaphoreTake(wifi_sem,
                           pdMS_TO_TICKS(APP_POLL_MS)) ==
                pdTRUE) {
            ESP_LOGI(APP_TAG, "wi-fi up");
            return ESP_OK;
        }
    }
    ESP_LOGE(APP_TAG, "wi-fi ip timeout");
    return ESP_FAIL;
}

static esp_err_t wifi_up(void)
{
    return wifi_setup() == ESP_OK ? wifi_wait() : ESP_FAIL;
}

static void sntp_configure(void)
{
    esp_sntp_setservername(0u, "pool.ntp.org");
    esp_sntp_init();
}

static esp_err_t sntp_wait(void)
{
    uint32_t waited;

    for (waited = 0u; waited < APP_SNTP_TIMEOUT_MS;
         waited += APP_POLL_MS) {
        if (esp_sntp_get_sync_status() ==
                SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(APP_TAG, "sntp synced");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_POLL_MS));
    }
    ESP_LOGE(APP_TAG, "sntp sync timeout");
    return ESP_FAIL;
}

static void ping_on_interaction(const DiscordInteraction *it)
{
    (void)it;
}

static const DiscordFeature ping_feature = {
    .name = "ping",
    .required_intents = 0u,
    .init_priority = 0u,
    .on_message = NULL,
    .on_interaction = ping_on_interaction
};

void app_main(void)
{
    unsigned errors;
    esp_err_t err;

    app_log_heap("boot");
    errors = dc_config_error_count();
    if (errors != 0u) {
        ESP_LOGE(APP_TAG,
                 "bot_config has %u failing checks, refusing to run",
                 errors);
        app_log_config();
        return;
    }

    err = dc_provision_ensure_token();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "token provisioning failed 0x%x",
                 (unsigned)err);
        return;
    }

    err = dc_provision_ensure_wifi(wifi_ssid, sizeof(wifi_ssid),
                                   wifi_pass, sizeof(wifi_pass));
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "wi-fi provisioning failed 0x%x",
                 (unsigned)err);
        return;
    }

    err = wifi_up();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "wifi_up failed 0x%x", (unsigned)err);
        return;
    }
    app_log_heap("after wifi");

    sntp_configure();

    err = sntp_wait();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "sntp_wait failed 0x%x", (unsigned)err);
        return;
    }

    err = discord_init();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "discord init failed 0x%x",
                 (unsigned)err);
        return;
    }
    if (!dc_registry_add(discord_registry(), &ping_feature)) {
        ESP_LOGE(APP_TAG, "ping feature register failed");
        return;
    }
    ESP_LOGI(APP_TAG, "ping feature registered (/ping)");
    err = discord_start();
    if (err != ESP_OK) {
        ESP_LOGE(APP_TAG, "discord start failed 0x%x",
                 (unsigned)err);
    }
}
