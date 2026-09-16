// ReefDO firmware entry: relays off first, then storage, the App, the indicator, the console, the network.
#include "board.hpp"
#include "console_cmds.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "hal/nvs_store.hpp"
#include "indicator.hpp"
#include "net.hpp"
#include "notify.hpp"
#include "sampler.hpp"
#include "web.hpp"

namespace
{
const char* const TAG = "reefdo";
}

extern "C" void app_main()
{
    board::InitRelaysDeenergised(); // before anything else: fail-safe state
    ESP_LOGI(TAG, "ReefDO %s: all six relays de-energised", esp_app_get_description()->version);

    board::InitLed();
    board::InitBuzzer();
    board::InitButton();
    board::LedRgb(40, 40, 40); // white: booting

    hal::nvs::Init();
    if(!sampler::Start()) ESP_LOGE(TAG, "a log partition is unusable: running without it");
    indicator::Start();
    console::Start();

    // Everything below is an observer: the ladder runs whether or not it comes up.
    notify::Start();
    net::Start();
    web::Start();
}
