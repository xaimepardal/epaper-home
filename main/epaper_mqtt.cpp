// Galería de imágenes en el ePaper del reTerminal E1001, alimentada por MQTT y
// pensada para batería. Cada imagen es un mensaje retenido en su propio topic
// (<MQTT_TOPIC_IMAGEN>/<índice>) y se guarda en LittleFS en su hueco; los
// botones pasan a la anterior/siguiente sin conectarse a nada y el de refresco
// descarga las nuevas. Cada CONFIG_INTERVAL_S el ESP32 despierta, recoge los
// cambios y vuelve a deep sleep; la pantalla sigue en la imagen seleccionada y
// sólo se repinta si ésta ha cambiado.
//
// Sincronización: los retenidos del broker son el estado completo y la sesión
// persistente trae sólo los cambios. Si el broker conserva la sesión (y su
// suscripción es la de este firmware) no se vuelve a suscribir: llega lo
// publicado mientras dormía y nada más. Si no (primer arranque, flash borrada,
// sesión caducada), se suscribe, recibe todos los retenidos y borra los huecos
// que ya no estén en el broker.
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
#include <bitset>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <string_view>
#include <optional>
#include <string>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "miniz.h"  // tinfl (inflate) de la ROM
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "wifi.hpp"
#include "battery.hpp"

static const char *TAG = "epaper_mqtt";

// Panel 7.5" 800x480 (valores por defecto de uc8179::config)
static constexpr size_t WIDTH = 800;
static constexpr size_t HEIGHT = 480;

// Pantallas de estado: spleen 8x16 a escala 3 -> 24x48 px por carácter
static constexpr unsigned TEXT_SCALE = 3;
static constexpr size_t MARGIN = 16;
// Payload: zlib de 800x480 a 1 bpp (48000 bytes). Comprimida no debería pasar
// de esto ni en el peor caso (deflate añade poco a datos incompresibles).
static constexpr size_t MAX_COMPRESSED_SIZE = 64 * 1024;
static constexpr unsigned MAX_IMAGES = CONFIG_MAX_IMAGES;

// Botones (activos a nivel bajo, con pull-up en la placa)
static constexpr gpio_num_t GPIO_PREV = static_cast<gpio_num_t>(CONFIG_BUTTON_PREV_GPIO);
static constexpr gpio_num_t GPIO_NEXT = static_cast<gpio_num_t>(CONFIG_BUTTON_NEXT_GPIO);
static constexpr gpio_num_t GPIO_REFRESH = static_cast<gpio_num_t>(CONFIG_BUTTON_REFRESH_GPIO);
static constexpr uint64_t BUTTON_MASK = (1ULL << GPIO_PREV) | (1ULL << GPIO_NEXT) | (1ULL << GPIO_REFRESH);
enum class button { none, previous, next, refresh };

// ---------------------------------------------------------------------------
// Estado que sobrevive al deep sleep (y a nada más: cualquier otro reset lo
// invalida, y entonces no se sabe qué hay en el cristal). Los tiempos son de
// time(), que el RTC sigue contando durante el deep sleep.
// ---------------------------------------------------------------------------
static constexpr uint32_t RTC_MAGIC = 0x45504D51;
// Resultado de la última conexión (lo que muestra el icono de WiFi)
enum class net_state : uint8_t { unknown, ok, no_wifi, no_broker };
struct rtc_state {
    uint32_t magic;       // RTC_MAGIC = el frame guardado coincide con el cristal
    uint32_t partials;   // parciales desde el último completo
    time_t t_full;        // último refresco completo
    time_t t_next_check;     // próxima comprobación de MQTT
    net_state network;
    int8_t battery;       // % que se muestra (múltiplo de 5), -1 = sin medida válida
};
RTC_DATA_ATTR static rtc_state rtc;

// ---------------------------------------------------------------------------
// LittleFS: último frame pintado, imágenes por índice e índice seleccionado
// ---------------------------------------------------------------------------
static constexpr const char *FRAME_PATH = "/lfs/frame.bin";
static constexpr const char *SELECTION_PATH = "/lfs/sel.bin";
// Filtro al que está suscrita la sesión del broker y número de huecos, escrito
// tras una sincronización completa que terminó bien. Si falta o no coincide con
// el de este firmware, el siguiente despertar vuelve a suscribirse.
static constexpr const char *SUBSCRIPTION_PATH = "/lfs/sub.txt";
static constexpr uint32_t FRAME_MAGIC = 0x46524D31;  // "FRM1"
struct frame_header {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint32_t crc;
};

// Protege las imágenes guardadas y la memoria: el handler MQTT guarda mientras
// app_main puede estar pintando (sólo en modo pruebas o con botones).
static SemaphoreHandle_t mtx;

static bool mount_littlefs()
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

static std::string image_path(unsigned index)
{
    char path[24];
    snprintf(path, sizeof(path), "/lfs/img%03u.bin", index);
    return path;
}

static bool image_exists(unsigned index)
{
    return access(image_path(index).c_str(), F_OK) == 0;
}

