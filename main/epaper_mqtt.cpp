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
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <string_view>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
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

// Las opciones bool de Kconfig no se definen cuando están desactivadas: no
// valen como expresión. Aquí, como constantes que siempre existen.
#ifdef CONFIG_SHOW_POSITION
static constexpr bool SHOW_POSITION = true;
#else
static constexpr bool SHOW_POSITION = false;
#endif
#ifdef CONFIG_SHOW_NETWORK
static constexpr bool SHOW_NETWORK = true;
#else
static constexpr bool SHOW_NETWORK = false;
#endif
#ifdef CONFIG_SHOW_BATTERY
static constexpr bool SHOW_BATTERY = true;
#else
static constexpr bool SHOW_BATTERY = false;
#endif
#ifdef CONFIG_NO_DEEP_SLEEP
static constexpr bool NO_DEEP_SLEEP = true;
#else
static constexpr bool NO_DEEP_SLEEP = false;
#endif

// ---------------------------------------------------------------------------
// Estado que sobrevive al deep sleep (y a nada más: cualquier otro reset lo
// invalida, y entonces no se sabe qué hay en el cristal). Los tiempos son de
// time(), que el RTC sigue contando durante el deep sleep.
// ---------------------------------------------------------------------------
static constexpr uint32_t RTC_MAGIC = 0x45504D51;
// Resultado de la última conexión (lo que muestra el icono de WiFi)
enum class net_state : uint8_t { unknown, ok, no_wifi, no_broker, syncing };
struct rtc_state {
    uint32_t magic;       // RTC_MAGIC = el frame guardado coincide con el cristal
    uint32_t partials;   // parciales desde el último completo
    time_t t_full;        // último refresco completo
    time_t t_next_check;     // próxima comprobación de MQTT
    time_t t_last_ntp;       // última hora correcta por SNTP (0 = nunca en este arranque)
    net_state network;
    int8_t battery;       // % que se muestra (múltiplo de 5), -1 = sin medida válida
    int16_t battery_mv;   // última medida, para el estado MQTT (-1 = falló)
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
    // Con stat(), no access(): el VFS de LittleFS no implementa access y
    // siempre devuelve -1 (ENOSYS), con lo que ninguna imagen "existía"
    struct stat st;
    return stat(image_path(index).c_str(), &st) == 0 && S_ISREG(st.st_mode);
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

// Mueve la selección `steps` imágenes (negativo = hacia atrás) entre las que
// existen, dando la vuelta. false si no se ha movido (ninguna o sólo una).
static bool navigate_by(int steps)
{
    const auto current = selection();
    if (!current) {
        ESP_LOGI(TAG, "No hay imágenes guardadas");
        return false;
    }
    std::vector<unsigned> existing;
    for (unsigned i = 0; i < MAX_IMAGES; ++i) {
        if (image_exists(i)) {
            existing.push_back(i);
        }
    }
    const int n = static_cast<int>(existing.size());
    const int pos = static_cast<int>(std::ranges::find(existing, *current) - existing.begin());
    const unsigned target = existing[((pos + steps) % n + n) % n];
    if (target == *current) {
        ESP_LOGI(TAG, "Imagen %u (no cambia: %d imágenes)", *current, n);
        return false;
    }
    ESP_LOGI(TAG, "Imagen %u -> %u (%+d)", *current, target, steps);
    save_selection(target);
    return true;
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
// fondo blanco liso (sin contorno), para que se lean sobre cualquier imagen.
// ---------------------------------------------------------------------------
// Todo se mide a partir de la altura del texto en px (CONFIG_INDICATOR_HEIGHT):
// la fuente Spleen 8x16 se escala por K, que puede ser fraccionario.
static constexpr float K = CONFIG_INDICATOR_HEIGHT / 16.0f;

// Unidad de la fuente (1/16 del alto del texto) en px, redondeada
static constexpr size_t px(float units)
{
    return static_cast<size_t>(units * K + 0.5f);
}

// Texto con la fuente escalada por `k` (vecino más cercano: cada píxel toma el
// del glifo bajo su centro). Con k fraccionario algunos trazos salen de un
// píxel más que otros, pero los glifos de Spleen siguen leyéndose bien.
static void draw_text_scaled(idfxx::epaper::mono_framebuffer &fb, const idfxx::font::mono_font &font, size_t x,
                             size_t y, std::string_view text, float k)
{
    const size_t cell_w = static_cast<size_t>(font.width * k + 0.5f);
    const size_t cell_h = static_cast<size_t>(font.height * k + 0.5f);
    for (char c : text) {
        if (font.contains(c)) {
            for (size_t row = 0; row < cell_h; ++row) {
                const size_t src_row = std::min<size_t>(font.height - 1, static_cast<size_t>((row + 0.5f) / k));
                for (size_t col = 0; col < cell_w; ++col) {
                    const size_t src_col = std::min<size_t>(font.width - 1, static_cast<size_t>((col + 0.5f) / k));
                    if (font.ink_at(c, src_col, src_row)) {
                        fb.set_pixel(x + col, y + row, true);
                    }
                }
            }
        }
        x += cell_w;
    }
}

// Ancho de `text` con la fuente de los indicadores
static size_t text_width(std::string_view text)
{
    return text.size() * px(idfxx::font::spleen_8x16.width);
}

// Fondo blanco liso (sin contorno) bajo un indicador, para que se lea sobre la imagen
static void clear_area(idfxx::gfx::canvas<idfxx::epaper::mono_framebuffer> &canvas, size_t x, size_t y, size_t width,
                       size_t height)
{
    canvas.fill_rect(x, y, width, height, false);
}

// Posición de `indice` entre las N imágenes guardadas (no el número de hueco)
static void draw_position(idfxx::epaper::mono_framebuffer &fb, unsigned index, bool only_label = false)
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
    const size_t padding = px(2);
    idfxx::gfx::canvas canvas(fb);
    clear_area(canvas, 0, 0, text_width(text) + 2 * padding, px(16) + 2 * padding);
    if (only_label) {
        // Sobre lo que hay en el cristal: borrar también lo que ocupara una
        // etiqueta más larga ("10/10" -> "9/10"); la imagen lo repinta luego
        const std::string widest = std::to_string(MAX_IMAGES) + "/" + std::to_string(MAX_IMAGES);
        clear_area(canvas, 0, 0, text_width(widest) + 2 * padding, px(16) + 2 * padding);
    }
    draw_text_scaled(fb, idfxx::font::spleen_8x16, padding, padding, text, K);
}

// true mientras hay conexión con el broker en este despertar: el icono de WiFi
// va sin luna. Al desconectar para dormir vuelve la luna (sleep_view()).
static bool online;

// Luna creciente de `size` px en (x0, y0)
static void draw_moon(idfxx::epaper::mono_framebuffer &fb, size_t x0, size_t y0, size_t size)
{
    const float r = size / 2.0f;
    const float cx = x0 + r, cy = y0 + r;
    const float bx = cx + r * 0.5f, by = cy - r * 0.35f;  // disco que "muerde" la luna
    const float rb = r * 0.8f;
    for (size_t y = y0; y < y0 + size; ++y) {
        for (size_t x = x0; x < x0 + size; ++x) {
            const float px_ = x + 0.5f, py = y + 0.5f;
            const bool in_moon = (px_ - cx) * (px_ - cx) + (py - cy) * (py - cy) <= r * r;
            const bool in_bite = (px_ - bx) * (px_ - bx) + (py - by) * (py - by) <= rb * rb;
            if (in_moon && !in_bite) {
                fb.set_pixel(x, y, true);
            }
        }
    }
}

// Icono de WiFi (punto + 3 arcos) en un recuadro de ancho x alto que empieza
// en (x0, y0). Tachado sin WiFi; con "!" si hay WiFi pero no broker; con una
// luna si fue bien y duerme (modo batería).
static void draw_wifi_icon(idfxx::epaper::mono_framebuffer &fb, size_t x0, size_t y0, size_t width, size_t height,
                         net_state network)
{
    idfxx::gfx::canvas canvas(fb);
    if (network == net_state::syncing) {
        // "…": conectando tras pulsar refresco
        const float r = height * 0.1f;
        for (float fx : {0.2f, 0.5f, 0.8f}) {
            const float cx = x0 + width * fx, cy = y0 + height * 0.6f;
            for (size_t y = y0; y < y0 + height; ++y) {
                for (size_t x = x0; x < x0 + width; ++x) {
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                        fb.set_pixel(x, y, true);
                    }
                }
            }
        }
        return;
    }
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
            const int thickness = static_cast<int>(pass == 0 ? px(4) : px(1));
            for (int k = -thickness; k <= thickness; ++k) {
                const int xa = std::clamp(static_cast<int>(x0) + k, static_cast<int>(x0), static_cast<int>(x0 + width - 1));
                const int xb = std::clamp(static_cast<int>(x0 + width - 1) + k, static_cast<int>(x0), static_cast<int>(x0 + width - 1));
                canvas.draw_line(xa, y0, xb, y0 + height - 1, pass == 1);
            }
        }
    } else if (network == net_state::no_broker) {
        const auto &font = idfxx::font::spleen_5x8;
        const size_t w = px(font.width), h = px(font.height);
        canvas.fill_rect(x0 + width - w - 2, y0, w + 2, h + 1, false);
        draw_text_scaled(fb, font, x0 + width - w - 1, y0, "!", K);
    } else if (network == net_state::ok && !NO_DEEP_SLEEP && !online) {
        // Luna en la misma esquina que el "!": la última comprobación fue bien
        // y está durmiendo (sin conexión ahora mismo).
        const size_t size = px(9);
        canvas.fill_rect(x0 + width - size - 2, y0, size + 2, size + 1, false);
        draw_moon(fb, x0 + width - size - 1, y0, size);
    }
}

