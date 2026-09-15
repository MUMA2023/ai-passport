// main/pet_audio.c —— 代码合成提示音 + 独立播放线程。
#include "pet_audio.h"

#include <stddef.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PET_AUDIO_SAMPLE_RATE 16000U
#define PET_AUDIO_CHUNK_SAMPLES 256U
#define PET_AUDIO_QUEUE_DEPTH 4U

typedef enum {
    PET_WAVE_SINE = 0,
    PET_WAVE_TRIANGLE,
    PET_WAVE_SQUARE,
} pet_wave_t;

typedef struct {
    uint16_t frequency;  // Hz，0 表示静音间隔
    uint16_t millis;
    pet_wave_t wave;
} pet_note_t;

typedef struct {
    const pet_note_t *notes;
    uint8_t count;
} pet_jingle_t;

/* 每段音效都是一小串音符；频率取常见的五声音阶，听感更柔和。 */
static const pet_note_t JINGLE_BOOT[] = {
    {784, 90, PET_WAVE_TRIANGLE}, {988, 90, PET_WAVE_TRIANGLE}, {1319, 150, PET_WAVE_SINE},
};
static const pet_note_t JINGLE_PET[] = {
    {880, 70, PET_WAVE_SINE}, {1175, 110, PET_WAVE_SINE},
};
static const pet_note_t JINGLE_FEED[] = {
    {659, 80, PET_WAVE_TRIANGLE}, {880, 120, PET_WAVE_TRIANGLE},
};
static const pet_note_t JINGLE_PLAY[] = {
    {784, 70, PET_WAVE_SQUARE}, {988, 70, PET_WAVE_SQUARE}, {1175, 130, PET_WAVE_SQUARE},
};
static const pet_note_t JINGLE_HUNGRY[] = {
    {523, 110, PET_WAVE_TRIANGLE}, {392, 160, PET_WAVE_TRIANGLE},
};
static const pet_note_t JINGLE_SLEEP[] = {
    {440, 130, PET_WAVE_SINE}, {330, 200, PET_WAVE_SINE},
};
static const pet_note_t JINGLE_WAKE[] = {
    {392, 80, PET_WAVE_SINE}, {523, 80, PET_WAVE_SINE}, {659, 140, PET_WAVE_SINE},
};
static const pet_note_t JINGLE_MOVE[] = {
    {1047, 40, PET_WAVE_SQUARE},
};
static const pet_note_t JINGLE_CONFIRM[] = {
    {1319, 60, PET_WAVE_SQUARE}, {1568, 90, PET_WAVE_SQUARE},
};
static const pet_note_t JINGLE_DENY[] = {
    {220, 120, PET_WAVE_SQUARE}, {196, 160, PET_WAVE_SQUARE},
};

static const pet_jingle_t JINGLES[PET_SOUND_COUNT] = {
    [PET_SOUND_BOOT] = {JINGLE_BOOT, sizeof(JINGLE_BOOT) / sizeof(JINGLE_BOOT[0])},
    [PET_SOUND_PET] = {JINGLE_PET, sizeof(JINGLE_PET) / sizeof(JINGLE_PET[0])},
    [PET_SOUND_FEED] = {JINGLE_FEED, sizeof(JINGLE_FEED) / sizeof(JINGLE_FEED[0])},
    [PET_SOUND_PLAY] = {JINGLE_PLAY, sizeof(JINGLE_PLAY) / sizeof(JINGLE_PLAY[0])},
    [PET_SOUND_HUNGRY] = {JINGLE_HUNGRY, sizeof(JINGLE_HUNGRY) / sizeof(JINGLE_HUNGRY[0])},
    [PET_SOUND_SLEEP] = {JINGLE_SLEEP, sizeof(JINGLE_SLEEP) / sizeof(JINGLE_SLEEP[0])},
    [PET_SOUND_WAKE] = {JINGLE_WAKE, sizeof(JINGLE_WAKE) / sizeof(JINGLE_WAKE[0])},
    [PET_SOUND_MOVE] = {JINGLE_MOVE, sizeof(JINGLE_MOVE) / sizeof(JINGLE_MOVE[0])},
    [PET_SOUND_CONFIRM] = {JINGLE_CONFIRM, sizeof(JINGLE_CONFIRM) / sizeof(JINGLE_CONFIRM[0])},
    [PET_SOUND_DENY] = {JINGLE_DENY, sizeof(JINGLE_DENY) / sizeof(JINGLE_DENY[0])},
};

static const char *TAG = "pet_audio";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile uint8_t s_volume = 60;
static volatile bool s_ready;