static bool load_frame(const char *path, idfxx::epaper::mono_framebuffer &fb)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    frame_header header{};
    auto data = fb.data();
    bool ok = fread(&header, sizeof(header), 1, f) == 1 && header.magic == FRAME_MAGIC && header.width == fb.width() &&
              header.height == fb.height() && fread(data.data(), 1, data.size(), f) == data.size() &&
              esp_rom_crc32_le(0, data.data(), data.size()) == header.crc;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "Frame inválido en %s", path);
    }
    return ok;
}

// CRC de los datos de un frame guardado, leyendo sólo la cabecera.
static std::optional<uint32_t> stored_crc(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return std::nullopt;
    }
    frame_header header{};
    const bool ok = fread(&header, sizeof(header), 1, f) == 1 && header.magic == FRAME_MAGIC && header.width == WIDTH &&
                    header.height == HEIGHT;
    fclose(f);
    return ok ? std::optional(header.crc) : std::nullopt;
}

// Escritura atómica: fichero temporal + rename, así un corte a mitad nunca deja
// un frame a medias con apariencia de válido.
static bool save_frame(const char *path, const idfxx::epaper::mono_framebuffer &fb)
{
    auto data = fb.data();
    frame_header header{FRAME_MAGIC, static_cast<uint16_t>(fb.width()), static_cast<uint16_t>(fb.height()),
                       esp_rom_crc32_le(0, data.data(), data.size())};
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1 && fwrite(data.data(), 1, data.size(), f) == data.size() &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        ok = rename(tmp.c_str(), path) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "No se pudo guardar %s", path);
        remove(tmp.c_str());
    }
    return ok;
}

static int read_selection()
{
    FILE *f = fopen(SELECTION_PATH, "rb");
    if (f == nullptr) {
        return -1;
    }
    uint8_t sel = 0;
    const bool ok = fread(&sel, 1, 1, f) == 1;
    fclose(f);
    return ok ? sel : -1;
}

static void save_selection(unsigned index)
{
    FILE *f = fopen(SELECTION_PATH, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "No se pudo guardar la selección");
        return;
    }
    const uint8_t sel = index;
    fwrite(&sel, 1, 1, f);
    fclose(f);
}

// Índice que hay que mostrar: el seleccionado si sigue existiendo; si no (se
// borró o aún no había ninguno), el siguiente que exista.
static std::optional<unsigned> selection()
{
    const int sel = read_selection();
    const unsigned start = (sel >= 0 && static_cast<unsigned>(sel) < MAX_IMAGES) ? sel : 0;
    for (unsigned i = 0; i < MAX_IMAGES; ++i) {
        const unsigned index = (start + i) % MAX_IMAGES;
        if (image_exists(index)) {
            if (static_cast<int>(index) != sel) {
                save_selection(index);
            }
            return index;
        }
    }
    return std::nullopt;
}

// Selecciona la imagen anterior (paso -1) o siguiente (+1) que exista, dando la vuelta.
static void navigate(int step)
{
    const auto current = selection();
    if (!current) {
        ESP_LOGI(TAG, "No hay imágenes guardadas");
        return;
    }
    for (unsigned i = 1; i < MAX_IMAGES; ++i) {
        int index = (static_cast<int>(*current) + step * static_cast<int>(i)) % static_cast<int>(MAX_IMAGES);
        if (index < 0) {
            index += MAX_IMAGES;
        }
        if (image_exists(index)) {
            ESP_LOGI(TAG, "Imagen %u -> %d", *current, index);
            save_selection(index);
            return;
        }
    }
    ESP_LOGI(TAG, "Sólo hay una imagen (%u)", *current);
}

static std::string read_subscription()
{
    FILE *f = fopen(SUBSCRIPTION_PATH, "rb");
    if (f == nullptr) {
        return {};
    }
    char buf[128];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return std::string(buf, n);
}

