#pragma once
// reefdo::probe::IUart over an ESP-IDF UART. The board's RS485 transceiver switches direction by itself.
#include <cstddef>
#include <cstdint>
#include <span>

#include "reefdo/probe.hpp"

#include "driver/uart.h"

namespace hal
{

class Rs485Uart : public reefdo::probe::IUart
{
public:
    bool Init(uart_port_t pPort, int pTxPin, int pRxPin, int pBaud);

    void Write(std::span<const uint8_t> pData) override;
    std::size_t Read(std::span<uint8_t> pOut, uint32_t pTimeoutMs) override;
    void DiscardInput() override;

    // Fault injection for testing: drop everything received, or flip a byte in every frame.
    void SetMute(bool pOn) { mMute = pOn; }
    void SetCorrupt(bool pOn) { mCorrupt = pOn; }

private:
    uart_port_t mPort = UART_NUM_1;
    bool mReady = false;
    bool mMute = false;
    bool mCorrupt = false;
};

} // namespace hal
