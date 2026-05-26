#include "laba.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/sdm.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-SensairShuttle-MainBoard-V1.0 audio nets:
// - PA_CTL: PA enable
// - PDM_P/PDM_N: differential PDM output to NS4150B input network
//
// If your wiring is different, change these GPIO numbers accordingly.
#define LABA_GPIO_PA_CTL   GPIO_NUM_1
#define LABA_GPIO_PDM_P    GPIO_NUM_7
#define LABA_GPIO_PDM_N    GPIO_NUM_8

#define LABA_SAMPLE_RATE_HZ       16000
#define LABA_SDM_CARRIER_HZ       (1 * 1000 * 1000)
#define LABA_TIMER_RES_HZ         (4 * 1000 * 1000)  // 4 MHz => 250 ticks @ 16 kHz
#define LABA_TIMER_ALARM_TICKS    (LABA_TIMER_RES_HZ / LABA_SAMPLE_RATE_HZ)

static bool s_inited = false;

// Q15 volume: 0..32768 (0.0..1.0)
static volatile int32_t s_volume_q15 = 32768;

static sdm_channel_handle_t s_chan_p = NULL;
static sdm_channel_handle_t s_chan_n = NULL;
static gptimer_handle_t s_timer = NULL;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile const int16_t *s_play_samples = NULL;
static volatile size_t s_play_total = 0;
static volatile size_t s_play_index = 0;
static TaskHandle_t s_wait_task = NULL;
static volatile bool s_playing = false;

static inline int8_t pcm16_to_density(int16_t sample)
{
    // Map [-32768, 32767] -> roughly [-128, 127], with a safer range.
    int32_t v = ((int32_t)sample * (int32_t)s_volume_q15) >> 15;
    int32_t d = v / 256;
    if (d > 90) d = 90;
    if (d < -90) d = -90;
    return (int8_t)d;
}

static inline void laba_set_outputs_isr(int8_t density)
{
    // Differential: PDM_P = +d, PDM_N = -d
    (void)sdm_channel_set_pulse_density(s_chan_p, density);
    (void)sdm_channel_set_pulse_density(s_chan_n, (int8_t)(-density));
}

static bool IRAM_ATTR laba_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    const int16_t *samples = (const int16_t *)s_play_samples;
    size_t total = s_play_total;
    size_t idx = s_play_index;

    if (!s_playing || samples == NULL || idx >= total) {
        laba_set_outputs_isr(0);
        return false;
    }

    int8_t density = pcm16_to_density(samples[idx]);
    laba_set_outputs_isr(density);
    s_play_index = idx + 1;

    if (idx + 1 >= total) {
        s_playing = false;
        if (s_wait_task != NULL) {
            BaseType_t woke = pdFALSE;
            vTaskNotifyGiveFromISR(s_wait_task, &woke);
            return woke == pdTRUE;
        }
    }

    return false;
}

static esp_err_t laba_sdm_init(void)
{
    sdm_config_t cfg_p = {
        .clk_src = SDM_CLK_SRC_DEFAULT,
        .sample_rate_hz = LABA_SDM_CARRIER_HZ,
        .gpio_num = LABA_GPIO_PDM_P,
        .flags = {
            .invert_out = 0,
            .io_loop_back = 0,
        },
    };
    sdm_config_t cfg_n = cfg_p;
    cfg_n.gpio_num = LABA_GPIO_PDM_N;

    esp_err_t err = sdm_new_channel(&cfg_p, &s_chan_p);
    if (err != ESP_OK) return err;
    err = sdm_new_channel(&cfg_n, &s_chan_n);
    if (err != ESP_OK) return err;
    err = sdm_channel_enable(s_chan_p);
    if (err != ESP_OK) return err;
    err = sdm_channel_enable(s_chan_n);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

static esp_err_t laba_timer_init(void)
{
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = LABA_TIMER_RES_HZ,
        .intr_priority = 0,
        .flags = {
            .intr_shared = 0,
            .allow_pd = 0,
        },
    };
    esp_err_t err = gptimer_new_timer(&timer_cfg, &s_timer);
    if (err != ESP_OK) return err;

    gptimer_event_callbacks_t cbs = {
        .on_alarm = laba_timer_cb,
    };
    err = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (err != ESP_OK) return err;

    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = LABA_TIMER_ALARM_TICKS,
        .reload_count = 0,
        .flags = {
            .auto_reload_on_alarm = 1,
        },
    };
    err = gptimer_set_alarm_action(s_timer, &alarm_cfg);
    if (err != ESP_OK) return err;

    return gptimer_enable(s_timer);
}

void laba_init(void)
{
    if (s_inited) {
        return;
    }

    gpio_set_direction(LABA_GPIO_PA_CTL, GPIO_MODE_OUTPUT);
    gpio_set_level(LABA_GPIO_PA_CTL, 1);

    if (laba_sdm_init() != ESP_OK) {
        // Leave PA on but output zero density to avoid loud noise.
        return;
    }
    if (laba_timer_init() != ESP_OK) {
        laba_set_outputs_isr(0);
        return;
    }

    laba_set_outputs_isr(0);
    s_inited = true;
}

void laba_set_volume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    s_volume_q15 = (int32_t)(volume * 32768.0f + 0.5f);
}

void laba_play_pcm16_mono(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return;
    }
    if (!s_inited) {
        laba_init();
    }
    if (!s_inited || s_timer == NULL || s_chan_p == NULL || s_chan_n == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_wait_task = xTaskGetCurrentTaskHandle();
    s_play_samples = samples;
    s_play_total = sample_count;
    s_play_index = 0;
    s_playing = true;
    portEXIT_CRITICAL(&s_lock);

    gptimer_set_raw_count(s_timer, 0);
    gptimer_start(s_timer);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    gptimer_stop(s_timer);

    portENTER_CRITICAL(&s_lock);
    s_play_samples = NULL;
    s_play_total = 0;
    s_play_index = 0;
    s_wait_task = NULL;
    portEXIT_CRITICAL(&s_lock);

    laba_set_outputs_isr(0);
}