static void save_subscription(const std::string &filter)
{
    FILE *f = fopen(SUBSCRIPTION_PATH, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "No se pudo guardar la suscripción");
        return;
    }
    fwrite(filter.data(), 1, filter.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Dibujo
// ---------------------------------------------------------------------------

// Pantalla de estado: texto con '\n' como saltos de línea (sin ajuste por palabras).
static void draw_message(idfxx::epaper::mono_framebuffer &fb, const std::string &text)
{
    const auto &font = idfxx::font::spleen_8x16;
    const size_t char_height = font.height * TEXT_SCALE;
    idfxx::gfx::canvas canvas(fb);
    canvas.clear();
    size_t y = MARGIN;
    for (size_t start = 0; start <= text.size() && y + char_height <= fb.height(); y += char_height) {
        size_t end = std::min(text.find('\n', start), text.size());
        canvas.draw_text(font, MARGIN, y, text.substr(start, end - start), true, TEXT_SCALE);
        start = end + 1;
    }
}

// ---------------------------------------------------------------------------
// Indicadores superpuestos: "k/N" arriba a la izquierda; WiFi y batería arriba
// a la derecha. Se dibujan en lo que se pinta, no en las imágenes guardadas, y
// como forman parte del frame, cualquier cambio (otra imagen guardada, la red
// cae o vuelve, la batería baja un escalón) hace que se repinte. Negro sobre un
// recuadro blanco con borde, para que se lean sobre cualquier imagen.
// ---------------------------------------------------------------------------
static constexpr size_t INDICATOR_SCALE = CONFIG_INDICATOR_SCALE;

// Recuadro blanco con borde negro de `grosor` px
static void draw_box(idfxx::gfx::canvas<idfxx::epaper::mono_framebuffer> &canvas, size_t x, size_t y, size_t width,
                     size_t height, size_t thickness)
{
    canvas.fill_rect(x, y, width, height, false);
    for (size_t g = 0; g < thickness; ++g) {
        canvas.draw_rect(x + g, y + g, width - 2 * g, height - 2 * g, true);
    }
}

// Posición de `indice` entre las N imágenes guardadas (no el número de hueco)
static void draw_position(idfxx::gfx::canvas<idfxx::epaper::mono_framebuffer> &canvas, unsigned index)
{
    unsigned position = 0, total = 0;
    for (unsigned i = 0; i < MAX_IMAGES; ++i) {
        if (image_exists(i)) {
            ++total;
            if (i <= index) {
                ++position;
            }
        }
    }
    const std::string text = std::to_string(position) + "/" + std::to_string(total);
    const auto &font = idfxx::font::spleen_8x16;
    constexpr size_t padding = 2 * INDICATOR_SCALE;
    draw_box(canvas, 0, 0, text.size() * font.width * INDICATOR_SCALE + 2 * padding,
             font.height * INDICATOR_SCALE + 2 * padding, 1);
    canvas.draw_text(font, padding, padding, text, true, INDICATOR_SCALE);
}

// Icono de WiFi (punto + 3 arcos) en un recuadro de ancho x alto que empieza
// en (x0, y0). Tachado sin WiFi; con "!" si hay WiFi pero no broker.
static void draw_wifi_icon(idfxx::epaper::mono_framebuffer &fb, size_t x0, size_t y0, size_t width, size_t height,
                         net_state network)
{
    idfxx::gfx::canvas canvas(fb);
    const float dot_radius = height * 0.12f;
    const float cx = x0 + width / 2.0f;
    const float cy = y0 + height - 1 - dot_radius;  // centro del punto y de los arcos
    const float radius = cy - y0;               // radio del arco exterior
    static constexpr float arcs[][2] = {{0.38f, 0.52f}, {0.62f, 0.76f}, {0.86f, 1.0f}};
    for (size_t y = y0; y < y0 + height; ++y) {
        for (size_t x = x0; x < x0 + width; ++x) {
            const float dx = x - cx, dy = cy - y;
            const float d = std::sqrt(dx * dx + dy * dy);
            bool ink = d <= dot_radius;
            if (dy > 0 && std::fabs(dx) <= dy) {  // cono de 90° hacia arriba
                for (const auto &a : arcs) {
                    ink = ink || (d >= a[0] * radius && d <= a[1] * radius);
                }
            }
            if (ink) {
                fb.set_pixel(x, y, true);
            }
        }
    }
    if (network == net_state::no_wifi) {
        // Raya blanca ancha y negra encima: corta los arcos y se ve tachado
        for (int pass = 0; pass < 2; ++pass) {
            const int thickness = pass == 0 ? 2 * INDICATOR_SCALE : INDICATOR_SCALE / 2;
            for (int k = -thickness; k <= thickness; ++k) {
                const int xa = std::clamp(static_cast<int>(x0) + k, static_cast<int>(x0), static_cast<int>(x0 + width - 1));
                const int xb = std::clamp(static_cast<int>(x0 + width - 1) + k, static_cast<int>(x0), static_cast<int>(x0 + width - 1));
                canvas.draw_line(xa, y0, xb, y0 + height - 1, pass == 1);
            }
        }
    } else if (network == net_state::no_broker) {
        const auto &font = idfxx::font::spleen_5x8;
        const size_t w = font.width * INDICATOR_SCALE, h = font.height * INDICATOR_SCALE;
        canvas.fill_rect(x0 + width - w - 2, y0, w + 2, h + 1, false);
        canvas.draw_text(font, x0 + width - w - 1, y0, "!", true, INDICATOR_SCALE);
    }
}

// WiFi y batería en un recuadro arriba a la derecha
static void draw_status(idfxx::epaper::mono_framebuffer &fb)
{
    const bool show_network = CONFIG_SHOW_NETWORK && rtc.network != net_state::unknown;
    const bool show_battery = CONFIG_SHOW_BATTERY;
    if (!show_network && !show_battery) {
        return;
    }
    const auto &font = idfxx::font::spleen_8x16;
    constexpr size_t e = INDICATOR_SCALE;
    constexpr size_t padding = 2 * e;
    const size_t height = font.height * e + 2 * padding;  // igual que el de "k/N"
    const size_t icon_height = font.height * e;
    const size_t icon_width = 2 * static_cast<size_t>(std::ceil(0.7072f * (icon_height * 0.88f))) + 1;
    const size_t body_width = 4 * font.width * e + 4 * e;  // cabe "100%"
    const size_t body_height = height - 2 * e;
    constexpr size_t terminal_width = 2 * e, terminal_height = 6 * e;

    size_t width = padding;
    if (show_network) {
        width += icon_width + padding;
    }
    if (show_battery) {
        width += body_width + terminal_width + 2 * padding;  // el polo no pegado al borde
    }
    const size_t x0 = fb.width() - width;
    idfxx::gfx::canvas canvas(fb);
    draw_box(canvas, x0, 0, width, height, 1);

    size_t x = x0 + padding;
    if (show_network) {
        draw_wifi_icon(fb, x, padding, icon_width, icon_height, rtc.network);
        x += icon_width + padding;
    }
    if (show_battery) {
        const size_t y = (height - body_height) / 2;
        for (size_t g = 0; g < std::max<size_t>(1, e / 2); ++g) {
            canvas.draw_rect(x + g, y + g, body_width - 2 * g, body_height - 2 * g, true);
        }
        canvas.fill_rect(x + body_width, (height - terminal_height) / 2, terminal_width, terminal_height, true);
        const std::string text = rtc.battery >= 0 ? std::to_string(rtc.battery) + "%" : "?";
        canvas.draw_text(font, x + (body_width - text.size() * font.width * e) / 2,
                         y + (body_height - font.height * e) / 2, text, true, e);
    }
}

static void draw_indicators(idfxx::epaper::mono_framebuffer &fb, std::optional<unsigned> sel)
{
    if (INDICATOR_SCALE == 0) {
        return;
    }
    idfxx::gfx::canvas canvas(fb);
    if (CONFIG_SHOW_POSITION && sel) {
        draw_position(canvas, *sel);
    }
    draw_status(fb);
}

// Mide la batería y actualiza el % que se muestra, en escalones de 5 y con
// histéresis: sólo cambia si la medida se aleja más de 3 puntos de lo que hay
// en pantalla, para no repintar por el ruido de la medida (cada repintado
// gasta batería y se nota).
static void update_battery()
{
    if (!CONFIG_SHOW_BATTERY || INDICATOR_SCALE == 0) {
        return;
    }
    const int pct = battery_percent(battery_mv());
    if (pct < 0) {
        rtc.battery = -1;
    } else if (rtc.battery < 0 || std::abs(pct - rtc.battery) > 3) {
        rtc.battery = static_cast<int8_t>((pct + 2) / 5 * 5);
    }
}

// Descomprime una imagen zlib directamente en el framebuffer. Falla si el
// stream está corrupto (zlib lleva Adler-32) o no mide exactamente un frame.
static bool decompress_image(std::string_view z, idfxx::epaper::mono_framebuffer &fb)
{
    auto out = fb.data();
    // El estado del descompresor (~11 KB) va al heap: no cabe en la pila
    auto dec = std::make_unique<tinfl_decompressor>();
    tinfl_init(dec.get());
    size_t len_in = z.size();
    size_t len_out = out.size();
    tinfl_status st = tinfl_decompress(dec.get(), reinterpret_cast<const mz_uint8 *>(z.data()), &len_in,
                                       out.data(), out.data(), &len_out,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || len_out != out.size()) {
        ESP_LOGE(TAG, "Imagen inválida (estado %d, %u bytes; se esperaban %u)", st, (unsigned)len_out,
                 (unsigned)out.size());
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
static void paint(const idfxx::epaper::mono_framebuffer &frame, const idfxx::epaper::mono_framebuffer *previous)
{
    idfxx::spi::bus_config bus_cfg{};
    bus_cfg.mosi = idfxx::gpio_9;
    bus_cfg.sclk = idfxx::gpio_7;
    idfxx::spi::master_bus bus(idfxx::spi::host_device::spi2, idfxx::spi::dma_chan::ch_auto, bus_cfg);

    idfxx::panel_io io(bus, idfxx::epaper::uc8179::spi_io_config(idfxx::gpio_10 /*CS*/, idfxx::gpio_11 /*DC*/));
    idfxx::epaper::uc8179 display(io, {.reset_gpio = idfxx::gpio_12, .busy_gpio = idfxx::gpio_13});

    auto mode = idfxx::epaper::refresh_mode::full;
    if (previous != nullptr) {
        display.restore_baseline(*previous);
        mode = idfxx::epaper::refresh_mode::partial;
    }
    display.write(frame);
    display.refresh(mode);
    display.sleep();
}

// PSRAM detectada en bytes (0 = no hay o no se inicializó: modo equivocado,
// p. ej. Quad en vez de Octal; con SPIRAM_IGNORE_NOTFOUND arranca igualmente)
static size_t psram_size()
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized() ? esp_psram_get_size() : 0;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static constexpr EventBits_t BIT_SUBSCRIBED = BIT0;
static constexpr EventBits_t BIT_CHANGED = BIT1;  // se ha guardado o borrado alguna imagen
static constexpr EventBits_t BIT_PUBLISHED = BIT2;  // PUBACK del estado
static EventGroupHandle_t events;
static std::string chunks;  // mensaje troceado en curso
static bool chunks_valid;
static int chunk_index;  // hueco del mensaje en curso (-1 = topic que no es de imágenes)
static volatile int64_t last_event_us;
static volatile bool mqtt_connected;

// Sincronización completa en curso: huecos que han llegado desde la suscripción
// (bajo mtx). Al terminar se borran los demás, que ya no están en el broker.
static bool full_sync;
static bool subscription_rejected;
static std::bitset<MAX_IMAGES> received;

static const std::string &image_filter()
{
    static const std::string filter = CONFIG_MQTT_IMAGE_TOPIC "/+";
    return filter;
}

// Lo que se apunta en sub.txt: con más huecos hay índices que la sesión no ha
// traído nunca (se ignoraban), así que cambiar MAX_IMAGES obliga a resincronizar.
static const std::string &subscription_signature()
{
    static const std::string signature = image_filter() + " " + std::to_string(MAX_IMAGES);
    return signature;
}

// Índice de un topic <MQTT_TOPIC_IMAGEN>/<n>, o -1 si no lo es o se sale de rango.
static int topic_index(std::string_view topic)
{
    constexpr std::string_view base = CONFIG_MQTT_IMAGE_TOPIC "/";
    if (!topic.starts_with(base) || topic.size() == base.size() || topic.size() > base.size() + 3) {
        return -1;
    }
    const std::string_view number = topic.substr(base.size());
    if (number.size() > 1 && number[0] == '0') {
        return -1;  // "07" no es el hueco 7: un solo topic por hueco
    }
    int index = 0;
    for (char c : number) {
        if (c < '0' || c > '9') {
            return -1;
        }
        index = index * 10 + (c - '0');
    }
    return index < static_cast<int>(MAX_IMAGES) ? index : -1;
}

#ifdef MQTT_CUSTOM_CA
// main/certs/mqtt_ca.crt incrustado por CMake (EMBED_TXTFILES lo termina en '\0')
extern const char mqtt_ca_pem[] asm("_binary_mqtt_ca_crt_start");
#endif

// Guarda la imagen de un mensaje completo en su hueco, o lo borra si el
// payload está vacío (retenido borrado en el broker).
static void store_received(unsigned index, std::string_view payload)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        if (payload.empty()) {
            received.set(index);
            if (remove(image_path(index).c_str()) == 0) {
                ESP_LOGI(TAG, "Borrada la imagen %u", index);
                xEventGroupSetBits(events, BIT_CHANGED);
            }
        } else {
            idfxx::epaper::mono_framebuffer fb(WIDTH, HEIGHT);
            if (decompress_image(payload, fb)) {
                received.set(index);
                // En una sincronización completa llegan también las que no han
                // cambiado: no se reescriben. Basta comparar el CRC de la
                // cabecera (sin cargar otro framebuffer de 48 KB).
                auto data = fb.data();
                if (stored_crc(image_path(index).c_str()) == esp_rom_crc32_le(0, data.data(), data.size())) {
                    ESP_LOGI(TAG, "Imagen %u sin cambios", index);
                } else if (save_frame(image_path(index).c_str(), fb)) {
                    ESP_LOGI(TAG, "Guardada la imagen %u (%u bytes comprimida)", index, (unsigned)payload.size());
                    xEventGroupSetBits(events, BIT_CHANGED);
                }
            }
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "No se pudo guardar la imagen %u: %s", index, e.what());
        // Sin memoria no se sabe si la imagen era válida: que la sincronización
        // completa no borre su hueco (se repetirá en el siguiente despertar)
        full_sync = false;
    }
    xSemaphoreGive(mtx);
}

static void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        subscription_rejected = false;
        last_event_us = esp_timer_get_time();
        xEventGroupSetBits(events, BIT_CHANGED);  // modo pruebas: repintar el icono de red
        if (event->session_present && read_subscription() == subscription_signature()) {
            // La suscripción sigue en el broker: sin volver a suscribirse sólo
            // llega lo publicado mientras dormía
            ESP_LOGI(TAG, "Sesión conservada en el broker: sólo cambios");
            xEventGroupSetBits(events, BIT_SUBSCRIBED);
        } else {
            ESP_LOGI(TAG, "Sincronización completa de %s (sesión previa: %d)", image_filter().c_str(),
                     event->session_present);
            xSemaphoreTake(mtx, portMAX_DELAY);
            full_sync = true;
            received.reset();
            remove(SUBSCRIPTION_PATH);  // se vuelve a escribir sólo si esta termina bien
            xSemaphoreGive(mtx);
            esp_mqtt_client_subscribe(event->client, image_filter().c_str(), 1);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        xEventGroupSetBits(events, BIT_CHANGED);  // modo pruebas: repintar el icono de red
        break;
    case MQTT_EVENT_SUBSCRIBED:
        last_event_us = esp_timer_get_time();
        // esp-mqtt avisa también de un SUBACK rechazado (0x80, p. ej. sin
        // permiso de lectura en el ACL): sin suscripción no llegaría nada y la
        // sincronización completa borraría todas las imágenes.
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            ESP_LOGE(TAG, "El broker rechaza la suscripción a %s (¿permiso de lectura?)", image_filter().c_str());
            xSemaphoreTake(mtx, portMAX_DELAY);
            full_sync = false;
            subscription_rejected = true;
            remove(SUBSCRIPTION_PATH);
            xSemaphoreGive(mtx);
        }
        xEventGroupSetBits(events, BIT_SUBSCRIBED);
        break;
    case MQTT_EVENT_PUBLISHED:
        xEventGroupSetBits(events, BIT_PUBLISHED);
        break;
    case MQTT_EVENT_DATA:
        // Los mensajes grandes llegan troceados: se reensamblan aquí. El topic
        // sólo viene en el primer trozo. Una excepción aquí (bad_alloc) no puede
        // salir al código C de esp-mqtt: abortaría.
        try {
            if (event->current_data_offset == 0) {
                chunks.clear();
                chunk_index = topic_index(std::string_view(event->topic, event->topic_len));
                chunks_valid = chunk_index >= 0 && static_cast<size_t>(event->total_data_len) <= MAX_COMPRESSED_SIZE;
                if (chunk_index < 0) {
                    ESP_LOGW(TAG, "Mensaje en %.*s ignorado (fuera de %s, o índice >= %u)", event->topic_len,
                             event->topic, image_filter().c_str(), MAX_IMAGES);
                } else if (!chunks_valid) {
                    ESP_LOGW(TAG, "Mensaje de %d bytes ignorado (máx %u)", event->total_data_len,
                             (unsigned)MAX_COMPRESSED_SIZE);
                }
                if (chunks_valid) {
                    chunks.reserve(event->total_data_len);
                }
            }
            if (chunks_valid) {
                chunks.append(event->data, event->data_len);
                if (static_cast<int>(chunks.size()) == event->total_data_len) {
                    chunks_valid = false;
                    store_received(chunk_index, chunks);
                    std::string().swap(chunks);  // libera los hasta 64 KB
                }
            }
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "Mensaje descartado: %s", e.what());
            chunks_valid = false;
            std::string().swap(chunks);
            xSemaphoreTake(mtx, portMAX_DELAY);
            full_sync = false;  // ver store_received()
            xSemaphoreGive(mtx);
        }
        last_event_us = esp_timer_get_time();
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
// publicados mientras dormimos (ver el comentario del principio).
static esp_mqtt_client_handle_t connect_network(bool &connected)
{
    connected = false;
    if (!wifi_connect()) {
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
#ifdef MQTT_CUSTOM_CA
    mqtt_cfg.broker.verification.certificate = mqtt_ca_pem;
#else
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    mqtt_cfg.credentials.client_id = client_id;
    if (CONFIG_MQTT_USERNAME[0] != '\0') {
        mqtt_cfg.credentials.username = CONFIG_MQTT_USERNAME;
        mqtt_cfg.credentials.authentication.password = CONFIG_MQTT_PASSWORD;
    }
    mqtt_cfg.session.disable_clean_session = true;
    mqtt_cfg.buffer.size = 4096;  // menos trozos al recibir imágenes
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(client);

    connected = (xEventGroupWaitBits(events, BIT_SUBSCRIBED, pdFALSE, pdTRUE,
                                     pdMS_TO_TICKS(CONFIG_CONNECT_TIMEOUT_S * 1000)) & BIT_SUBSCRIBED) &&
                !subscription_rejected;
    if (!connected) {
        ESP_LOGE(TAG, "No se pudo conectar/suscribir al broker");
    }
    return client;
}

// Seguro de llamar aunque no se haya intentado conectar (despertar por botón).
[[maybe_unused]] static void disconnect_network(esp_mqtt_client_handle_t client)
{
    if (client != nullptr) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
    wifi_disconnect();
}

// Cierra una sincronización completa: borra los huecos que no han llegado (ya
// no están retenidos en el broker) y apunta la suscripción, para que los
// siguientes despertares se fíen de la sesión. Si se ha cortado la conexión a
// mitad no se hace nada y el siguiente despertar vuelve a empezar.
static void finish_sync()
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (full_sync && mqtt_connected) {
        full_sync = false;
        for (unsigned i = 0; i < MAX_IMAGES; ++i) {
            if (!received.test(i) && remove(image_path(i).c_str()) == 0) {
                ESP_LOGI(TAG, "Borrada la imagen %u (ya no está en el broker)", i);
                xEventGroupSetBits(events, BIT_CHANGED);
            }
        }
        save_subscription(subscription_signature());
        ESP_LOGI(TAG, "Sincronización completa terminada (%u imágenes)", (unsigned)received.count());
    }
    xSemaphoreGive(mtx);
}

