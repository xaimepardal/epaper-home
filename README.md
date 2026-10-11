# epaper-home

Firmware para el **Seeed Studio reTerminal E1001** (ESP32-S3 + ePaper monocromo de 7,5" y 800×480) que muestra en la pantalla **texto o imágenes recibidos por MQTT**. Está pensado para funcionar con batería: el dispositivo pasa la mayor parte del tiempo en deep sleep y, al actualizar, usa refresco parcial (sin parpadeo).

## Funciones

- **Texto por MQTT** (`reterminal/texto`). Ajusta las líneas por palabras, respeta los `\n` y usa la fuente Spleen 8×16 a escala ×3 (32 columnas × 9 filas).
- **Imágenes por MQTT** (`reterminal/imagen`). Recibe imágenes de 800×480 a 1 bit comprimidas con zlib; el ESP32 las descomprime con el *inflate* de su ROM y descarta las que lleguen corruptas. Formato en [Formato de las imágenes](#formato-de-las-imágenes).
- **Prioridad de mensajes**. La pantalla muestra lo último que llegue, sea texto o imagen.
- **Refresco parcial incluso tras deep sleep**. El último frame se guarda en LittleFS y, al despertar, se carga como imagen de referencia del controlador mediante un [parche del driver](#parche-del-driver-uc8179).
- **Refresco completo cuando conviene**. Se hace uno tras cualquier reinicio que no sea un deep sleep, cada 10 refrescos parciales, cada 24 h o si el frame guardado no es fiable; así se limpian los restos que deja el parcial.
- **Sin refrescos innecesarios**. Si la imagen resultante es idéntica a la que ya hay en pantalla, no se refresca.
- **Modo batería**. Despierta cada 5 min, se conecta a la WiFi y a MQTT con sesión persistente, recoge el último mensaje (retenido o en cola QoS 1), pinta si hace falta y vuelve a dormir.
- **Modo pruebas** (opcional). No usa deep sleep: se queda conectado y pinta cada mensaje en cuanto llega.
- **Pantallas de estado**. «Esperando mensajes» o «Sin conexión» cuando aún no hay nada que mostrar.
- **Volcado de fallos** en la partición `coredump`, que se lee con `idf.py coredump-info`.
- **Herramienta de PC** [`tools/enviar_imagen.py`](tools/enviar_imagen.py). Convierte cualquier imagen (PNG, JPG…) al formato del panel, con tramado o umbral, y la publica.

## Hardware

| | |
|---|---|
| Dispositivo | [reTerminal E1001](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) — [wiki](https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/) · [serie reTerminal E](https://wiki.seeedstudio.com/reterminal_e10xx_main_page/) · [datasheet](https://files.seeedstudio.com/Bazaar/product_pdf/100073581.pdf) |
| SoC | ESP32-S3, 32 MB de flash |
| Pantalla | ePaper 7,5" monocromo, 800×480, controlador UC8179 (panel GDEY075T7) |
| Conexión al PC | USB-C (conversor USB-serie: aparece como `/dev/ttyUSB0` en Linux) |

Pines que usa el firmware: SPI de la pantalla con MOSI en 9, SCLK en 7, CS en 10, DC en 11, RESET en 12 y BUSY en 13. La microSD comparte el bus, así que se desactiva con SD_EN en 16 y SD_CS en 14.

## Requisitos para compilar y flashear

| Qué | Versión | Enlace |
|---|---|---|
| ESP-IDF | **v6.1** (rama `release/v6.1`) | [GitHub](https://github.com/espressif/esp-idf) · [Guía de instalación (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) |
| Git, Python 3, CMake, Ninja | los que pide ESP-IDF | [Requisitos en Linux/macOS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/linux-macos-setup.html) |
| Conexión a Internet | — | La primera compilación descarga los componentes del [registro de Espressif](https://components.espressif.com) |
| Broker MQTT | cualquiera | Por defecto el público `test.mosquitto.org`; para uso real, uno propio, como [Mosquitto](https://mosquitto.org/) |

Para la herramienta del PC también hacen falta **Pillow** y **mosquitto_pub** (Arch/Manjaro: `sudo pacman -S python-pillow mosquitto`; Fedora: `sudo dnf install python3-pillow mosquitto`; Debian/Ubuntu: `sudo apt install python3-pil mosquitto-clients`).

### Componentes que se descargan solos

Los declara [`main/idf_component.yml`](main/idf_component.yml) y ESP-IDF los descarga al compilar:

- [`cleishm/idfxx`](https://github.com/cleishm/idfxx): wrappers C++ para ESP-IDF (SPI, gráficos, fuentes y [driver UC8179](https://components.espressif.com/components/cleishm/idfxx_epaper_uc8179)). La fuente es [Spleen](https://github.com/fcambus/spleen).
- [`espressif/mqtt`](https://components.espressif.com/components/espressif/mqtt): cliente MQTT.
- [`joltwallet/littlefs`](https://components.espressif.com/components/joltwallet/littlefs): sistema de ficheros para guardar el último frame.
- `protocol_examples_common` (incluido en ESP-IDF): conexión WiFi.

## Instalación

### 1. Instalar ESP-IDF

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b release/v6.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

En cada terminal nueva hay que cargar el entorno:

```bash
. ~/esp/esp-idf/export.sh
```

> Si mueves la carpeta de ESP-IDF después de instalarlo, `export.sh` deja de funcionar; vuelve a ejecutar `./install.sh esp32s3` desde la nueva ruta.

### 2. Descargar el proyecto

```bash
git clone https://github.com/xaimepardal/epaper-home.git
cd epaper-home
```

### 3. Configurar

```bash
idf.py menuconfig
```

- **Example Connection Configuration**: SSID y contraseña de la WiFi (solo 2,4 GHz).
- **ePaper MQTT**:

| Opción | Por defecto | |
|---|---|---|
| URI del broker MQTT | `mqtt://test.mosquitto.org` | p. ej. `mqtt://usuario:clave@192.168.1.10` |
| Topic para texto | `reterminal/texto` | |
| Topic para imágenes | `reterminal/imagen` | |
| Modo pruebas: no dormir nunca | desactivado | solo para desarrollo |
| Segundos en deep sleep | 300 | cada cuánto comprueba si hay mensajes |
| Ms esperando mensajes | 2000 | tras suscribirse |
| Segundos máximos para conectar | 20 | |
| Parciales antes de un completo | 10 | |
| Horas máximas sin completo | 24 | |

> ⚠️ La configuración se guarda en `sdkconfig`, **que contiene la contraseña WiFi**. Está en `.gitignore`: no lo subas al repositorio.

### 4. Compilar y flashear

Conecta el reTerminal por USB-C y ejecuta:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

- **Sobre `-p`**: `flash` compila antes si hace falta; para ver el puerto de tu placa, usa `ls /dev/ttyUSB* /dev/ttyACM*`.
- **Salir del monitor**: <kbd>Ctrl</kbd>+<kbd>]</kbd>.
- **Error `Permission denied`** al abrir el puerto: añade tu usuario al grupo del puerto serie (`uucp` en Arch/Manjaro, `dialout` en Debian/Ubuntu/Fedora) y vuelve a iniciar sesión.
- **Primera vez**: si la placa tiene el firmware de fábrica, conviene borrar antes la flash con `idf.py -p /dev/ttyUSB0 erase-flash flash monitor`. Esto **elimina el firmware original de Seeed**.

## Uso

**Enviar texto:**

```bash
mosquitto_pub -h test.mosquitto.org -t reterminal/texto -r -q 1 -m "Hola desde MQTT"
```

**Enviar una imagen:**

```bash
tools/enviar_imagen.py foto.jpg                    # tramado (fotos)
tools/enviar_imagen.py logo.png --umbral 128       # blanco/negro puro (texto, gráficos)
tools/enviar_imagen.py foto.jpg --previa prev.png  # solo previsualizar, sin publicar
```

**Dejar la pantalla en blanco:**

```bash
python3 -c "import zlib,sys; sys.stdout.buffer.write(zlib.compress(b'\xff'*48000))" | mosquitto_pub -h test.mosquitto.org -t reterminal/imagen -r -q 1 -s
```

**Publicar siempre con `-r` (retenido) y `-q 1`.** El dispositivo pasa casi todo el tiempo dormido, y así recibe el mensaje en su siguiente despertar (como mucho 5 min después en modo batería). Si hay a la vez un texto y una imagen retenidos, ambos llegan en cada despertar; para evitarlo, `enviar_imagen.py` borra el texto retenido al publicar. Para borrar a mano un retenido, envía un mensaje vacío: `mosquitto_pub -h test.mosquitto.org -t reterminal/imagen -r -n`.

## Formato de las imágenes

| | |
|---|---|
| Topic | `reterminal/imagen` |
| Payload | zlib de 48 000 bytes, máximo 64 KB comprimido |
| Píxeles | 800×480, 1 bit por píxel, filas de arriba abajo, el bit más significativo es el píxel izquierdo, **1 = blanco** |

Es exactamente lo que produce Pillow:

```python
import zlib
from PIL import Image
payload = zlib.compress(Image.open("imagen.png").resize((800, 480)).convert("1").tobytes(), 9)
```

## Estructura del proyecto

```
main/epaper_mqtt.cpp      Firmware: MQTT, dibujo, refresco y deep sleep
main/Kconfig.projbuild    Opciones de menuconfig "ePaper MQTT"
main/idf_component.yml    Dependencias del registro de componentes
components/idfxx_epaper*  Copias parcheadas del driver ePaper (ver abajo)
partitions.csv            Tabla de particiones para los 32 MB de flash
tools/enviar_imagen.py    Conversión y publicación de imágenes desde el PC
```

### Tabla de particiones

| Partición | Tamaño | Uso |
|---|---|---|
| `nvs` | 24 KB | Credenciales y calibración WiFi |
| `otadata` + `ota_0` + `ota_1` | 2 × 6 MB | Firmware (preparado para actualizaciones OTA) |
| `coredump` | 64 KB | Volcado de fallos |
| `storage` | ~19,8 MB | LittleFS: último frame pintado |

### Parche del driver UC8179

`components/idfxx_epaper` y `components/idfxx_epaper_uc8179` son copias de [`cleishm/idfxx`](https://github.com/cleishm/idfxx) v1.0.0 (Apache-2.0) con estos cambios, marcados en el código como `[epaper_mqtt patch]`:

- `panel::restore_baseline()`: tras despertar el panel, escribe el frame que hay en el cristal en la RAM de «imagen anterior» del UC8179 (comando `0x10`), de modo que el siguiente refresco puede ser parcial. Sin esto, cada despertar obligaría a un refresco completo con parpadeo.
- `mono_framebuffer::data()` modificable, para cargar un frame guardado.

Más detalles en [`components/idfxx_epaper/PATCH.md`](components/idfxx_epaper/PATCH.md). Si se actualiza la librería, hay que volver a aplicar el parche.

## Referencias

- [Wiki del reTerminal E1001 (Seeed Studio)](https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/)
- [Documentación de ESP-IDF para ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
- [Registro de componentes de Espressif](https://components.espressif.com)
- [idfxx: C++ para ESP-IDF](https://github.com/cleishm/idfxx)
- [Mosquitto](https://mosquitto.org/)
