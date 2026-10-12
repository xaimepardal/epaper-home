#include "battery.hpp"

#include <algorithm>
#include <array>
#include <iterator>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bateria";

static constexpr int GPIO_ADC = 1;
static constexpr gpio_num_t GPIO_ENABLE = GPIO_NUM_21;
static constexpr int SAMPLES = 16;

// Activa el divisor el tiempo justo de medir. Fuera de eso GPIO21 queda a 0 y
// retenido (gpio_hold), también en deep sleep: el divisor no gasta batería.
static void enable_divider(bool enabled)
{
    gpio_hold_dis(GPIO_ENABLE);
    gpio_set_direction(GPIO_ENABLE, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_ENABLE, enabled);
    if (!enabled) {
        gpio_hold_en(GPIO_ENABLE);
    }
}

int battery_mv()
{
    adc_unit_t unit;
    adc_channel_t channel;
    adc_oneshot_unit_handle_t adc = nullptr;
    adc_cali_handle_t cali = nullptr;
    int result = -1;

    adc_oneshot_unit_init_cfg_t unit_cfg{};
    adc_oneshot_chan_cfg_t chan_cfg{.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_cali_curve_fitting_config_t cali_cfg{};
    if (adc_oneshot_io_to_channel(GPIO_ADC, &unit, &channel) != ESP_OK) {
        return -1;
    }
    unit_cfg.unit_id = unit;
    cali_cfg = {.unit_id = unit, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, channel, &chan_cfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo preparar el ADC");
    } else {
        enable_divider(true);
        vTaskDelay(pdMS_TO_TICKS(10));  // que se estabilice el divisor (Seeed espera 5 ms)
        std::array<int, SAMPLES> mv{};
        int valid = 0;
        for (int &m : mv) {
            if (adc_oneshot_get_calibrated_result(adc, cali, channel, &m) == ESP_OK) {
                ++valid;
            } else {
                m = -1;
            }
        }
        enable_divider(false);
        if (valid == SAMPLES) {
            // Media de la mitad central: descarta picos del ruido de la placa
            std::ranges::sort(mv);
            int sum = 0;
            for (auto it = mv.begin() + SAMPLES / 4; it != mv.end() - SAMPLES / 4; ++it) {
                sum += *it;
            }
            result = 2 * sum / (SAMPLES / 2);  // divisor 1/2
        }
    }
    if (cali != nullptr) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    if (adc != nullptr) {
        adc_oneshot_del_unit(adc);
    }
    ESP_LOGI(TAG, "Batería: %d mV", result);
    return result;
}

int battery_percent(int mv)
{
    // Tensión en reposo de una LiPo frente a carga restante (curva típica)
    struct point {
        int mv;
        int pct;
    };
    static constexpr point curve[] = {
        {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 70}, {3950, 60},
        {3910, 50},  {3870, 40}, {3850, 30}, {3840, 20}, {3800, 10}, {3700, 5},  {3300, 0},
    };
    // Fuera de esto no es una LiPo conectada al divisor (lectura errónea)
    if (mv < 2500 || mv > 4500) {
        return -1;
    }
    if (mv >= curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < std::size(curve); ++i) {
        if (mv >= curve[i].mv) {
            const auto &a = curve[i];
            const auto &b = curve[i - 1];
            return a.pct + (mv - a.mv) * (b.pct - a.pct) / (b.mv - a.mv);
        }
    }
    return 0;
}
