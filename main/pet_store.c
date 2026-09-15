// main/pet_store.c —— NVS 双槽持久化实现。
#include "pet_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PET_STORE_MAGIC 0x44504B31U  /* "DPK1" */

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t model_size;
    pet_model_t model;
    uint32_t crc;
} pet_store_record_t;

static const char *TAG = "pet_store";
static const char *NAMESPACE = "desk_pal";
static QueueHandle_t s_save_queue;
static TaskHandle_t s_save_task;
static volatile uint32_t s_sequence;
static volatile bool s_pending;

static uint32_t pet_store_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static bool pet_store_record_valid(const pet_store_record_t *record, size_t length)
{
    if (length != sizeof(*record) || record->magic != PET_STORE_MAGIC ||
        record->model_size != sizeof(record->model)) {
        return false;
    }
    if (record->crc != pet_store_crc32(record, offsetof(pet_store_record_t, crc))) {
        return false;
    }
    pet_model_t copy = record->model;
    return pet_model_sanitize(&copy);
}

static bool pet_store_read_slot(nvs_handle_t nvs, const char *key,
                                pet_store_record_t *record)
{
    size_t length = sizeof(*record);
    const esp_err_t err = nvs_get_blob(nvs, key, record, &length);
    return err == ESP_OK && pet_store_record_valid(record, length);
}

static void pet_store_task(void *context)
{
    (void)context;
    pet_model_t model;
    for (;;) {
        if (xQueueReceive(s_save_queue, &model, portMAX_DELAY) != pdTRUE) continue;

        pet_store_record_t record = {
            .magic = PET_STORE_MAGIC,
            .sequence = ++s_sequence,
            .model_size = sizeof(model),
            .model = model,
        };
        record.crc = pet_store_crc32(&record, offsetof(pet_store_record_t, crc));

        nvs_handle_t nvs;
        esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            const char *key = (record.sequence & 1U) ? "slot_a" : "slot_b";
            err = nvs_set_blob(nvs, key, &record, sizeof(record));
            if (err == ESP_OK) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "已保存 seq=%u", (unsigned)record.sequence);
        }
        s_pending = false;
    }
}

bool pet_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要重建");
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    if (s_save_queue) return true;

    s_save_queue = xQueueCreate(1, sizeof(pet_model_t));
    if (!s_save_queue) return false;
    if (xTaskCreate(pet_store_task, "pet_store", 3072, NULL, 3, &s_save_task) != pdPASS) {
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
        return false;
    }
    return true;
}

bool pet_store_load(pet_model_t *model)
{
    if (!model) return false;
    nvs_handle_t nvs;
    if (nvs_open(NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    pet_store_record_t a;
    pet_store_record_t b;
    const bool a_ok = pet_store_read_slot(nvs, "slot_a", &a);
    const bool b_ok = pet_store_read_slot(nvs, "slot_b", &b);
    nvs_close(nvs);
    if (!a_ok && !b_ok) return false;

    const pet_store_record_t *chosen = !a_ok ? &b
                                      : !b_ok ? &a
                                      : ((int32_t)(a.sequence - b.sequence) > 0 ? &a : &b);
    *model = chosen->model;
    s_sequence = chosen->sequence;
    return pet_model_sanitize(model);
}

bool pet_store_save_async(const pet_model_t *model)
{
    if (!s_save_queue || !model) return false;
    if (xQueueOverwrite(s_save_queue, model) != pdPASS) return false;
    s_pending = true;
    return true;
}

bool pet_store_pending(void)
{
    return s_pending;
}
