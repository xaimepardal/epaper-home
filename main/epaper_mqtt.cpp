// Galería de imágenes en el ePaper del reTerminal E1001, alimentada por MQTT y
// pensada para batería. Cada imagen llega con un índice y se guarda en LittleFS
// en su hueco; los botones pasan a la anterior/siguiente sin conectarse a nada
// y el de refresco descarga las nuevas. Cada CONFIG_INTERVALO_S el ESP32
// despierta, recoge la cola MQTT y vuelve a deep sleep; la pantalla sigue en la
// imagen seleccionada y sólo se repinta si ésta ha cambiado.
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
#include <ctime>
#include <exception>
#include <memory>
#include <string_view>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_crt_bundle.h"
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

// Pantallas de estado: spleen 8x16 a escala 3 -> 24x48 px por carácter
static constexpr unsigned ESCALA = 3;
static constexpr size_t MARGEN = 16;
// Payload: 1 byte de índice + zlib de 800x480 a 1 bpp (48000 bytes). Comprimida
// no debería pasar de esto ni en el peor caso (deflate añade poco a datos incompresibles).
static constexpr size_t MAX_IMAGEN_Z = 64 * 1024;
static constexpr unsigned MAX_IMAGENES = CONFIG_MAX_IMAGENES;

// Botones (activos a nivel bajo, con pull-up en la placa)
static constexpr gpio_num_t GPIO_ANTERIOR = static_cast<gpio_num_t>(CONFIG_BOTON_ANTERIOR_GPIO);
static constexpr gpio_num_t GPIO_SIGUIENTE = static_cast<gpio_num_t>(CONFIG_BOTON_SIGUIENTE_GPIO);
static constexpr gpio_num_t GPIO_REFRESCO = static_cast<gpio_num_t>(CONFIG_BOTON_REFRESCO_GPIO);
static constexpr uint64_t MASCARA_BOTONES = (1ULL << GPIO_ANTERIOR) | (1ULL << GPIO_SIGUIENTE) | (1ULL << GPIO_REFRESCO);
enum class boton { ninguno, anterior, siguiente, refresco };

// ---------------------------------------------------------------------------
// Estado que sobrevive al deep sleep (y a nada más: cualquier otro reset lo
// invalida, y entonces no se sabe qué hay en el cristal). Los tiempos son de
// time(), que el RTC sigue contando durante el deep sleep.
// ---------------------------------------------------------------------------
static constexpr uint32_t RTC_MAGIC = 0x45504D51;
struct estado_rtc {
    uint32_t magic;       // RTC_MAGIC = el frame guardado coincide con el cristal
    uint32_t parciales;   // parciales desde el último completo
    time_t t_full;        // último refresco completo
    time_t t_proxima;     // próxima comprobación de MQTT
};
RTC_DATA_ATTR static estado_rtc rtc;

// ---------------------------------------------------------------------------
// LittleFS: último frame pintado, imágenes por índice e índice seleccionado
// ---------------------------------------------------------------------------
static constexpr const char *RUTA_FRAME = "/lfs/frame.bin";
static constexpr const char *RUTA_SEL = "/lfs/sel.bin";
static constexpr uint32_t FRAME_MAGIC = 0x46524D31;  // "FRM1"
struct cabecera_frame {
    uint32_t magic;
    uint16_t ancho;
    uint16_t alto;
    uint32_t crc;
};

// Protege las imágenes guardadas y la memoria: el handler MQTT guarda mientras
// app_main puede estar pintando (sólo en modo pruebas o con botones).
static SemaphoreHandle_t mtx;

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

static std::string ruta_imagen(unsigned indice)
{
    char ruta[24];
    snprintf(ruta, sizeof(ruta), "/lfs/img%03u.bin", indice);
    return ruta;
}

static bool existe_imagen(unsigned indice)
{
    return access(ruta_imagen(indice).c_str(), F_OK) == 0;
}

static bool cargar_frame(const char *ruta, idfxx::epaper::mono_framebuffer &fb)
{
    FILE *f = fopen(ruta, "rb");
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
        ESP_LOGW(TAG, "Frame inválido en %s", ruta);
    }
    return ok;
}