// Espera hasta que pase VENTANA_MS sin mensajes (la cola ya se ha vaciado).
static void wait_for_queue()
{
    while (esp_timer_get_time() - last_event_us < CONFIG_QUEUE_WINDOW_MS * 1000LL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    finish_sync();
    xEventGroupClearBits(events, BIT_CHANGED);
}

static const char *wakeup_cause = "arranque";

// Publica (retenido) el estado del dispositivo en MQTT_TOPIC_ESTADO y espera
// el PUBACK, para que no se pierda al desconectar justo después.
static void publish_status(esp_mqtt_client_handle_t client)
{
    if (CONFIG_MQTT_STATUS_TOPIC[0] == '\0') {
        return;
    }
    std::string images;
    xSemaphoreTake(mtx, portMAX_DELAY);
    const auto sel = selection();
    for (unsigned i = 0; i < MAX_IMAGES; ++i) {
        if (image_exists(i)) {
            images += (images.empty() ? "" : ",") + std::to_string(i);
        }
    }
    xSemaphoreGive(mtx);
    wifi_ap_record_t ap{};
    const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;

    char json[1280];  // cabe la lista de 255 huecos
    snprintf(json, sizeof(json),
             R"({"firmware":"%s","despertar":"%s","seleccion":%d,"imagenes":[%s],"rssi":%d,"bateria":%d,)"
             R"("psram":%u,"despierto_ms":%lld})",
             esp_app_get_description()->version, wakeup_cause, sel ? static_cast<int>(*sel) : -1,
             images.c_str(), rssi, rtc.battery, static_cast<unsigned>(psram_size()), esp_timer_get_time() / 1000);
    xEventGroupClearBits(events, BIT_PUBLISHED);
    if (esp_mqtt_client_publish(client, CONFIG_MQTT_STATUS_TOPIC, json, 0, 1, 1) < 0 ||
        !(xEventGroupWaitBits(events, BIT_PUBLISHED, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000)) & BIT_PUBLISHED)) {
        ESP_LOGW(TAG, "No se pudo publicar el estado");
    }
}

