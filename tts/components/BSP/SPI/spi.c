#include "spi.h"

void spi2_init(void)
{
    esp_err_t ret=0;
    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI_GPIO_PIN,
        .miso_io_num = SPI_MISO_GPIO_PIN,
        .sclk_io_num = SPI_CLK_GPIO_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
}
void spi2_write_cmd(spi_device_handle_t handle,uint8_t cmd)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = (void*)0; // 命令
    esp_err_t ret = spi_device_transmit(handle, &t);
    ESP_ERROR_CHECK(ret);
}
void spi2_write_data(spi_device_handle_t handle,const uint8_t *data,int len)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    if (len == 0) return; // 没有数据要发送
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void*)1; // 数据
    esp_err_t ret = spi_device_transmit(handle, &t);
    ESP_ERROR_CHECK(ret);
}
uint8_t spi2_transfer_byte(spi_device_handle_t handle,uint8_t data)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_data[0] = data;
    spi_device_transmit(handle, &t);
    return t.rx_data[0];
}