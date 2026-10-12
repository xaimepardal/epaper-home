// Conexión WiFi rápida para despertar, conectar y volver a dormir.
//
// Lo que más tarda al conectar es buscar el AP (escanear los 13 canales) y
// pedir IP por DHCP. Aquí:
//  - El canal y el BSSID del AP de la última conexión buena se guardan en
//    memoria RTC; el siguiente despertar conecta directamente a ellos. Si falla,
//    se olvidan y se vuelve a intentar buscando el AP (en CONFIG_WIFI_CANAL si
//    está fijado, o en todos).
//  - Con CONFIG_WIFI_IP no hay DHCP: la IP está lista al asociarse.
//  - WIFI_STORAGE_RAM: la configuración no se escribe en NVS en cada conexión.
//  - WIFI_PS_NONE: sin ahorro de energía del módem mientras se descarga; se
//    está conectado pocos segundos y así la transferencia es más rápida.

#include "wifi.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

static constexpr EventBits_t BIT_LISTA = BIT0;  // asociada y con IP
static constexpr EventBits_t BIT_FALLO = BIT1;
static constexpr bool IP_ESTATICA = CONFIG_WIFI_IP[0] != '\0';

// Último AP bueno; sobrevive al deep sleep, no a otros reinicios
static constexpr uint32_t CACHE_MAGIC = 0x57494649;  // "WIFI"
struct cache_ap {
    uint32_t magic;
    uint8_t bssid[6];
    uint8_t canal;
};
RTC_DATA_ATTR static cache_ap cache;

static EventGroupHandle_t eventos;
static bool iniciada;
static bool reconectar;  // tras la primera conexión buena (modo pruebas: horas conectado)

static void manejador(void *, esp_event_base_t base, int32_t id, void *datos)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *d = static_cast<wifi_event_sta_disconnected_t *>(datos);
        ESP_LOGW(TAG, "Desconectada (motivo %d)", d->reason);
        xEventGroupSetBits(eventos, BIT_FALLO);
        if (reconectar) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && IP_ESTATICA) {
        // El handler por defecto de esp_netif (registrado antes) ya ha levantado
        // la interfaz con la IP fija
        xEventGroupSetBits(eventos, BIT_LISTA);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(eventos, BIT_LISTA);
    }
}

static void configurar_ip_estatica(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip{};
    esp_netif_dns_info_t dns{};
    if (esp_netif_str_to_ip4(CONFIG_WIFI_IP, &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_WIFI_MASCARA, &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_WIFI_GATEWAY, &ip.gw) != ESP_OK) {
        ESP_LOGE(TAG, "IP estática mal escrita en menuconfig");
        abort();
    }
    const char *servidor_dns = CONFIG_WIFI_DNS[0] != '\0' ? CONFIG_WIFI_DNS : CONFIG_WIFI_GATEWAY;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4(servidor_dns, &dns.ip.u_addr.ip4);
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
}

// Un intento de conexión. Con `directo`, al BSSID y canal guardados.
static bool intentar(bool directo, int timeout_ms)
{
    wifi_config_t cfg{};
    strncpy(reinterpret_cast<char *>(cfg.sta.ssid), CONFIG_WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy(reinterpret_cast<char *>(cfg.sta.password), CONFIG_WIFI_CLAVE, sizeof(cfg.sta.password));
    // Con contraseña no se acepta un AP abierto o WEP con el mismo nombre
    cfg.sta.threshold.authmode = CONFIG_WIFI_CLAVE[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    if (directo) {
        memcpy(cfg.sta.bssid, cache.bssid, sizeof(cache.bssid));
        cfg.sta.bssid_set = true;
        cfg.sta.channel = cache.canal;
    } else {
        cfg.sta.channel = CONFIG_WIFI_CANAL;  // 0 = todos
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    xEventGroupClearBits(eventos, BIT_LISTA | BIT_FALLO);
    if (esp_wifi_connect() != ESP_OK) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(eventos, BIT_LISTA | BIT_FALLO, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_LISTA) {
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(cache.bssid, ap.bssid, sizeof(cache.bssid));
            cache.canal = ap.primary;
            cache.magic = CACHE_MAGIC;
        }
        return true;
    }
    esp_wifi_disconnect();
    return false;
}

bool wifi_conectar()
{
    esp_err_t err = nvs_flash_init();  // la WiFi guarda ahí la calibración RF
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    eventos = xEventGroupCreate();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (IP_ESTATICA) {
        configurar_ip_estatica(netif);
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, manejador, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, manejador, nullptr));
    ESP_ERROR_CHECK(esp_wifi_start());
    iniciada = true;
    esp_wifi_set_ps(WIFI_PS_NONE);

    bool ok = false;
    if (cache.magic == CACHE_MAGIC) {
        ESP_LOGI(TAG, "Conectando a %s (canal %u, AP guardado)", CONFIG_WIFI_SSID, cache.canal);
        ok = intentar(true, 3000);
        if (!ok) {
            cache.magic = 0;  // el AP ha cambiado de canal o ya no está
        }
    }
    for (int i = 1; !ok && i <= CONFIG_WIFI_INTENTOS; ++i) {
        ESP_LOGI(TAG, "Conectando a %s (canal %d, intento %d)", CONFIG_WIFI_SSID, CONFIG_WIFI_CANAL, i);
        ok = intentar(false, 10000);
    }
    if (ok) {
        reconectar = true;
        return true;
    }
    ESP_LOGE(TAG, "No se pudo conectar a %s", CONFIG_WIFI_SSID);
    return false;
}

void wifi_desconectar()
{
    if (iniciada) {
        reconectar = false;
        esp_wifi_disconnect();
        esp_wifi_stop();
        iniciada = false;
    }
}
