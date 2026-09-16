#include "hal/rs485_uart.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace hal
{

namespace
{
const char* const TAG = "rs485";
}

bool Rs485Uart::Init(uart_port_t pPort, int pTxPin, int pRxPin, int pBaud)
{
    mPort = pPort;
    uart_config_t cfg = {};
    cfg.baud_rate = pBaud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_driver_install(pPort, 256, 0, 0, nullptr, 0);
    if(err == ESP_OK) err = uart_param_config(pPort, &cfg);
    if(err == ESP_OK) err = uart_set_pin(pPort, pTxPin, pRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if(err == ESP_OK) err = uart_set_mode(pPort, UART_MODE_UART);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return false;
    }
    mReady = true;
    ESP_LOGI(TAG, "UART%d %d 8N1, TX GPIO%d RX GPIO%d", static_cast<int>(pPort), pBaud, pTxPin, pRxPin);
    return true;
}

void Rs485Uart::Write(std::span<const uint8_t> data)
{
    if(!mReady) return;
    uart_write_bytes(mPort, data.data(), data.size());
    uart_wait_tx_done(mPort, pdMS_TO_TICKS(50));
}

std::size_t Rs485Uart::Read(std::span<uint8_t> out, uint32_t timeout_ms)
{
    if(!mReady || out.empty()) return 0;
    const int n = uart_read_bytes(mPort, out.data(), out.size(), pdMS_TO_TICKS(timeout_ms));
    if(n <= 0) return 0;
    if(mMute) return 0;
    if(mCorrupt) out[0] ^= 0x55;
    return static_cast<std::size_t>(n);
}

void Rs485Uart::DiscardInput()
{
    if(mReady) uart_flush_input(mPort);
}

} // namespace hal