// Escritura atómica: fichero temporal + rename, así un corte a mitad nunca deja
// un frame a medias con apariencia de válido.
static bool guardar_frame(const char *ruta, const idfxx::epaper::mono_framebuffer &fb)
{
    auto datos = fb.data();
    cabecera_frame cab{FRAME_MAGIC, static_cast<uint16_t>(fb.width()), static_cast<uint16_t>(fb.height()),
                       esp_rom_crc32_le(0, datos.data(), datos.size())};
    const std::string tmp = std::string(ruta) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fwrite(&cab, sizeof(cab), 1, f) == 1 && fwrite(datos.data(), 1, datos.size(), f) == datos.size() &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        ok = rename(tmp.c_str(), ruta) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "No se pudo guardar %s", ruta);
        remove(tmp.c_str());
    }
    return ok;
}

static int leer_seleccion()
{
    FILE *f = fopen(RUTA_SEL, "rb");
    if (f == nullptr) {
        return -1;
    }
    uint8_t sel = 0;
    const bool ok = fread(&sel, 1, 1, f) == 1;
    fclose(f);
    return ok ? sel : -1;
}

static void guardar_seleccion(unsigned indice)
{
    FILE *f = fopen(RUTA_SEL, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "No se pudo guardar la selección");
        return;
    }
    const uint8_t sel = indice;
    fwrite(&sel, 1, 1, f);
    fclose(f);
}

// Índice que hay que mostrar: el seleccionado si sigue existiendo; si no (se
// borró o aún no había ninguno), el siguiente que exista.
static std::optional<unsigned> seleccion()
{
    const int sel = leer_seleccion();
    const unsigned inicio = (sel >= 0 && static_cast<unsigned>(sel) < MAX_IMAGENES) ? sel : 0;
    for (unsigned i = 0; i < MAX_IMAGENES; ++i) {
        const unsigned indice = (inicio + i) % MAX_IMAGENES;
        if (existe_imagen(indice)) {
            if (static_cast<int>(indice) != sel) {
                guardar_seleccion(indice);
            }
            return indice;
        }
    }
    return std::nullopt;
}