// WiFi y batería en un recuadro arriba a la derecha
static void draw_status(idfxx::epaper::mono_framebuffer &fb)
{
    const bool show_network = SHOW_NETWORK && rtc.network != net_state::unknown;
    const bool show_battery = SHOW_BATTERY;
    if (!show_network && !show_battery) {
        return;
    }
    const size_t padding = px(2);
    const size_t text_height = px(16);
    const size_t height = text_height + 2 * padding;  // igual que el de "k/N"
    const size_t icon_height = text_height;
    const size_t icon_width = 2 * static_cast<size_t>(std::ceil(0.7072f * (icon_height * 0.88f))) + 1;
    const size_t body_width = text_width("100%") + px(4);
    const size_t body_height = height - px(2);
    const size_t terminal_width = px(2), terminal_height = px(6);
    const size_t outline = std::max<size_t>(1, px(1));

    size_t width = padding;
    if (show_network) {
        width += icon_width + padding;
    }
    if (show_battery) {
        width += body_width + terminal_width + 2 * padding;  // el polo no pegado al borde
    }
    const size_t x0 = fb.width() - width;
    idfxx::gfx::canvas canvas(fb);
    clear_area(canvas, x0, 0, width, height);

    size_t x = x0 + padding;
    if (show_network) {
        draw_wifi_icon(fb, x, padding, icon_width, icon_height, rtc.network);
        x += icon_width + padding;
    }

    if (show_battery) {
        const size_t y = (height - body_height) / 2;
        for (size_t g = 0; g < outline; ++g) {
            canvas.draw_rect(x + g, y + g, body_width - 2 * g, body_height - 2 * g, true);
        }
        canvas.fill_rect(x + body_width, (height - terminal_height) / 2, terminal_width, terminal_height, true);
        const std::string text = rtc.battery >= 0 ? std::to_string(rtc.battery) + "%" : "?";
        draw_text_scaled(fb, idfxx::font::spleen_8x16, x + (body_width - text_width(text)) / 2,
                         y + (body_height - text_height) / 2, text, K);
    }
}

