#ifndef BSP_IIS_H
#define BSP_IIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_types.h"
#include "esp_err.h"

/*
 * ESP32-C5 IIS/I2S PDM speaker bottom layer.
 *
 * Public hardware format:
 *   PCM16 mono at 24 kHz -> I2S PDM TX PCM2PDM
 *
 * This BSP owns the I2S/PDM channel, GPIO timing parameters and PA control.
 * Speaker modules should call these functions instead of using ESP-IDF I2S
 * driver APIs directly.
 */

#ifndef IIS_PDM_GPIO_CLK
#define IIS_PDM_GPIO_CLK GPIO_NUM_8
#endif

#ifndef IIS_PDM_GPIO_DATA
#define IIS_PDM_GPIO_DATA GPIO_NUM_7
#endif

#ifndef IIS_PDM_GPIO_DATA2
#define IIS_PDM_GPIO_DATA2 GPIO_NUM_NC
#endif

#ifndef IIS_PDM_CLK_INVERT
#define IIS_PDM_CLK_INVERT 0
#endif

#ifndef IIS_PDM_UPSAMPLE_FP
#define IIS_PDM_UPSAMPLE_FP 960
#endif

#ifndef IIS_PDM_UPSAMPLE_FS
#define IIS_PDM_UPSAMPLE_FS (IIS_SAMPLE_RATE_HZ / 100)
#endif

#ifndef IIS_GPIO_PA_CTL
#define IIS_GPIO_PA_CTL GPIO_NUM_1
#endif

#ifndef IIS_PA_ENABLE_LEVEL
#define IIS_PA_ENABLE_LEVEL 1
#endif

#ifndef IIS_PORT
#define IIS_PORT I2S_NUM_0
#endif

#ifndef IIS_SAMPLE_RATE_HZ
#define IIS_SAMPLE_RATE_HZ 24000
#endif

#ifndef IIS_BITS_PER_SAMPLE
#define IIS_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#endif

#ifndef IIS_PDM_SLOT_MODE
#define IIS_PDM_SLOT_MODE I2S_SLOT_MODE_MONO
#endif

#ifndef IIS_PDM_SLOT_MASK
#define IIS_PDM_SLOT_MASK I2S_PDM_SLOT_LEFT
#endif

#ifndef IIS_DMA_DESC_NUM
#define IIS_DMA_DESC_NUM 8
#endif

#ifndef IIS_DMA_FRAME_NUM
#define IIS_DMA_FRAME_NUM 512
#endif

#define IIS_EFFECTIVE_DMA_DESC_NUM \
    ((IIS_DMA_DESC_NUM) < 8 ? 8 : (IIS_DMA_DESC_NUM))

#define IIS_EFFECTIVE_DMA_FRAME_NUM \
    ((IIS_DMA_FRAME_NUM) < 512 ? 512 : (IIS_DMA_FRAME_NUM))

#define IIS_EXPECTED_PDM_CLOCK_HZ 6144000UL
#define IIS_PDM_CLOCK_LOW_LIMIT_HZ ((IIS_EXPECTED_PDM_CLOCK_HZ * 95UL) / 100UL)

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t iis_init(void);
esp_err_t iis_start(void);
esp_err_t iis_write(const void *data,
                    size_t bytes,
                    size_t *bytes_written,
                    uint32_t timeout_ms);
esp_err_t iis_stop(void);
esp_err_t iis_get_info(i2s_chan_info_t *out_info);
bool iis_is_started(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_IIS_H */
