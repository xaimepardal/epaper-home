// Muestra en el ePaper del reTerminal E1001 el texto o la imagen recibidos por MQTT,
// pensado para batería: cada CONFIG_INTERVALO_S el ESP32 despierta, recoge el
// último mensaje, pinta sólo si la imagen cambia y vuelve a deep sleep.
//
// Refresco parcial tras deep sleep: el último frame pintado se guarda en
// LittleFS y, al despertar, se carga en la RAM de "imagen anterior" del
// controlador (restore_baseline, parche en components/idfxx_epaper*). Sólo se
// confía en ese frame si el ESP32 viene de deep sleep y el último pintado
// terminó bien; en cualquier otro caso se hace un refresco completo.

#include <idfxx/epaper/mono_framebuffer>
#include <idfxx/epaper/uc8179>
#include <idfxx/spi/master>
#include <idfxx/gfx.hpp>
#include <idfxx/font/spleen.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_crc.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "miniz.h"  // tinfl (inflate) de la ROM
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "epaper_mqtt";

// Panel 7.5" 800x480 (valores por defecto de uc8179::config)
static constexpr size_t ANCHO = 800;
static constexpr size_t ALTO = 480;

// spleen 8x16 a escala 3 -> 24x48 px por carácter
static constexpr unsigned ESCALA = 3;
static constexpr size_t MARGEN = 16;
static constexpr size_t MAX_TEXTO = 512;
// Imagen: zlib de 800x480 a 1 bpp (48000 bytes). Comprimida no debería
// pasar de esto ni en el peor caso (deflate añade poco a datos incompresibles).
static constexpr size_t MAX_IMAGEN_Z = 64 * 1024;

// ---------------------------------------------------------------------------
// Estado que sobrevive al deep sleep (y a nada más: cualquier otro reset lo
// invalida, y entonces no se sabe qué hay en el cristal).
// ---------------------------------------------------------------------------
static constexpr uint32_t RTC_MAGIC = 0x45504D51;
struct estado_rtc {
    uint32_t magic;          // RTC_MAGIC = el frame guardado coincide con el cristal
    uint32_t parciales;      // parciales desde el último completo
    uint32_t s_desde_full;   // segundos (aprox.) desde el último completo
};
RTC_DATA_ATTR static estado_rtc rtc;

// ---------------------------------------------------------------------------
// Último frame en LittleFS
// ---------------------------------------------------------------------------
static constexpr const char *RUTA_FRAME = "/lfs/frame.bin";
static constexpr const char *RUTA_TMP = "/lfs/frame.tmp";
static constexpr uint32_t FRAME_MAGIC = 0x46524D31;  // "FRM1"
struct cabecera_frame {
    uint32_t magic;
    uint16_t ancho;
    uint16_t alto;
    uint32_t crc;
};

static bool montar_littlefs()
{
    esp_vfs_littlefs_conf_t conf{};
    conf.base_path = "/lfs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar LittleFS: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool cargar_frame(idfxx::epaper::mono_framebuffer &fb)
{
    FILE *f = fopen(RUTA_FRAME, "rb");
    if (f == nullptr) {
        return false;
    }
    cabecera_frame cab{};
    auto datos = fb.data();
    bool ok = fread(&cab, sizeof(cab), 1, f) == 1 && cab.magic == FRAME_MAGIC && cab.ancho == fb.width() &&
              cab.alto == fb.height() && fread(datos.data(), 1, datos.size(), f) == datos.size() &&
              esp_rom_crc32_le(0, datos.data(), datos.size()) == cab.crc;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "Frame guardado inválido");
    }
    return ok;
}