static void draw_indicators(idfxx::epaper::mono_framebuffer &fb, std::optional<unsigned> sel)
{
    if (CONFIG_INDICATOR_HEIGHT == 0) {
        return;
    }
    if (SHOW_POSITION && sel) {
        draw_position(fb, *sel);
    }
    draw_status(fb);
}

// Mide la batería y actualiza el % que se muestra, en escalones de 5 y con
// histéresis: sólo cambia si la medida se aleja más de 3 puntos de lo que hay
// en pantalla, para no repintar por el ruido de la medida (cada repintado
// gasta batería y se nota).
static void update_battery()
{
    if (!SHOW_BATTERY || CONFIG_INDICATOR_HEIGHT == 0) {
        return;
    }
    rtc.battery_mv = static_cast<int16_t>(battery_mv());
    const int pct = battery_percent(rtc.battery_mv);
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

// ---------------------------------------------------------------------------
// Panel. Se mantiene encendido, y con el modo de refresco ya cargado, mientras
// dura una tanda de pintados (p. ej. varias pulsaciones seguidas): cada cambio
// es entonces "enviar la zona que cambia + refresco", sin reset, sin volver a
// encender la alimentación (100 ms + BUSY cada vez) y sin mandar 48 KB.
// end_painting() lo duerme y guarda frame.bin.
// ---------------------------------------------------------------------------
static idfxx::spi::bus_config panel_bus_config()
{
    idfxx::spi::bus_config cfg{};
    cfg.mosi = idfxx::gpio_9;
    cfg.sclk = idfxx::gpio_7;
    return cfg;
}

struct panel_session {
    idfxx::spi::master_bus bus{idfxx::spi::host_device::spi2, idfxx::spi::dma_chan::ch_auto, panel_bus_config()};
    idfxx::panel_io io{bus, idfxx::epaper::uc8179::spi_io_config(idfxx::gpio_10 /*CS*/, idfxx::gpio_11 /*DC*/)};
    idfxx::epaper::uc8179 display{io, {.reset_gpio = idfxx::gpio_12, .busy_gpio = idfxx::gpio_13}};
    // Al crearse, el driver pone en blanco las dos RAM del controlador. Tras el
    // primer pintado completo, la RAM "nueva" coincide con el cristal (y la
    // "anterior" también: N2OCP la copia en cada refresco), así que basta con
    // reescribir la zona que cambia.
    bool ram_matches_glass = false;
};
static std::unique_ptr<panel_session> panel;

// Rectángulo de bytes completos (x y ancho múltiplos de 8) donde `a` y `b`
// difieren; vacío (h == 0) si son iguales.
struct region {
    size_t x, y, w, h;
};
static region changed_region(const idfxx::epaper::mono_framebuffer &a, const idfxx::epaper::mono_framebuffer &b)
{
    const size_t stride = a.stride_bytes();
    const auto da = a.data(), db = b.data();
    size_t y0 = SIZE_MAX, y1 = 0, c0 = SIZE_MAX, c1 = 0;
    for (size_t y = 0; y < a.height(); ++y) {
        for (size_t c = 0; c < stride; ++c) {
            if (da[y * stride + c] != db[y * stride + c]) {
                y0 = std::min(y0, y);
                y1 = y;
                c0 = std::min(c0, c);
                c1 = std::max(c1, c);
            }
        }
    }
    if (y0 == SIZE_MAX) {
        return {0, 0, 0, 0};
    }
    return {c0 * 8, y0, (c1 - c0 + 1) * 8, y1 - y0 + 1};
}

// Pinta `frame`. Con `previous` (lo que hay en el cristal) refresco parcial,
// enviando sólo la zona que cambia si la RAM del controlador ya está al día;
// sin él, completo.
static void paint(const idfxx::epaper::mono_framebuffer &frame, const idfxx::epaper::mono_framebuffer *previous)
{
    if (!panel) {
        panel = std::make_unique<panel_session>();
    }
    auto &display = panel->display;
    if (previous == nullptr) {
        display.write(frame);
        display.refresh(idfxx::epaper::refresh_mode::full);
        panel->ram_matches_glass = true;
        return;
    }
    if (!panel->ram_matches_glass) {
        display.restore_baseline(*previous);  // RAM "anterior" = cristal
        display.write(frame);
    } else {
        const region r = changed_region(*previous, frame);
        if (r.h == 0) {
            return;
        }
        // Copia de la zona en un framebuffer pequeño y escritura en su sitio
        idfxx::epaper::mono_framebuffer part(r.w, r.h);
        const size_t stride = frame.stride_bytes(), part_stride = part.stride_bytes();
        const auto src = frame.data();
        auto dst = part.data();
        for (size_t y = 0; y < r.h; ++y) {
            std::copy_n(src.begin() + (r.y + y) * stride + r.x / 8, part_stride, dst.begin() + y * part_stride);
        }
        display.write(part, r.x, r.y);
    }
    display.refresh(idfxx::epaper::refresh_mode::partial);
    panel->ram_matches_glass = true;
}

// ---------------------------------------------------------------------------
// Hora. Se sincroniza por SNTP en cada conexión y el RTC la mantiene durante el
// deep sleep (con la deriva de su oscilador, que la siguiente conexión corrige).
// Las comprobaciones se alinean a múltiplos de INTERVAL_S desde la medianoche
// local: 3600 = cada hora en punto, 1800 = a las :00 y :30.
// ---------------------------------------------------------------------------
static bool clock_valid()
{
    return time(nullptr) > 1735689600;  // después del 1-1-2025: viene de SNTP, no del arranque
}

// Siguiente instante alineado estrictamente posterior a `from`. Sin hora
// real (nunca hubo SNTP), simplemente `from` + intervalo.
static time_t next_slot(time_t from)
{
    if (!clock_valid()) {
        return from + CONFIG_INTERVAL_S;
    }
    struct tm local {};
    localtime_r(&from, &local);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;  // que mktime decida si es horario de verano
    const time_t midnight = mktime(&local);
    return midnight + ((from - midnight) / CONFIG_INTERVAL_S + 1) * CONFIG_INTERVAL_S;
}

// Próxima comprobación tras una hecha ahora. El margen absorbe la deriva del
// RTC: despertar a las 10:59:57 (o a las 11:00:03) cuenta como la de las
// 11:00, y la siguiente es a las 12:00, sin despertar otra vez a los 3 s.
static time_t next_check_after_now()
{
    const time_t guard = std::min<time_t>(60, CONFIG_INTERVAL_S / 4);
    return next_slot(time(nullptr) + guard);
}

static std::string format_time(time_t t, const char *fmt)
{
    struct tm local {};
    localtime_r(&t, &local);
    char buf[32];
    strftime(buf, sizeof(buf), fmt, &local);
    return buf;
}

static bool sntp_started;

// Hora del RTC y del temporizador al pedir SNTP, para medir cuánto corrige
static time_t sntp_rtc_at_start;
static int64_t sntp_us_at_start;

static void on_time_sync(struct timeval *tv)
{
    // Lo que diría el RTC ahora frente a lo que dice el servidor: la deriva
    // acumulada desde la última sincronización
    const int64_t rtc_now_ms = sntp_rtc_at_start * 1000LL + (esp_timer_get_time() - sntp_us_at_start) / 1000;
    const int64_t correction_ms = tv->tv_sec * 1000LL + tv->tv_usec / 1000 - rtc_now_ms;
    if (rtc.t_last_ntp > 0) {
        ESP_LOGI(TAG, "SNTP: hora corregida %+lld ms tras %lld min sin sincronizar", (long long)correction_ms,
                 (long long)(tv->tv_sec - rtc.t_last_ntp) / 60);
    } else {
        ESP_LOGI(TAG, "SNTP: hora puesta");
    }
    rtc.t_last_ntp = tv->tv_sec;
}

// SNTP sólo si no hay hora real o la última sincronización tiene más de
// NTP_EVERY_H horas: entre medias el RTC la mantiene durante el deep sleep
static void start_sntp()
{
    if (sntp_started) {
        return;
    }
    const time_t now = time(nullptr);
    if (clock_valid() && rtc.t_last_ntp > 0 && now - rtc.t_last_ntp < CONFIG_NTP_EVERY_H * 3600) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_NTP_SERVER);
    cfg.sync_cb = on_time_sync;
    sntp_rtc_at_start = now;
    sntp_us_at_start = esp_timer_get_time();
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        sntp_started = true;
    } else {
        ESP_LOGW(TAG, "No se pudo iniciar SNTP");
    }
}

