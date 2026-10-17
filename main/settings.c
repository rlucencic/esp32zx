#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "zx"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = ZX_MODEL_128;
    settings.joystick = ZX_JOYSTICK_KEMPSTON;
    settings.tape_fast = 1;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 < ZX_MODEL_COUNT) {
        settings.model = u8;
    }
    if (nvs_get_u8(h, "joystick", &u8) == ESP_OK && u8 < ZX_JOYSTICK_COUNT) {
        settings.joystick = u8;
    }
    if (nvs_get_u8(h, "tape_fast", &u8) == ESP_OK) {
        settings.tape_fast = u8 != 0;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "joystick", settings.joystick);
    nvs_set_u8(h, "tape_fast", settings.tape_fast);
    nvs_commit(h);
    nvs_close(h);
}