// Selecciona la imagen anterior (paso -1) o siguiente (+1) que exista, dando la vuelta.
static void navegar(int paso)
{
    const auto actual = seleccion();
    if (!actual) {
        ESP_LOGI(TAG, "No hay imágenes guardadas");
        return;
    }
    for (unsigned i = 1; i < MAX_IMAGENES; ++i) {
        int indice = (static_cast<int>(*actual) + paso * static_cast<int>(i)) % static_cast<int>(MAX_IMAGENES);
        if (indice < 0) {
            indice += MAX_IMAGENES;
        }
        if (existe_imagen(indice)) {
            ESP_LOGI(TAG, "Imagen %u -> %d", *actual, indice);
            guardar_seleccion(indice);
            return;
        }
    }
    ESP_LOGI(TAG, "Sólo hay una imagen (%u)", *actual);
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------

// Pantalla de estado: texto con '\n' como saltos de línea (sin ajuste por palabras).
static void dibujar_texto(idfxx::epaper::mono_framebuffer &fb, const std::string &texto)
{
    const auto &fuente = idfxx::font::spleen_8x16;
    const size_t alto_char = fuente.height * ESCALA;
    idfxx::gfx::canvas canvas(fb);
    canvas.clear();
    size_t y = MARGEN;
    for (size_t inicio = 0; inicio <= texto.size() && y + alto_char <= fb.height(); y += alto_char) {
        size_t fin = std::min(texto.find('\n', inicio), texto.size());
        canvas.draw_text(fuente, MARGEN, y, texto.substr(inicio, fin - inicio), true, ESCALA);
        inicio = fin + 1;
    }
}

// Descomprime una imagen zlib directamente en el framebuffer. Falla si el
// stream está corrupto (zlib lleva Adler-32) o no mide exactamente un frame.
static bool descomprimir_imagen(std::string_view z, idfxx::epaper::mono_framebuffer &fb)
{
    auto salida = fb.data();
    // El estado del descompresor (~11 KB) va al heap: no cabe en la pila
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
static constexpr EventBits_t BIT_NUEVO = BIT1;  // se ha guardado o borrado alguna imagen
static EventGroupHandle_t eventos;
static std::string trozos;  // mensaje troceado en curso
static bool trozos_validos;
static volatile int64_t ultimo_evento_us;

#ifdef MQTT_CA_PROPIA
// main/certs/mqtt_ca.crt incrustado por CMake (EMBED_TXTFILES lo termina en '\0')
extern const char mqtt_ca_pem[] asm("_binary_mqtt_ca_crt_start");
#endif

// Guarda (o borra, si sólo trae el índice) la imagen de un mensaje completo.
static void guardar_recibida(std::string_view payload)
{
    const unsigned indice = static_cast<uint8_t>(payload[0]);
    if (indice >= MAX_IMAGENES) {
        ESP_LOGW(TAG, "Índice %u fuera de rango (máx %u), ignorado", indice, MAX_IMAGENES - 1);
        return;
    }
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        if (payload.size() == 1) {
            remove(ruta_imagen(indice).c_str());
            ESP_LOGI(TAG, "Borrada la imagen %u", indice);
            xEventGroupSetBits(eventos, BIT_NUEVO);
        } else {
            idfxx::epaper::mono_framebuffer fb(ANCHO, ALTO);
            if (descomprimir_imagen(payload.substr(1), fb) && guardar_frame(ruta_imagen(indice).c_str(), fb)) {
                ESP_LOGI(TAG, "Guardada la imagen %u (%u bytes comprimida)", indice, (unsigned)payload.size() - 1);
                xEventGroupSetBits(eventos, BIT_NUEVO);
            }
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "No se pudo guardar la imagen %u: %s", indice, e.what());
    }
    xSemaphoreGive(mtx);
}

static void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado al broker (sesión previa: %d)", event->session_present);
        esp_mqtt_client_subscribe(event->client, CONFIG_MQTT_TOPIC_IMAGEN, 1);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ultimo_evento_us = esp_timer_get_time();
        xEventGroupSetBits(eventos, BIT_SUSCRITO);
        break;
    case MQTT_EVENT_DATA:
        // Los mensajes grandes llegan troceados: se reensamblan aquí.
        if (event->current_data_offset == 0) {
            trozos.clear();
            // Payload vacío = alguien borra un retenido: se ignora
            trozos_validos = event->total_data_len > 0 && static_cast<size_t>(event->total_data_len) <= MAX_IMAGEN_Z + 1;
            if (!trozos_validos && event->total_data_len > 0) {
                ESP_LOGW(TAG, "Mensaje de %d bytes ignorado (máx %u)", event->total_data_len,
                         (unsigned)MAX_IMAGEN_Z + 1);
            }
            if (trozos_validos) {
                trozos.reserve(event->total_data_len);
            }
        }
        if (trozos_validos) {
            trozos.append(event->data, event->data_len);
            if (static_cast<int>(trozos.size()) == event->total_data_len) {
                trozos_validos = false;
                guardar_recibida(trozos);
                std::string().swap(trozos);  // libera los hasta 64 KB
            }
        }
        ultimo_evento_us = esp_timer_get_time();
        break;
    case MQTT_EVENT_ERROR: {
        const auto *e = event->error_handle;
        if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            // TLS/red: certificado que no valida, host que no está en el SAN, puerto cerrado...
            ESP_LOGE(TAG, "Error de transporte: esp-tls 0x%x, mbedtls -0x%x, errno %d",
                     e->esp_tls_last_esp_err, -e->esp_tls_stack_err, e->esp_transport_sock_errno);
        } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            // 0x4 = usuario/contraseña incorrectos, 0x5 = no autorizado
            ESP_LOGE(TAG, "El broker rechaza la conexión: código 0x%x", e->connect_return_code);
        } else {
            ESP_LOGE(TAG, "Error MQTT (tipo %d)", e->error_type);
        }
        break;
    }
    default:
        break;
    }
}

// Conecta WiFi + MQTT y espera a la suscripción. El client_id fijo y la
// sesión persistente hacen que el broker guarde en cola los mensajes QoS 1
// publicados mientras dormimos.
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
    // TLS (sólo cuenta con mqtts://): la CA propia de main/certs/ si se compiló con ella,
    // si no, el bundle de CAs públicas. El host del URI tiene que estar en el certificado
    // (CN o SAN; vale una IP si el SAN la incluye).
#ifdef MQTT_CA_PROPIA
    mqtt_cfg.broker.verification.certificate = mqtt_ca_pem;
