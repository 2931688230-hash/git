#ifndef __SPI_H__
#define __SPI_H__
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#define SPI_MOSI_GPIO_PIN   GPIO_NUM_7
#define SPI_MISO_GPIO_PIN   GPIO_NUM_2
#define SPI_CLK_GPIO_PIN   GPIO_NUM_6

void spi2_init(void);
void spi2_write_cmd(spi_device_handle_t handle,uint8_t cmd);
void spi2_write_data(spi_device_handle_t handle,const uint8_t *data,int len); /*SPI发送命令*/
uint8_t spi2_transfer_byte(spi_device_handle_t handle,uint8_t byte);
#endif