static void stop_sntp()
{
    if (sntp_started) {
        esp_netif_sntp_deinit();
        sntp_started = false;
    }
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
// El handler está dentro de un mensaje (entre trozos o guardándolo en flash,
// que puede tardar más que la ventana de espera): la cola aún no está vacía
static volatile bool handling;

// Sincronización completa en curso: huecos que han llegado desde la suscripción
// (bajo mtx). Al terminar se borran los demás, que ya no están en el broker.
static bool full_sync;
static bool subscription_rejected;
static volatile bool subscription_acked;  // SUBACK aceptado (o sesión conservada) en esta conexión
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

// Un mensaje no se ha podido guardar, pero el broker ya tiene su PUBACK y no lo
// repetirá en la sesión. Se cancela la sincronización completa en curso (para
// no borrar ese hueco) y se borra sub.txt: la próxima conexión se vuelve a
// suscribir y recibe de nuevo todos los retenidos. Con mtx tomado.
static void force_full_sync()
{
    full_sync = false;
    remove(SUBSCRIPTION_PATH);
}

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
                } else {
                    force_full_sync();
                }
            }
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "No se pudo guardar la imagen %u: %s", index, e.what());
        force_full_sync();
    }
    xSemaphoreGive(mtx);
}

static void handle_mqtt_event(int32_t event_id, esp_mqtt_event_handle_t event)
{
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        subscription_rejected = false;
        subscription_acked = false;
        last_event_us = esp_timer_get_time();
        xEventGroupSetBits(events, BIT_CHANGED);  // modo pruebas: repintar el icono de red
        if (event->session_present && read_subscription() == subscription_signature()) {
            // La suscripción sigue en el broker: sin volver a suscribirse sólo
            // llega lo publicado mientras dormía
            ESP_LOGI(TAG, "Sesión conservada en el broker: sólo cambios");
            subscription_acked = true;
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
        chunks_valid = false;  // el mensaje a medias no se completará
        handling = false;
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
        } else {
            subscription_acked = true;
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
        last_event_us = esp_timer_get_time();
        handling = true;
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
            force_full_sync();
            xSemaphoreGive(mtx);
        }
        handling = chunks_valid;  // a mitad de un mensaje troceado, sigue "dentro"
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

// Lo llama esp-mqtt (código C) desde su tarea: ninguna excepción puede salir de aquí
static void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    try {
        handle_mqtt_event(event_id, static_cast<esp_mqtt_event_handle_t>(event_data));
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Evento MQTT %" PRIi32 ": %s", event_id, e.what());
        handling = false;
    }
}