#else
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    mqtt_cfg.credentials.client_id = client_id;
    if (CONFIG_MQTT_USUARIO[0] != '\0') {
        mqtt_cfg.credentials.username = CONFIG_MQTT_USUARIO;
        mqtt_cfg.credentials.authentication.password = CONFIG_MQTT_CLAVE;
    }
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

// Espera hasta que pase VENTANA_MS sin mensajes (la cola ya se ha vaciado).
static void esperar_cola()
{
    while (esp_timer_get_time() - ultimo_evento_us < CONFIG_VENTANA_MS * 1000LL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xEventGroupClearBits(eventos, BIT_NUEVO);
}

// Conexión de este despertar: se abre la primera vez que hace falta (un solo
// intento) y se mantiene hasta dormir.
static esp_mqtt_client_handle_t cliente;
static bool intentado, conectado;

static void sincronizar()
{
    rtc.t_proxima = time(nullptr) + CONFIG_INTERVALO_S;
    if (!intentado) {
        intentado = true;
        cliente = conectar(conectado);
    }
    if (conectado) {
        esperar_cola();
    }
}

// ---------------------------------------------------------------------------
// Botones
// ---------------------------------------------------------------------------
static void configurar_botones()
{
    gpio_config_t cfg{};
    cfg.pin_bit_mask = MASCARA_BOTONES;
    cfg.mode = GPIO_MODE_INPUT;  // pull-up externo
    gpio_config(&cfg);
}

static boton leer_botones()
{
    if (gpio_get_level(GPIO_ANTERIOR) == 0) {
        return boton::anterior;
    }
    if (gpio_get_level(GPIO_SIGUIENTE) == 0) {
        return boton::siguiente;
    }
    if (gpio_get_level(GPIO_REFRESCO) == 0) {
        return boton::refresco;
    }
    return boton::ninguno;
}

// Botón pulsado (estable 30 ms) antes de `timeout_ms`; en modo pruebas
// también vuelve, con ninguno, si llega una imagen.
static boton esperar_boton(int64_t timeout_ms)
{
    const int64_t fin = esp_timer_get_time() + timeout_ms * 1000;
    while (esp_timer_get_time() < fin) {
        boton b = leer_botones();
        if (b != boton::ninguno) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (leer_botones() == b) {
                return b;
            }
        }
#if CONFIG_SIN_DEEP_SLEEP
        if (xEventGroupGetBits(eventos) & BIT_NUEVO) {
            return boton::ninguno;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return boton::ninguno;
}

// Espera a que se suelten todos los botones; false si alguno sigue pulsado.
static bool esperar_suelta()
{
    for (int i = 0; i < 250; ++i) {  // 5 s
        if (leer_botones() == boton::ninguno) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "Botón atascado: no despertará con los botones");
    return false;
}

// Botón que ha despertado al ESP32, si ha sido uno.
static boton boton_del_despertar()
{
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) {
        return boton::ninguno;
    }
    const uint64_t pines = esp_sleep_get_ext1_wakeup_status();
    if (pines & (1ULL << GPIO_ANTERIOR)) {
        return boton::anterior;
    }
    if (pines & (1ULL << GPIO_SIGUIENTE)) {
        return boton::siguiente;
    }
    return boton::refresco;
}

// ---------------------------------------------------------------------------

[[maybe_unused, noreturn]] static void dormir(bool con_botones)
{
    const time_t ahora = time(nullptr);
    const time_t espera = std::max<time_t>(rtc.t_proxima - ahora, 1);
    ESP_LOGI(TAG, "Despierto %lld ms; a deep sleep %lld s", esp_timer_get_time() / 1000, (long long)espera);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(espera) * 1000000ULL);
    if (con_botones) {
        esp_sleep_enable_ext1_wakeup_io(MASCARA_BOTONES, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    esp_deep_sleep_start();
}

// Pinta la imagen seleccionada si no es lo que ya hay en el cristal. Si no hay
// ninguna guardada pinta `aviso` (si lo hay). `forzar` = refresco completo
// aunque no haya cambios (limpia los restos de los parciales).
static void mostrar(bool lfs, const char *aviso, bool forzar)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        // Frame que hay en el cristal, si nos fiamos de él
        std::optional<idfxx::epaper::mono_framebuffer> anterior;
        if (rtc.magic == RTC_MAGIC && lfs) {
            anterior.emplace(ANCHO, ALTO);
            if (!cargar_frame(RUTA_FRAME, *anterior)) {
                anterior.reset();
            }
        }

        idfxx::epaper::mono_framebuffer nuevo(ANCHO, ALTO);
        const auto sel = lfs ? seleccion() : std::nullopt;
        bool hay_imagen = sel && cargar_frame(ruta_imagen(*sel).c_str(), nuevo);
        if (!hay_imagen) {
            if (aviso == nullptr) {
                xSemaphoreGive(mtx);
                return;
            }
            dibujar_texto(nuevo, aviso);
        }
        if (!forzar && anterior && std::ranges::equal(anterior->data(), nuevo.data())) {
            ESP_LOGI(TAG, "Sin cambios en la imagen, no se refresca");
            xSemaphoreGive(mtx);
            return;
        }

        const time_t ahora = time(nullptr);
        const bool parcial = !forzar && anterior && rtc.parciales < CONFIG_PARCIALES_MAX &&
                             ahora - rtc.t_full < CONFIG_FULL_CADA_H * 3600;
        if (hay_imagen) {
            ESP_LOGI(TAG, "Imagen %u, refresco %s (parciales=%" PRIu32 ")", *sel, parcial ? "parcial" : "completo",
                     rtc.parciales);
        }

        rtc.magic = 0;  // mientras se pinta, el cristal está en estado incierto
        pintar(nuevo, parcial ? &*anterior : nullptr);
        if (parcial) {
            rtc.parciales++;
        } else {
            rtc.parciales = 0;
            rtc.t_full = ahora;
        }
        if (lfs && guardar_frame(RUTA_FRAME, nuevo)) {
            rtc.magic = RTC_MAGIC;
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error: %s", e.what());
        rtc.magic = 0;  // el próximo pintado será completo
    }
    xSemaphoreGive(mtx);
}

static const char *aviso()
{
    static const std::string sin_conexion = "Sin conexion.\nReintentando cada " + std::to_string(CONFIG_INTERVALO_S) + " s";
    return conectado ? "Esperando imagenes en\n" CONFIG_MQTT_TOPIC_IMAGEN : sin_conexion.c_str();
}

// Anterior/siguiente: sólo cambia de imagen, sin red. Refresco (y despertar
// por tiempo o arranque, `ninguno`): recoge la cola MQTT y pinta la seleccionada.
static void atender(boton b, bool lfs)
{
    switch (b) {
    case boton::anterior:
    case boton::siguiente:
        navegar(b == boton::anterior ? -1 : 1);
        mostrar(lfs, nullptr, false);
        break;
    case boton::refresco:
        ESP_LOGI(TAG, "Botón de refresco");
        sincronizar();
        mostrar(lfs, aviso(), true);
        break;
    case boton::ninguno:
        sincronizar();
        mostrar(lfs, aviso(), false);
        break;
    }
}

extern "C" void app_main()
{
    sd_release_bus();
    configurar_botones();
    eventos = xEventGroupCreate();
    mtx = xSemaphoreCreateMutex();

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        rtc = {};  // arranque en frío, flasheo, crash...: no sabemos qué hay en el cristal
    }
    const bool lfs = montar_littlefs();

    // Si despierta un botón de navegación no se toca la red; la comprobación
    // de MQTT sigue programada para rtc.t_proxima (ver dormir()).
    boton b = boton_del_despertar();
    atender(b, lfs);

#if CONFIG_SIN_DEEP_SLEEP
    // Modo pruebas: seguir conectado, pintar cada imagen según llega y atender los botones
    ESP_LOGW(TAG, "Modo pruebas: sin deep sleep");
    while (true) {
        esperar_suelta();
        b = esperar_boton(INT32_MAX);
        if (b == boton::ninguno) {
            esperar_cola();
            mostrar(lfs, aviso(), false);
        } else {
            atender(b, lfs);
        }
    }
#else
    // Unos segundos para seguir pasando imágenes sin volver a despertar
    bool sueltos;
    while ((sueltos = esperar_suelta()) && (b = esperar_boton(CONFIG_VENTANA_BOTONES_MS)) != boton::ninguno) {
        atender(b, lfs);
    }
    desconectar(cliente);
    dormir(sueltos);
#endif
}
