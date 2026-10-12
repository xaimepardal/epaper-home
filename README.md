# epaper-home

Firmware para el **Seeed Studio reTerminal E1001** (ESP32-S3 + ePaper monocromo de 7,5" y 800×480) que funciona como **galería de imágenes recibidas por MQTT**, con los botones para pasar de una a otra. Está pensado para funcionar con batería: el dispositivo pasa la mayor parte del tiempo en deep sleep y, al actualizar, usa refresco parcial (sin parpadeo).

## Funciones

- **Imágenes por MQTT** (`reterminal/image`). Cada imagen llega con un **índice** (0 a 9 por defecto) y se guarda en ese hueco de LittleFS, sustituyendo a la anterior. Son imágenes de 800×480 a 1 bit comprimidas con zlib; el ESP32 las descomprime con el *inflate* de su ROM y descarta las que lleguen corruptas. Un mensaje con solo el índice borra esa imagen. Formato en [Formato de las imágenes](#formato-de-las-imágenes).
- **Cola MQTT**. Las imágenes se publican con QoS 1 y sin retener; el broker las guarda en la cola de la sesión persistente y las entrega todas en el siguiente despertar.
- **Imagen seleccionada**. La pantalla muestra siempre la imagen del índice seleccionado, que se guarda en LittleFS y sobrevive a reinicios. Al llegar imágenes nuevas no se cambia de índice; solo se repinta si llega una nueva versión de la que se está viendo. Si se borra la seleccionada, pasa a la siguiente que exista.
- **Botones físicos**. *Anterior* (izquierdo, GPIO5) y *siguiente* (central, GPIO4) recorren las imágenes guardadas, saltando los huecos vacíos y dando la vuelta, sin conectarse a la WiFi. *Refresco* (verde, GPIO3) descarga la cola de MQTT y hace un refresco completo. Los tres despiertan el dispositivo del deep sleep, y tras cada pulsación sigue 3 s despierto por si se pulsa otra vez. Los GPIO se cambian en `menuconfig`.
- **Refresco parcial incluso tras deep sleep**. El último frame se guarda en LittleFS y, al despertar, se carga como imagen de referencia del controlador mediante un [parche del driver](#parche-del-driver-uc8179).
- **Refresco completo cuando conviene**. Se hace uno tras cualquier reinicio que no sea un deep sleep, cada 10 refrescos parciales, cada 24 h o si el frame guardado no es fiable; así se limpian los restos que deja el parcial.
- **Sin refrescos innecesarios**. Si la imagen resultante es idéntica a la que ya hay en pantalla, no se refresca.
- **MQTT con TLS y usuario**. Admite `mqtts://` con la CA propia del broker incrustada en el firmware ([`main/certs/`](main/certs/README.md)) o con el bundle de CAs públicas, y usuario y contraseña. Los fallos de TLS y de autenticación se explican en el log.
- **Modo batería**. Despierta cada hora (configurable) o al pulsar un botón. Por tiempo, se conecta a la WiFi y a MQTT con sesión persistente, guarda las imágenes en cola, pinta si hace falta y vuelve a dormir. Pasar de imagen con los botones no adelanta ni retrasa la siguiente comprobación.
- **Modo pruebas** (opcional). No usa deep sleep: se queda conectado, guarda cada imagen en cuanto llega y atiende los botones al momento.
- **Pantallas de estado**. «Esperando imágenes» o «Sin conexión» cuando aún no hay ninguna imagen guardada.
- **Volcado de fallos** en la partición `coredump`, que se lee con `idf.py coredump-info`.
- **Herramienta de PC** [`tools/enviar_imagen.py`](tools/enviar_imagen.py). Convierte cualquier imagen (PNG, JPG…) al formato del panel, con tramado o umbral, y la publica en el índice indicado; también borra índices.

## Hardware

| | |
|---|---|
| Dispositivo | [reTerminal E1001](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) — [wiki](https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/) · [serie reTerminal E](https://wiki.seeedstudio.com/reterminal_e10xx_main_page/) · [datasheet](https://files.seeedstudio.com/Bazaar/product_pdf/100073581.pdf) |
| SoC | ESP32-S3, 32 MB de flash |
| Pantalla | ePaper 7,5" monocromo, 800×480, controlador UC8179 (panel GDEY075T7) |
| Conexión al PC | USB-C (conversor USB-serie: aparece como `/dev/ttyUSB0` en Linux) |

Pines que usa el firmware: SPI de la pantalla con MOSI en 9, SCLK en 7, CS en 10, DC en 11, RESET en 12 y BUSY en 13. La microSD comparte el bus, así que se desactiva con SD_EN en 16 y SD_CS en 14. Botones (activos a nivel bajo, con pull-up en la placa): KEY0 verde en 3, KEY1 central en 4 y KEY2 izquierdo en 5, según la [wiki de periféricos](https://wiki.seeedstudio.com/reterminal_e10xx_with_arduino_peripherals/).

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
| URI del broker MQTT | `mqtt://test.mosquitto.org` | `mqtt://host:1883` sin cifrar o `mqtts://host:8883` con TLS ([ver abajo](#broker-con-tls-y-ca-propia)) |
| Usuario MQTT | vacío (anónimo) | |
| Contraseña MQTT | vacío | se guarda en `sdkconfig` |
| Topic para imágenes | `reterminal/image` | |
| Número de imágenes | 10 | índices 0 a N−1, hasta 255; 48 KB cada una |
| GPIO anterior / siguiente / refresco | 5 / 4 / 3 | si «anterior» y «siguiente» van al revés, intercambia 5 y 4 |
| Ms despierto esperando otra pulsación | 3000 | |
| Modo pruebas: no dormir nunca | desactivado | solo para desarrollo |
| Segundos en deep sleep | 3600 (1 h) | cada cuánto comprueba si hay imágenes nuevas |
| Ms esperando mensajes | 2000 | sin mensajes nuevos para dar la cola por vaciada |
| Segundos máximos para conectar | 20 | |
| Parciales antes de un completo | 10 | |
| Horas máximas sin completo | 24 | |

> ⚠️ La configuración se guarda en `sdkconfig`, **que contiene las contraseñas de la WiFi y de MQTT**. Está en `.gitignore`: no lo subas al repositorio.

### Broker con TLS y CA propia

Por ejemplo, el Mosquitto de un NAS con certificado firmado por una CA propia:

1. Copia el certificado de la CA (solo `ca.crt`, nunca la clave) en **`main/certs/mqtt_ca.crt`**. Está en `.gitignore`. Al compilar debe aparecer `MQTT: CA propia …`.
2. En `menuconfig` → *ePaper MQTT*:
   - **URI:** `mqtts://<host>:8883`. El host tiene que figurar en el certificado del broker (CN o SAN). Puede ser una IP si el SAN la incluye; es lo más fiable, porque no depende del DNS de la red.
   - **Usuario y contraseña:** los de un usuario que pueda **leer** `reterminal/#`.
3. Compila y flashea. En el monitor debe salir `Conectado al broker`.

Si falla, el log dice por qué:

| Mensaje | Causa probable |
|---|---|
| `Error de transporte: esp-tls 0x…, mbedtls -0x2700` | El certificado no valida: falta la CA correcta en `main/certs/` o el host del URI no está en el certificado. |
| `Error de transporte: … errno 113/111` | No llega al broker: IP o puerto incorrectos, o un cortafuegos. |
| `El broker rechaza la conexión: código 0x4` / `0x5` | Usuario o contraseña incorrectos / sin permiso. |

Sin `main/certs/mqtt_ca.crt`, `mqtts://` valida con el bundle de CAs públicas de ESP-IDF; sirve para brokers con certificado de Let's Encrypt.

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

**Enviar una imagen** (el primer argumento es el índice):

```bash
tools/enviar_imagen.py 0 foto.jpg                    # tramado (fotos)
tools/enviar_imagen.py 1 foto.jpg --host 192.168.1.50 --ca main/certs/mqtt_ca.crt --usuario admin   # broker con TLS
tools/enviar_imagen.py 2 logo.png --umbral 128       # blanco/negro puro (texto, gráficos)
tools/enviar_imagen.py 0 foto.jpg --previa prev.png  # solo previsualizar, sin publicar
```

**Borrar una imagen:**

```bash
tools/enviar_imagen.py 2 --borrar
```

**Imagen en blanco en el índice 0 sin la herramienta:**

```bash
python3 -c "import zlib,sys; sys.stdout.buffer.write(b'\x00' + zlib.compress(b'\xff'*48000))" | mosquitto_pub -h test.mosquitto.org -t reterminal/image -q 1 -s
```

Con un broker con TLS y usuario, añade a `mosquitto_pub` las opciones `-p 8883 --cafile main/certs/mqtt_ca.crt -u <usuario> -P <contraseña>`. El usuario que publica necesita permiso de **escritura**; el del reTerminal basta con que pueda leer.

**Publicar con `-q 1` y sin `-r`.** El dispositivo pasa casi todo el tiempo dormido, pero se conecta siempre con el mismo *client id* y sesión persistente, así que el broker le guarda en cola todo lo publicado con QoS 1 y se lo entrega en el siguiente despertar (como mucho 1 h después, o al pulsar *refresco*). Para que la cola funcione, el reTerminal tiene que haberse conectado al broker al menos una vez. Si en el broker quedó un mensaje retenido de versiones anteriores, bórralo con `mosquitto_pub -t reterminal/imagen -r -n`.

**Botones:** *anterior* y *siguiente* cambian de imagen en un par de segundos (refresco parcial, sin red); *refresco* conecta, descarga lo pendiente y hace un refresco completo.

## Formato de las imágenes

| | |
|---|---|
| Topic | `reterminal/image`, QoS 1, sin retener |
| Payload | 1 byte con el **índice** (0 a N−1) + zlib de 48 000 bytes, máximo 64 KB comprimido. Solo el byte del índice borra esa imagen |
| Píxeles | 800×480, 1 bit por píxel, filas de arriba abajo, el bit más significativo es el píxel izquierdo, **1 = blanco** |

Es exactamente lo que produce Pillow:

```python
import zlib
from PIL import Image
indice = 0
payload = bytes([indice]) + zlib.compress(Image.open("imagen.png").resize((800, 480)).convert("1").tobytes(), 9)
```

## Estructura del proyecto

```
main/epaper_mqtt.cpp      Firmware: MQTT, galería, botones, refresco y deep sleep
main/Kconfig.projbuild    Opciones de menuconfig "ePaper MQTT"
main/certs/               CA propia del broker MQTT (opcional, no versionada)
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
| `storage` | ~19,8 MB | LittleFS: imágenes por índice (`imgNNN.bin`), índice seleccionado y último frame pintado |

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
