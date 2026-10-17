#include "discord_provision.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dc_nvs";

#define DC_NVS_IDENTIFY_COUNT_KEY "identify_count"
#define DC_NVS_IDENTIFY_WINDOW_KEY "identify_wstart"

static esp_err_t nvs_flash_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }

    return nvs_open(DC_NVS_NAMESPACE, mode, handle);
}

static DcErr identify_read(void *ctx, uint32_t *count, uint64_t *window_start_ms)
{
    nvs_handle_t handle;
    esp_err_t err;
    esp_err_t count_err;
    esp_err_t window_err;

    (void)ctx;
    *count = 0;
    *window_start_ms = 0;

    err = nvs_flash_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    count_err = nvs_get_u32(handle, DC_NVS_IDENTIFY_COUNT_KEY, count);
    window_err = nvs_get_u64(handle, DC_NVS_IDENTIFY_WINDOW_KEY, window_start_ms);
    nvs_close(handle);

    if (count_err == ESP_OK && window_err == ESP_OK) {
        return ESP_OK;
    }
    if (count_err == ESP_ERR_NVS_NOT_FOUND &&
        window_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (count_err != ESP_OK && count_err != ESP_ERR_NVS_NOT_FOUND) {
        return count_err;
    }
    if (window_err != ESP_OK && window_err != ESP_ERR_NVS_NOT_FOUND) {
        return window_err;
    }

    ESP_LOGE(TAG, "identify counter store corrupted, one key without its pair");
    return ESP_FAIL;
}

static DcErr identify_write(void *ctx, uint32_t count, uint64_t window_start_ms)
{
    nvs_handle_t handle;
    esp_err_t err;

    (void)ctx;

    err = nvs_flash_open(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, DC_NVS_IDENTIFY_COUNT_KEY, count);
        if (err == ESP_OK) {
            err = nvs_set_u64(handle, DC_NVS_IDENTIFY_WINDOW_KEY, window_start_ms);
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identify counter write failed: 0x%x", (unsigned int)err);
    }

    return err;
}

DcIdentifyStore dc_nvs_identify_store(void)
{
    DcIdentifyStore store;
    store.read = identify_read;
    store.write = identify_write;
    store.ctx = NULL;
    return store;
}