// Conexión de este despertar: se abre la primera vez que hace falta (un solo
// intento) y se mantiene hasta dormir.
static esp_mqtt_client_handle_t mqtt_client;
static bool attempted, connected;

// Estado de la red ahora mismo. "ok" = suscrito al broker: lo que importa es
// poder recibir imágenes, no sólo tener WiFi.
static net_state current_network()
{
    if (!wifi_connected()) {
        return net_state::no_wifi;
    }
    return mqtt_connected && !subscription_rejected ? net_state::ok : net_state::no_broker;
}

static void sync_mqtt()
{
    rtc.t_next_check = time(nullptr) + CONFIG_INTERVAL_S;
    if (!attempted) {
        attempted = true;
        mqtt_client = connect_network(connected);
    }
    if (connected) {
        wait_for_queue();
        publish_status(mqtt_client);
    }
}

// ---------------------------------------------------------------------------
// Botones
// ---------------------------------------------------------------------------
static void setup_buttons()
{
    gpio_config_t cfg{};
    cfg.pin_bit_mask = BUTTON_MASK;
    cfg.mode = GPIO_MODE_INPUT;  // pull-up externo
    gpio_config(&cfg);
}

static button read_buttons()
{
    if (gpio_get_level(GPIO_PREV) == 0) {
        return button::previous;
    }
    if (gpio_get_level(GPIO_NEXT) == 0) {
        return button::next;
    }
    if (gpio_get_level(GPIO_REFRESH) == 0) {
        return button::refresh;
    }
    return button::none;
}