// Conecta WiFi + MQTT y espera a la suscripción. El client_id fijo y la
// sesión persistente hacen que el broker guarde en cola los mensajes QoS 1
// publicados mientras dormimos (ver el comentario del principio).
static esp_mqtt_client_handle_t connect_network(bool &connected)
{
    connected = false;
    // Una caída de la WiFi llega aquí al instante; esp-mqtt no se entera hasta
    // que falla el socket o el keepalive (minutos)
    wifi_set_listener([] { xEventGroupSetBits(events, BIT_CHANGED); });
    if (!wifi_connect()) {
        return nullptr;
    }
    start_sntp();  // en paralelo con MQTT; sync_mqtt() espera el resultado al final

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
    // El handler descomprime y guarda en LittleFS desde la tarea de MQTT (6 KB por defecto)
    mqtt_cfg.task.stack_size = 8192;
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
    stop_sntp();
    wifi_disconnect();
}

// Cierra una sincronización completa: borra los huecos que no han llegado (ya
// no están retenidos en el broker) y apunta la suscripción, para que los
// siguientes despertares se fíen de la sesión. Si se ha cortado la conexión a
// mitad no se hace nada y el siguiente despertar vuelve a empezar.
static void finish_sync()
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (full_sync && mqtt_connected && subscription_acked && !handling) {
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

// Espera hasta que pase QUEUE_WINDOW_MS sin mensajes y sin ninguno a medias
// (la cola ya se ha vaciado). Guardar una imagen en flash puede tardar más que
// la ventana: mientras el handler esté dentro de un mensaje se sigue esperando.
// Se corta si cae la conexión, y como mucho a los 60 s.
static void wait_for_queue()
{
    const int64_t limit = esp_timer_get_time() + 60 * 1000000LL;
    while (mqtt_connected && esp_timer_get_time() < limit &&
           (handling || esp_timer_get_time() - last_event_us < CONFIG_QUEUE_WINDOW_MS * 1000LL)) {
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
             R"({"firmware":"%s","despertar":"%s","seleccion":%d,"imagenes":[%s],"rssi":%d,"bateria":%d,"bateria_mv":%d,)"
             R"("psram":%u,"hora":"%s","proxima":"%s","despierto_ms":%lld})",
             esp_app_get_description()->version, wakeup_cause, sel ? static_cast<int>(*sel) : -1,
             images.c_str(), rssi, rtc.battery, rtc.battery_mv, static_cast<unsigned>(psram_size()),
             clock_valid() ? format_time(time(nullptr), "%Y-%m-%dT%H:%M:%S").c_str() : "",
             clock_valid() ? format_time(rtc.t_next_check, "%H:%M:%S").c_str() : "", esp_timer_get_time() / 1000);
    xEventGroupClearBits(events, BIT_PUBLISHED);
    if (esp_mqtt_client_publish(client, CONFIG_MQTT_STATUS_TOPIC, json, 0, 1, 1) < 0 ||
        !(xEventGroupWaitBits(events, BIT_PUBLISHED, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000)) & BIT_PUBLISHED)) {
        ESP_LOGW(TAG, "No se pudo publicar el estado");
    }
}

// Conexión de este despertar: se abre la primera vez que hace falta y se
// mantiene hasta dormir. Si no hubo WiFi se reintenta en la siguiente
// sincronización (botón de refresco; en modo pruebas, cada minuto). Si hubo
// WiFi, esp-mqtt sigue reintentando el broker por su cuenta.
static esp_mqtt_client_handle_t mqtt_client;
static bool attempted;

// Suscrito y recibiendo ahora mismo
static bool mqtt_ready()
{
    return mqtt_connected && subscription_acked && !subscription_rejected;
}

// Estado de la red ahora mismo. "ok" = suscrito al broker: lo que importa es
// poder recibir imágenes, no sólo tener WiFi.
static net_state current_network()
{
    if (!wifi_connected()) {
        return net_state::no_wifi;
    }
    return mqtt_ready() ? net_state::ok : net_state::no_broker;
}

static bool lfs_mounted;
static void redraw_status();  // más abajo: necesita quick_update()