// Escritura atómica: fichero temporal + rename, así un corte a mitad nunca deja
// un frame a medias con apariencia de válido.
static bool guardar_frame(const idfxx::epaper::mono_framebuffer &fb)
{
    auto datos = fb.data();
    cabecera_frame cab{FRAME_MAGIC, static_cast<uint16_t>(fb.width()), static_cast<uint16_t>(fb.height()),
                       esp_rom_crc32_le(0, datos.data(), datos.size())};
    FILE *f = fopen(RUTA_TMP, "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fwrite(&cab, sizeof(cab), 1, f) == 1 && fwrite(datos.data(), 1, datos.size(), f) == datos.size() &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        ok = rename(RUTA_TMP, RUTA_FRAME) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "No se pudo guardar el frame");
        remove(RUTA_TMP);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------

// Parte el texto en líneas de como mucho `cols` caracteres, respetando '\n'
// y cortando por palabras cuando se puede.
static std::vector<std::string> ajustar_lineas(const std::string &texto, size_t cols)
{
    std::vector<std::string> lineas;
    size_t inicio = 0;
    while (inicio <= texto.size()) {
        size_t fin = texto.find('\n', inicio);
        if (fin == std::string::npos) {
            fin = texto.size();
        }
        std::string parrafo = texto.substr(inicio, fin - inicio);
        while (parrafo.size() > cols) {
            size_t corte = parrafo.rfind(' ', cols);
            if (corte == std::string::npos || corte == 0) {
                corte = cols;
            }
            lineas.push_back(parrafo.substr(0, corte));
            parrafo.erase(0, parrafo[corte] == ' ' ? corte + 1 : corte);
        }
        lineas.push_back(parrafo);
        inicio = fin + 1;
    }
    return lineas;
}

static void dibujar_texto(idfxx::epaper::mono_framebuffer &fb, const std::string &texto)
{
    const auto &fuente = idfxx::font::spleen_8x16;
    const size_t ancho_char = fuente.width * ESCALA;
    const size_t alto_char = fuente.height * ESCALA;
    const size_t cols = (fb.width() - 2 * MARGEN) / ancho_char;
    const size_t filas = (fb.height() - 2 * MARGEN) / alto_char;

    idfxx::gfx::canvas canvas(fb);
    canvas.clear();
    auto lineas = ajustar_lineas(texto, cols);
    size_t y = MARGEN;
    for (size_t i = 0; i < lineas.size() && i < filas; ++i, y += alto_char) {
        canvas.draw_text(fuente, MARGEN, y, lineas[i], true, ESCALA);
    }
}

// Descomprime una imagen zlib directamente en el framebuffer. Falla si el
// stream está corrupto (zlib lleva Adler-32) o no mide exactamente un frame.
static bool descomprimir_imagen(const std::string &z, idfxx::epaper::mono_framebuffer &fb)
{
    auto salida = fb.data();
    // El estado del descompresor (~11 KB) va al heap: no cabe en la pila de app_main
    auto dec = std::make_unique<tinfl_decompressor>();
    tinfl_init(dec.get());
    size_t len_in = z.size();
    size_t len_out = salida.size();
    tinfl_status st = tinfl_decompress(dec.get(), reinterpret_cast<const mz_uint8 *>(z.data()), &len_in,
                                       salida.data(), salida.data(), &len_out,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || len_out != salida.size()) {
        ESP_LOGE(TAG, "Imagen inválida (estado %d, %u bytes; se esperaban %u)", st, (unsigned)len_out,
                 (unsigned)salida.size());
        return false;
    }
    return true;
}

// La microSD comparte el bus SPI; sin esto puede bloquear BUSY (workaround de Seeed)
static void sd_release_bus()
{
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_16, 1);  // SD_EN
    gpio_set_level(GPIO_NUM_14, 1);  // SD_CS inactivo
}

// Pinta `nuevo`. Con `anterior` (lo que hay en el cristal) hace refresco
// parcial; sin él, completo. Deja el panel en deep sleep.
static void pintar(const idfxx::epaper::mono_framebuffer &nuevo, const idfxx::epaper::mono_framebuffer *anterior)
{
    idfxx::spi::bus_config bus_cfg{};
    bus_cfg.mosi = idfxx::gpio_9;
    bus_cfg.sclk = idfxx::gpio_7;
    idfxx::spi::master_bus bus(idfxx::spi::host_device::spi2, idfxx::spi::dma_chan::ch_auto, bus_cfg);

    idfxx::panel_io io(bus, idfxx::epaper::uc8179::spi_io_config(idfxx::gpio_10 /*CS*/, idfxx::gpio_11 /*DC*/));
    idfxx::epaper::uc8179 display(io, {.reset_gpio = idfxx::gpio_12, .busy_gpio = idfxx::gpio_13});

    auto modo = idfxx::epaper::refresh_mode::full;
    if (anterior != nullptr) {
        display.restore_baseline(*anterior);
        modo = idfxx::epaper::refresh_mode::partial;
    }
    display.write(nuevo);
    display.refresh(modo);
    display.sleep();
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static constexpr EventBits_t BIT_SUSCRITO = BIT0;
static constexpr EventBits_t BIT_NUEVO = BIT1;  // hay mensaje completo sin procesar
static EventGroupHandle_t eventos;
static SemaphoreHandle_t mtx;
struct contenido {
    bool imagen;       // false = texto, true = imagen zlib
    std::string datos;
};
static std::optional<contenido> recibido;  // último mensaje completo sin procesar
static contenido trozos;                    // mensaje troceado en curso
static bool trozos_validos;
static volatile int64_t ultimo_evento_us;

static void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado al broker (sesión previa: %d)", event->session_present);
        esp_mqtt_client_subscribe(event->client, CONFIG_MQTT_TOPIC, 1);
        esp_mqtt_client_subscribe(event->client, CONFIG_MQTT_TOPIC_IMAGEN, 1);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ultimo_evento_us = esp_timer_get_time();
        xEventGroupSetBits(eventos, BIT_SUSCRITO);
        break;
    case MQTT_EVENT_DATA:
        // Los mensajes grandes llegan troceados y el topic sólo viene en el
        // primer trozo: se reensamblan aquí.
        if (event->current_data_offset == 0) {
            std::string_view topic(event->topic, event->topic_len);
            trozos.imagen = topic == CONFIG_MQTT_TOPIC_IMAGEN;
            trozos.datos.clear();
            const size_t max = trozos.imagen ? MAX_IMAGEN_Z : MAX_TEXTO;
            // Payload vacío = alguien borra el retenido: se ignora
            trozos_validos = event->total_data_len > 0 && static_cast<size_t>(event->total_data_len) <= max;
            if (!trozos_validos && event->total_data_len > 0) {
                ESP_LOGW(TAG, "Mensaje de %d bytes en %.*s ignorado (máx %u)", event->total_data_len,
                         event->topic_len, event->topic, (unsigned)max);
            }
            if (trozos_validos) {
                trozos.datos.reserve(event->total_data_len);
            }
        }
        if (trozos_validos) {
            trozos.datos.append(event->data, event->data_len);
            if (static_cast<int>(trozos.datos.size()) == event->total_data_len) {
                if (trozos.imagen) {
                    ESP_LOGI(TAG, "Recibida imagen (%d bytes comprimida)", event->total_data_len);
                } else {
                    ESP_LOGI(TAG, "Recibido texto (%d bytes): %s", event->total_data_len, trozos.datos.c_str());
                }
                xSemaphoreTake(mtx, portMAX_DELAY);
                recibido = std::move(trozos);
                xSemaphoreGive(mtx);
                trozos_validos = false;
                xEventGroupSetBits(eventos, BIT_NUEVO);
            }
        }
        ultimo_evento_us = esp_timer_get_time();
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error MQTT");
        break;
    default:
        break;
    }
}