// Botón pulsado (estable 30 ms) antes de `timeout_ms`; en modo pruebas
// también vuelve, con ninguno, si llega una imagen.
static button wait_for_button(int64_t timeout_ms)
{
    const int64_t end = esp_timer_get_time() + timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        button b = read_buttons();
        if (b != button::none) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (read_buttons() == b) {
                return b;
            }
        }
#if CONFIG_NO_DEEP_SLEEP
        if (xEventGroupGetBits(events) & BIT_CHANGED) {
            return button::none;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return button::none;
}

// Espera a que se suelten todos los botones; false si alguno sigue pulsado.
static bool wait_for_release()
{
    for (int i = 0; i < 250; ++i) {  // 5 s
        if (read_buttons() == button::none) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "Botón atascado: no despertará con los botones");
    return false;
}

// Botón que ha despertado al ESP32, si ha sido uno.
static button wakeup_button()
{
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) {
        return button::none;
    }
    const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    if (pins & (1ULL << GPIO_PREV)) {
        return button::previous;
    }
    if (pins & (1ULL << GPIO_NEXT)) {
        return button::next;
    }
    return button::refresh;
}

// ---------------------------------------------------------------------------

[[maybe_unused, noreturn]] static void go_to_sleep(bool wake_on_buttons)
{
    const time_t now = time(nullptr);
    const time_t sleep_s = std::max<time_t>(rtc.t_next_check - now, 1);
    ESP_LOGI(TAG, "Despierto %lld ms; a deep sleep %lld s", esp_timer_get_time() / 1000, (long long)sleep_s);
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleep_s) * 1000000ULL);
    if (wake_on_buttons) {
        esp_sleep_enable_ext1_wakeup_io(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    esp_deep_sleep_start();
}

// Pinta la imagen seleccionada si no es lo que ya hay en el cristal. Si no hay
// ninguna guardada pinta `aviso` (si lo hay). `forzar` = refresco completo
// aunque no haya cambios (limpia los restos de los parciales).
static void show(bool lfs, const char *notice, bool force)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        // Frame que hay en el cristal, si nos fiamos de él
        std::optional<idfxx::epaper::mono_framebuffer> previous;
        if (rtc.magic == RTC_MAGIC && lfs) {
            previous.emplace(WIDTH, HEIGHT);
            if (!load_frame(FRAME_PATH, *previous)) {
                previous.reset();
            }
        }

        idfxx::epaper::mono_framebuffer frame(WIDTH, HEIGHT);
        std::optional<unsigned> sel;
        bool has_image = false;
        while (lfs && !has_image && (sel = selection())) {
            has_image = load_frame(image_path(*sel).c_str(), frame);
            if (!has_image) {
                // Corrupta: se borra y se pasa a la siguiente. Sin sub.txt, la
                // próxima conexión hace una sincronización completa y la recupera
                // del broker si sigue retenida.
                remove(image_path(*sel).c_str());
                remove(SUBSCRIPTION_PATH);
            }
        }
        if (attempted) {
            rtc.network = current_network();  // con conexión abierta en este despertar, su estado real
        }
        draw_indicators(frame, has_image ? sel : std::nullopt);
        if (!has_image) {
            if (notice == nullptr) {
                xSemaphoreGive(mtx);
                return;
            }
            draw_message(frame, notice);
        }
        if (!force && previous && std::ranges::equal(previous->data(), frame.data())) {
            ESP_LOGI(TAG, "Sin cambios en la imagen, no se refresca");
            xSemaphoreGive(mtx);
            return;
        }

        const time_t now = time(nullptr);
        const bool partial = !force && previous && rtc.partials < CONFIG_MAX_PARTIALS &&
                             now - rtc.t_full < CONFIG_FULL_REFRESH_H * 3600;
        if (has_image) {
            ESP_LOGI(TAG, "Imagen %u, refresco %s (parciales=%" PRIu32 ")", *sel, partial ? "parcial" : "completo",
                     rtc.partials);
        }

        rtc.magic = 0;  // mientras se pinta, el cristal está en estado incierto
        paint(frame, partial ? &*previous : nullptr);
        if (partial) {
            rtc.partials++;
        } else {
            rtc.partials = 0;
            rtc.t_full = now;
        }
        if (lfs && save_frame(FRAME_PATH, frame)) {
            rtc.magic = RTC_MAGIC;
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error: %s", e.what());
        rtc.magic = 0;  // el próximo pintado será completo
    }
    xSemaphoreGive(mtx);
}

