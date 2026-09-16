#include "board.hpp"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace board
{

namespace
{

const char* const TAG = "board";
bool sGRelay[RELAY_COUNT] = {};
led_strip_handle_t sGStrip = nullptr;

} // namespace

void InitRelaysDeenergised()
{
    for(const gpio_num_t pin : RELAY_PINS)
    {
        gpio_reset_pin(pin);
        gpio_set_level(pin, 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
    }
}

void SetRelay(int pIndex, bool pEnergised)
{
    if(pIndex < 0 || pIndex >= RELAY_COUNT) return;
    sGRelay[pIndex] = pEnergised;
    gpio_set_level(RELAY_PINS[pIndex], pEnergised ? 1 : 0);
}

bool RelayEnergised(int pIndex)
{
    return pIndex >= 0 && pIndex < RELAY_COUNT && sGRelay[pIndex];
}

void InitBuzzer()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 2000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {};
    channel.gpio_num = BUZZER_PIN;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void BuzzerSet(bool pOn, uint32_t pHz, uint32_t pVolume)
{
    static const uint32_t DUTY[] = {0, 12, 80, 512}; // of 1024; a passive buzzer is loudest at 50 %
    if(pOn) ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, pHz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pOn ? DUTY[pVolume > 3 ? 3 : pVolume] : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void BuzzerTone(uint32_t pHz, uint32_t pMs)
{
    BuzzerSet(true, pHz);
    vTaskDelay(pdMS_TO_TICKS(pMs));
    BuzzerSet(false, pHz);
}

void InitLed()
{
    led_strip_config_t strip = {};
    strip.strip_gpio_num = RGB_PIN;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;

    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10 * 1000 * 1000;

    const esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &sGStrip);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "RGB LED init failed: %s", esp_err_to_name(err));
        sGStrip = nullptr;
        return;
    }
    led_strip_clear(sGStrip);
}

void LedRgb(uint8_t pR, uint8_t pG, uint8_t pB)
{
    if(sGStrip == nullptr) return;
    led_strip_set_pixel(sGStrip, 0, pR, pG, pB);
    led_strip_refresh(sGStrip);
}

void InitButton()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOOT_BUTTON_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

bool BootButtonPressed()
{
    return gpio_get_level(BOOT_BUTTON_PIN) == 0;
}

} // namespace board