static void sync_mqtt()
{
    rtc.t_next_check = next_check_after_now();  // por si algo falla antes de terminar
    if (mqtt_client == nullptr) {
        attempted = true;
        bool subscribed;
        mqtt_client = connect_network(subscribed);
        if (mqtt_ready() && !NO_DEEP_SLEEP && SHOW_NETWORK) {
            // Conectado: el icono pierde la luna mientras dure la conexión
            online = true;
            rtc.network = net_state::ok;
            redraw_status();
        }
    }
    online = online || mqtt_ready();
    if (mqtt_ready()) {
        wait_for_queue();
    }
    // SNTP se pidió al conectar, en paralelo con MQTT: si responde, ya lo ha
    // hecho. Esperar más sólo alarga el despertar con la radio encendida; sin
    // respuesta se sigue con el RTC, que en un intervalo se desvía segundos, y
    // la siguiente conexión lo corrige.
    if (sntp_started && esp_netif_sntp_sync_wait(pdMS_TO_TICKS(200)) != ESP_OK) {
        ESP_LOGI(TAG, "SNTP (%s) sin respuesta aún: se sigue con la hora del RTC", CONFIG_NTP_SERVER);
    }
    // Con la hora ya corregida por SNTP
    rtc.t_next_check = next_check_after_now();
    if (clock_valid()) {
        ESP_LOGI(TAG, "Hora %s; próxima comprobación %s", format_time(time(nullptr), "%Y-%m-%d %H:%M:%S").c_str(),
                 format_time(rtc.t_next_check, "%H:%M:%S").c_str());
    }
    if (mqtt_ready()) {
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

// Pulsaciones pendientes. Las cuenta button_task, que sigue leyendo los
// botones mientras app_main pinta o se conecta: dos "siguiente" seguidos
// avanzan dos imágenes aunque el panel aún esté refrescando.
static constexpr EventBits_t BIT_PRESS = BIT3;
static std::atomic<int> nav_pending{0};  // +1 siguiente, -1 anterior
static std::atomic<bool> refresh_pending{false};

static void register_press(button b)
{
    if (b == button::next) {
        ++nav_pending;
    } else if (b == button::previous) {
        --nav_pending;
    } else if (b == button::refresh) {
        refresh_pending = true;
    }
    xEventGroupSetBits(events, BIT_PRESS);
}

static bool press_pending()
{
    return nav_pending != 0 || refresh_pending;
}

// Lee los botones cada 10 ms; una pulsación cuenta al estar 30 ms abajo y no
// se repite hasta soltar (30 ms arriba). Un botón que ya está pulsado al
// arrancar (el que ha despertado al ESP32) no cuenta: lo apunta app_main.
static void button_task(void *)
{
    static constexpr gpio_num_t pins[] = {GPIO_PREV, GPIO_NEXT, GPIO_REFRESH};
    static constexpr button buttons[] = {button::previous, button::next, button::refresh};
    static constexpr int STABLE = 3;
    bool down[3];
    int count[3] = {};
    for (int i = 0; i < 3; ++i) {
        down[i] = gpio_get_level(pins[i]) == 0;
    }
    while (true) {
        for (int i = 0; i < 3; ++i) {
            const bool low = gpio_get_level(pins[i]) == 0;
            count[i] = low != down[i] ? count[i] + 1 : 0;
            if (count[i] >= STABLE) {
                down[i] = low;
                count[i] = 0;
                if (low) {
                    register_press(buttons[i]);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Espera hasta `ms` a que haya alguna pulsación pendiente. El bit se limpia
// antes de mirar: si no, el de una pulsación ya atendida cortaría la espera.
static bool wait_for_press(int64_t ms)
{
    xEventGroupClearBits(events, BIT_PRESS);
    if (!press_pending() && ms > 0) {
        xEventGroupWaitBits(events, BIT_PRESS, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
    }
    return press_pending();
}

// Espera a que se suelten todos los botones; false si alguno sigue pulsado.
[[maybe_unused]] static bool wait_for_release()
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
    ESP_LOGI(TAG, "Despierto %lld ms; a deep sleep %lld s; pila libre (mínimo) %u bytes", esp_timer_get_time() / 1000,
             (long long)sleep_s, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleep_s) * 1000000ULL);
    if (wake_on_buttons) {
        esp_sleep_enable_ext1_wakeup_io(BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    esp_deep_sleep_start();
}

// Lo que hay en el cristal, en RAM mientras dura la tanda de pintados.
// frame.bin (y rtc.magic) sólo se ponen al día en end_painting(): escribir
// 48 KB con fsync en cada pulsación haría lenta la respuesta.
static std::optional<idfxx::epaper::mono_framebuffer> glass;
static bool glass_unsaved;

// Lo que hay en el cristal, si se sabe; si no, nullptr. Con mtx tomado.
static const idfxx::epaper::mono_framebuffer *glass_frame(bool lfs)
{
    if (!glass && rtc.magic == RTC_MAGIC && lfs) {
        glass.emplace(WIDTH, HEIGHT);
        if (!load_frame(FRAME_PATH, *glass)) {
            glass.reset();
        }
    }
    return glass ? &*glass : nullptr;
}

// Pinta `frame` y lo apunta como lo que hay en el cristal. Con `previous` (lo
// que hay ahora) refresco parcial; sin él, completo. `count_partial`: cuenta
// para el límite de parciales seguidos (no los retoques pequeños de los
// indicadores, que apenas dejan restos). Con mtx tomado.
static void commit(const idfxx::epaper::mono_framebuffer &frame, const idfxx::epaper::mono_framebuffer *previous,
                   bool count_partial)
{
    rtc.magic = 0;  // frame.bin ya no coincide con el cristal (y mientras se pinta, éste es incierto)
    std::optional<idfxx::epaper::mono_framebuffer> before;
    if (previous != nullptr) {
        before = *previous;  // `previous` apunta a glass, que se invalida aquí
    }
    glass.reset();  // por si paint() falla: no se sabe qué ha quedado
    paint(frame, before ? &*before : nullptr);
    if (!before) {
        rtc.partials = 0;
        rtc.t_full = time(nullptr);
    } else if (count_partial) {
        rtc.partials++;
    }
    glass = frame;
    glass_unsaved = true;
}

// Fin de una tanda de pintados: duerme el panel y guarda el cristal en frame.bin.
static void end_painting(bool lfs)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        if (panel) {
            panel->display.sleep();
            panel.reset();
        }
        if (glass_unsaved && glass && lfs && save_frame(FRAME_PATH, *glass)) {
            rtc.magic = RTC_MAGIC;
            glass_unsaved = false;
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error al dormir el panel: %s", e.what());
        panel.reset();
    }
    xSemaphoreGive(mtx);
}

// Retoca sólo los indicadores sobre lo que ya hay en el cristal, con un
// refresco parcial: respuesta inmediata a un botón antes del trabajo lento.
// Sin saber qué hay en el cristal no se hace nada (lo pintará show()).
template <typename Draw>
static void quick_update(bool lfs, Draw draw)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        const auto *previous = glass_frame(lfs);
        if (previous != nullptr && CONFIG_INDICATOR_HEIGHT > 0) {
            idfxx::epaper::mono_framebuffer frame = *previous;
            draw(frame);
            if (!std::ranges::equal(previous->data(), frame.data())) {
                commit(frame, previous, false);
            }
        }
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error: %s", e.what());
        panel.reset();
    }
    xSemaphoreGive(mtx);
}

// Repinta sólo el recuadro de WiFi y batería con el estado actual
static void redraw_status()
{
    quick_update(lfs_mounted, [](idfxx::epaper::mono_framebuffer &frame) { draw_status(frame); });
}

// Pinta la imagen seleccionada si no es lo que ya hay en el cristal. Si no hay
// ninguna guardada pinta `aviso` (si lo hay). `forzar` = refresco completo
// aunque no haya cambios (limpia los restos de los parciales).
// `browsing`: pintado de una tanda de botones. El refresco completo que toque
// por MAX_PARTIALS o FULL_REFRESH_H (2-3 s con parpadeo) no se hace a mitad de
// la navegación: se aplaza al siguiente pintado normal (la próxima comprobación
// por tiempo), salvo que se acumulen el doble de parciales.
static void show(bool lfs, const char *notice, bool force, bool browsing = false)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    try {
        const auto *previous = glass_frame(lfs);  // lo que hay en el cristal, si se sabe

        idfxx::epaper::mono_framebuffer frame(WIDTH, HEIGHT);
        std::optional<unsigned> sel;
        bool has_image = false;
        // Como mucho una vuelta: si una corrupta no se puede ni borrar, selection()
        // la devolvería siempre y el bucle no acabaría (con mtx tomado)
        for (unsigned tries = 0; lfs && !has_image && tries < MAX_IMAGES && (sel = selection()); ++tries) {
            has_image = load_frame(image_path(*sel).c_str(), frame);
            if (!has_image) {
                // Corrupta: se borra y se pasa a la siguiente. Sin sub.txt, la
                // próxima conexión hace una sincronización completa y la recupera
                // del broker si sigue retenida.
                remove(SUBSCRIPTION_PATH);
                if (remove(image_path(*sel).c_str()) != 0) {
                    ESP_LOGE(TAG, "No se puede borrar la imagen %u corrupta", *sel);
                    break;
                }
            }
        }
        if (attempted) {
            rtc.network = current_network();  // con conexión abierta en este despertar, su estado real
        }
        if (!has_image) {
            if (notice == nullptr) {
                xSemaphoreGive(mtx);
                return;
            }
            draw_message(frame, notice);  // limpia la pantalla: antes que los indicadores
        }
        draw_indicators(frame, has_image ? sel : std::nullopt);
        const time_t now = time(nullptr);
        const bool full_due = rtc.partials >= CONFIG_MAX_PARTIALS || now - rtc.t_full >= CONFIG_FULL_REFRESH_H * 3600;
        const bool defer_full = browsing && rtc.partials < 2 * CONFIG_MAX_PARTIALS;
        // Sin cambios no se repinta, salvo un completo pendiente fuera de la
        // navegación: limpia los restos de los parciales aunque nada cambie
        if (!force && previous && std::ranges::equal(previous->data(), frame.data()) && (!full_due || browsing)) {
            ESP_LOGI(TAG, "Sin cambios en la imagen, no se refresca");
            xSemaphoreGive(mtx);
            return;
        }

        const bool partial = !force && previous && (!full_due || defer_full);
        if (has_image) {
            ESP_LOGI(TAG, "Imagen %u, refresco %s (parciales=%" PRIu32 ")", *sel, partial ? "parcial" : "completo",
                     rtc.partials);
        }

        commit(frame, partial ? previous : nullptr, true);
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Error: %s", e.what());
        panel.reset();  // el próximo pintado empieza de cero (y será completo: glass se borró)
    }
    xSemaphoreGive(mtx);
}

static const char *notice()
{
    static const std::string offline_text = "Sin conexion.\nReintentando cada " + std::to_string(CONFIG_INTERVAL_S) + " s";
    return mqtt_ready() ? "Esperando imagenes en\n" CONFIG_MQTT_IMAGE_TOPIC "/<n>" : offline_text.c_str();
}

// Anterior/siguiente: cada tanda de pulsaciones mueve la selección y repinta
// sólo "k/N" (rápido); cuando pasan NAV_SETTLE_MS sin pulsar, se pinta la
// imagen. Sin red.
static void handle_navigation(bool lfs)
{
    bool moved = false;
    while (true) {
        const int steps = nav_pending.exchange(0);
        if (steps != 0) {
            if (navigate_by(steps)) {
                moved = true;
                if (SHOW_POSITION) {
                    quick_update(lfs, [](idfxx::epaper::mono_framebuffer &frame) {
                        if (const auto sel = selection()) {
                            draw_position(frame, *sel, true);
                        }
                    });
                }
            }
            continue;  // las pulsaciones durante el pintado ya están contadas
        }
        if (refresh_pending || !wait_for_press(CONFIG_NAV_SETTLE_MS) || refresh_pending) {
            break;
        }
    }
    if (moved) {
        show(lfs, nullptr, false, true);
    }
}

// Refresco: el icono de WiFi pasa a "…" al momento; luego se conecta, recoge la
// cola MQTT y repinta entero. Las pulsaciones repetidas mientras tanto cuentan
// como una.
static void handle_refresh(bool lfs)
{
    ESP_LOGI(TAG, "Botón de refresco");
    if (SHOW_NETWORK) {
        // show() lo sustituye luego por el estado real (sync_mqtt deja attempted)
        rtc.network = net_state::syncing;
        quick_update(lfs, [](idfxx::epaper::mono_framebuffer &frame) { draw_status(frame); });
    }
    end_painting(lfs);  // el panel no se queda encendido mientras se conecta
    sync_mqtt();
    show(lfs, notice(), true);
    refresh_pending = false;
}

// Atiende pulsaciones hasta que pasen `window_ms` sin ninguna
static void handle_buttons(bool lfs, int64_t window_ms)
{
    while (true) {
        if (refresh_pending) {
            handle_refresh(lfs);
        } else if (nav_pending != 0) {
            handle_navigation(lfs);
        } else if (!wait_for_press(window_ms)) {
            return;
        }
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
    xTaskCreate(button_task, "botones", 2048, nullptr, 5, nullptr);

    setenv("TZ", CONFIG_TIMEZONE, 1);
    tzset();
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        rtc = {};  // arranque en frío, flasheo, crash...: no sabemos qué hay en el cristal
    }
    const bool lfs = mount_littlefs();
    lfs_mounted = lfs;
    update_battery();  // antes de encender la WiFi: sin la caída de tensión al transmitir

    // Si despierta un botón de navegación no se toca la red; la comprobación
    // de MQTT sigue programada para rtc.t_next_check (ver dormir()).
    const button b = wakeup_button();
    if (b != button::none) {
        wakeup_cause = "boton";
        register_press(b);
    } else {
        if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            wakeup_cause = "temporizador";
        }
        sync_mqtt();
        show(lfs, notice(), false);
    }

#if CONFIG_NO_DEEP_SLEEP
    // Modo pruebas: seguir conectado, pintar cada imagen según llega y atender los botones
    ESP_LOGW(TAG, "Modo pruebas: sin deep sleep");
    while (true) {
        handle_buttons(lfs, 0);
        end_painting(lfs);
        // Hasta otra pulsación, una imagen nueva o un minuto (para seguir la batería)
        xEventGroupClearBits(events, BIT_PRESS);
        if (!press_pending()) {
            xEventGroupWaitBits(events, BIT_PRESS | BIT_CHANGED, pdTRUE, pdFALSE, pdMS_TO_TICKS(60000));
        }
        if (!press_pending()) {
            if (mqtt_client == nullptr) {
                sync_mqtt();  // no hubo WiFi: reintentar
            }
            wait_for_queue();
            update_battery();
            show(lfs, notice(), false);
            end_painting(lfs);
        }
    }
#else
    // Si ha despertado un botón, unos segundos para seguir pulsando sin volver
    // a despertar. Tras un despertar por tiempo no hay ventana (sería radio
    // encendida esperando una pulsación que casi nunca llega), pero se atiende
    // lo que se haya pulsado mientras tanto.
    handle_buttons(lfs, b != button::none ? CONFIG_BUTTON_WINDOW_MS : 0);
    // Si la comprobación por tiempo vence ya (o en unos segundos), se hace en
    // este despertar en vez de dormir 1 s y arrancar otra vez sólo para ella
    if (!attempted && time(nullptr) + 5 >= rtc.t_next_check) {
        wakeup_cause = "temporizador";
        sync_mqtt();
        show(lfs, notice(), false);
        handle_buttons(lfs, 0);  // lo pulsado mientras tanto
    }
    if (online) {
        // Se desconecta antes de dormir: el icono recupera la luna
        disconnect_network(mqtt_client);
        mqtt_client = nullptr;
        online = false;
        if (SHOW_NETWORK && rtc.network == net_state::ok) {
            redraw_status();
        }
    }
    end_painting(lfs);
    const bool released = wait_for_release();
    disconnect_network(mqtt_client);
    go_to_sleep(released);
#endif
}