static const char *notice()
{
    static const std::string offline_text = "Sin conexion.\nReintentando cada " + std::to_string(CONFIG_INTERVAL_S) + " s";
    return connected ? "Esperando imagenes en\n" CONFIG_MQTT_IMAGE_TOPIC "/<n>" : offline_text.c_str();
}

// Anterior/siguiente: sólo cambia de imagen, sin red. Refresco (y despertar
// por tiempo o arranque, `ninguno`): recoge la cola MQTT y pinta la seleccionada.
static void handle_button(button b, bool lfs)
{
    switch (b) {
    case button::previous:
    case button::next:
        navigate(b == button::previous ? -1 : 1);
        show(lfs, nullptr, false);
        break;
    case button::refresh:
        ESP_LOGI(TAG, "Botón de refresco");
        sync_mqtt();
        show(lfs, notice(), true);
        break;
    case button::none:
        sync_mqtt();
        show(lfs, notice(), false);
        break;
    }
}

extern "C" void app_main()
{
    sd_release_bus();
    ESP_LOGI(TAG, "PSRAM: %u KB; RAM interna libre: %u KB", static_cast<unsigned>(psram_size() / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    setup_buttons();
    events = xEventGroupCreate();
    mtx = xSemaphoreCreateMutex();

    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        rtc = {};  // arranque en frío, flasheo, crash...: no sabemos qué hay en el cristal
    }
    const bool lfs = mount_littlefs();
    update_battery();  // antes de encender la WiFi: sin la caída de tensión al transmitir

    // Si despierta un botón de navegación no se toca la red; la comprobación
    // de MQTT sigue programada para rtc.t_next_check (ver dormir()).
    button b = wakeup_button();
    if (b != button::none) {
        wakeup_cause = "boton";
    } else if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        wakeup_cause = "temporizador";
    }
    handle_button(b, lfs);

#if CONFIG_NO_DEEP_SLEEP
    // Modo pruebas: seguir conectado, pintar cada imagen según llega y atender los botones
    ESP_LOGW(TAG, "Modo pruebas: sin deep sleep");
    while (true) {
        wait_for_release();
        b = wait_for_button(60000);  // como mucho cada minuto, para seguir la batería
        if (b == button::none) {
            wait_for_queue();
            update_battery();
            show(lfs, notice(), false);
        } else {
            handle_button(b, lfs);
        }
    }
#else
    // Si ha despertado un botón, unos segundos para seguir pasando imágenes sin
    // volver a despertar. Tras un despertar por tiempo no: sería radio encendida
    // esperando una pulsación que casi nunca llega.
    bool released = wait_for_release();
    while (b != button::none && released && (b = wait_for_button(CONFIG_BUTTON_WINDOW_MS)) != button::none) {
        handle_button(b, lfs);
        released = wait_for_release();
    }
    disconnect_network(mqtt_client);
    go_to_sleep(released);
#endif
}