static void pet_audio_pause(uint16_t millis)
{
    if (millis == 0) return;
    int16_t silence[PET_AUDIO_CHUNK_SAMPLES] = {0};
    uint32_t remaining = (uint32_t)PET_AUDIO_SAMPLE_RATE * millis / 1000U;
    while (remaining > 0) {
        const uint32_t count = remaining < PET_AUDIO_CHUNK_SAMPLES ? remaining
                                                                   : PET_AUDIO_CHUNK_SAMPLES;
        bsp_audio_write(silence, count * sizeof(silence[0]));
        remaining -= count;
    }
}

static int16_t pet_audio_sample(uint32_t phase, pet_wave_t wave, int16_t amplitude)
{
    /* phase 为 32 位定点，高 16 位即一个周期内的位置。 */
    const int32_t point = (int32_t)(phase >> 16);
    int32_t value;
    switch (wave) {
    case PET_WAVE_SQUARE:
        value = point < 32768 ? 32767 : -32768;
        break;
    case PET_WAVE_TRIANGLE:
        value = point < 32768 ? point * 2 - 32768 : 98303 - point * 2;
        break;
    case PET_WAVE_SINE:
    default: {
        /* 用抛物线近似正弦，避免引入浮点与查表。 */
        const int32_t x = point - 32768;
        const int32_t normalized = x / 2;                       /* -16384..16383 */
        const int32_t t = normalized < 0 ? -normalized : normalized;
        const int32_t shaped = (t * (32768 - t)) >> 13;         /* 0..32767 */
        value = normalized < 0 ? -shaped : shaped;
        break;
    }
    }
    return (int16_t)(value * amplitude / 32768);
}

static void pet_audio_render_note(uint16_t frequency, uint16_t millis, pet_wave_t wave)
{
    const uint8_t volume = s_volume;
    if (volume == 0) return;
    if (frequency == 0) {
        pet_audio_pause(millis);
        return;
    }

    const uint32_t total = (uint32_t)PET_AUDIO_SAMPLE_RATE * millis / 1000U;
    const uint32_t step = (uint32_t)(((uint64_t)frequency << 32) / PET_AUDIO_SAMPLE_RATE);
    const uint32_t edge = total / 6U + 1U;
    const int16_t peak = (int16_t)(1200 + (int32_t)volume * 1300 / 100);

    int16_t samples[PET_AUDIO_CHUNK_SAMPLES];
    uint32_t phase = 0;
    uint32_t rendered = 0;
    while (rendered < total) {
        const uint32_t count = total - rendered < PET_AUDIO_CHUNK_SAMPLES
                                   ? total - rendered
                                   : PET_AUDIO_CHUNK_SAMPLES;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t position = rendered + i;
            /* 淡入淡出，避免爆音。 */
            const uint32_t envelope = position < edge
                                          ? position * 100U / edge
                                          : (total - position < edge
                                                 ? (total - position) * 100U / edge
                                                 : 100U);
            samples[i] = pet_audio_sample(phase, wave, (int16_t)(peak * envelope / 100));
            phase += step;
        }
        bsp_audio_write(samples, count * sizeof(samples[0]));
        rendered += count;
    }
}

static void pet_audio_task(void *context)
{
    (void)context;
    pet_sound_t sound;
    for (;;) {
        if (xQueueReceive(s_queue, &sound, portMAX_DELAY) != pdTRUE) continue;
        if (sound >= PET_SOUND_COUNT) continue;
        const pet_jingle_t *jingle = &JINGLES[sound];
        for (uint8_t i = 0; i < jingle->count; i++) {
            pet_audio_render_note(jingle->notes[i].frequency, jingle->notes[i].millis,
                                  jingle->notes[i].wave);
        }
    }
}

bool pet_audio_init(void)
{
    if (s_ready) return true;
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频不可用，音效将被忽略");
        return false;
    }
    if (bsp_audio_set_format(PET_AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "采样格式设置失败，音效将被忽略");
        return false;
    }
    bsp_audio_set_volume(s_volume);

    s_queue = xQueueCreate(PET_AUDIO_QUEUE_DEPTH, sizeof(pet_sound_t));
    if (!s_queue) return false;
    /* 优先级低于按键与 LVGL，避免抢占交互响应。 */
    if (xTaskCreate(pet_audio_task, "pet_audio", 4096, NULL, 3, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    s_ready = true;
    return true;
}

void pet_audio_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
    bsp_audio_set_volume(s_volume);
}

uint8_t pet_audio_volume(void)
{
    return s_volume;
}

void pet_audio_play(pet_sound_t sound)
{
    if (!s_ready || !s_queue || sound >= PET_SOUND_COUNT) return;
    (void)xQueueSend(s_queue, &sound, 0);
}