// Conecta WiFi + MQTT y espera a la suscripción. El client_id fijo y la
// sesión persistente hacen que el broker guarde los mensajes QoS1 publicados
// mientras dormimos; los retenidos llegan siempre al suscribirse.
static esp_mqtt_client_handle_t conectar(bool &conectado)
{
    conectado = false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    eventos = xEventGroupCreate();
    mtx = xSemaphoreCreateMutex();
    if (example_connect() != ESP_OK) {  // SSID/clave en menuconfig -> Example Connection Configuration
        ESP_LOGE(TAG, "Sin WiFi");
        return nullptr;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char client_id[32];
    snprintf(client_id, sizeof(client_id), "reterminal-%02x%02x%02x", mac[3], mac[4], mac[5]);

    esp_mqtt_client_config_t mqtt_cfg{};
    mqtt_cfg.broker.address.uri = CONFIG_BROKER_URI;
    mqtt_cfg.credentials.client_id = client_id;
    mqtt_cfg.session.disable_clean_session = true;
    mqtt_cfg.buffer.size = 4096;  // menos trozos al recibir imágenes
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(client);

    conectado = xEventGroupWaitBits(eventos, BIT_SUSCRITO, pdFALSE, pdTRUE,
                                    pdMS_TO_TICKS(CONFIG_TIMEOUT_CONEXION_S * 1000)) & BIT_SUSCRITO;
    if (!conectado) {
        ESP_LOGE(TAG, "No se pudo conectar/suscribir al broker");
    }
    return client;
}

[[maybe_unused]] static void desconectar(esp_mqtt_client_handle_t client)
{
    if (client != nullptr) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
    example_disconnect();
}

// Espera hasta que pase VENTANA_MS sin mensajes y devuelve el último sin procesar.
static std::optional<contenido> recoger_mensaje()
{
    while (esp_timer_get_time() - ultimo_evento_us < CONFIG_VENTANA_MS * 1000LL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xEventGroupClearBits(eventos, BIT_NUEVO);
    xSemaphoreTake(mtx, portMAX_DELAY);
    auto resultado = std::exchange(recibido, std::nullopt);
    xSemaphoreGive(mtx);
    return resultado;
}

// ---------------------------------------------------------------------------

// Acumula en rtc.s_desde_full el tiempo despierto desde la última llamada.
static void acumular_tiempo()
{
    static int64_t ultimo_us = 0;
    const int64_t ahora = esp_timer_get_time();
    rtc.s_desde_full += static_cast<uint32_t>((ahora - ultimo_us) / 1000000);
    ultimo_us = ahora - (ahora - ultimo_us) % 1000000;
}

[[maybe_unused, noreturn]] static void dormir()
{
    acumular_tiempo();
    rtc.s_desde_full += CONFIG_INTERVALO_S;
    ESP_LOGI(TAG, "Despierto %lld ms; a deep sleep %d s", esp_timer_get_time() / 1000, CONFIG_INTERVALO_S);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(CONFIG_INTERVALO_S) * 1000000ULL);
    esp_deep_sleep_start();
}

// Pinta `msg` si cambia lo que hay en el cristal. Sin mensaje y sin un frame
// fiable, pinta una pantalla de estado para dejar el cristal en estado conocido.
static void procesar(std::optional<contenido> msg, bool lfs, bool conectado)
{
    acumular_tiempo();
    try {
        // Frame que hay en el cristal, si nos fiamos de él
        std::optional<idfxx::epaper::mono_framebuffer> anterior;
        if (rtc.magic == RTC_MAGIC && lfs) {
            anterior.emplace(ANCHO, ALTO);
            if (!cargar_frame(*anterior)) {
                anterior.reset();
            }
        }

        idfxx::epaper::mono_framebuffer nuevo(ANCHO, ALTO);
        if (msg && msg->imagen) {
            if (!descomprimir_imagen(msg->datos, nuevo)) {
                msg.reset();
            }
        } else if (msg) {
            dibujar_texto(nuevo, msg->datos);
        }
        if (!msg) {
            if (anterior) {
                return;
            }
            dibujar_texto(nuevo, conectado ? std::string("Esperando mensajes en\n" CONFIG_MQTT_TOPIC "\n" CONFIG_MQTT_TOPIC_IMAGEN)
                                           : "Sin conexion.\nReintentando cada " + std::to_string(CONFIG_INTERVALO_S) + " s");
        }
        if (anterior && std::ranges::equal(anterior->data(), nuevo.data())) {
            ESP_LOGI(TAG, "Sin cambios en la imagen, no se refresca");
            return;
        }

        const bool parcial = anterior && rtc.parciales < CONFIG_PARCIALES_MAX &&
                             rtc.s_desde_full < CONFIG_FULL_CADA_H * 3600u;
        ESP_LOGI(TAG, "Refresco %s (parciales=%" PRIu32 ", %" PRIu32 " s desde el completo)",
                 parcial ? "parcial" : "completo", rtc.parciales, rtc.s_desde_full);

        rtc.magic = 0;  // mientras se pinta, el cristal está en estado incierto
        pintar(nuevo, parcial ? &*anterior : nullptr);
        if (parcial) {
            rtc.parciales++;
        } else {
            rtc.parciales = 0;
            rtc.s_desde_full = 0;
        }
        if (lfs && guardar_frame(nuevo)) {
            rtc.magic = RTC_MAGIC;
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error: %s", e.what());
        rtc.magic = 0;  // el próximo pintado será completo
    }
}

extern "C" void app_main()
{
    sd_release_bus();

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        rtc = {};  // arranque en frío, flasheo, crash...: no sabemos qué hay en el cristal
    }
    const bool lfs = montar_littlefs();

    bool conectado = false;
    [[maybe_unused]] esp_mqtt_client_handle_t client = conectar(conectado);
    procesar(conectado ? recoger_mensaje() : std::nullopt, lfs, conectado);

#if CONFIG_SIN_DEEP_SLEEP
    // Modo pruebas: seguir conectado y pintar cada mensaje según llega
    ESP_LOGW(TAG, "Modo pruebas: sin deep sleep");
    while (true) {
        xEventGroupWaitBits(eventos, BIT_NUEVO, pdFALSE, pdTRUE, portMAX_DELAY);
        procesar(recoger_mensaje(), lfs, true);
    }
#else
    desconectar(client);
    dormir();
#endif
}
