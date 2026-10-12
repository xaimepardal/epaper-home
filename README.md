# epaper-home

Firmware para el **Seeed Studio reTerminal E1001** (ESP32-S3 + ePaper monocromo de 7,5" y 800×480) que funciona como **galería de imágenes recibidas por MQTT**, con los botones para pasar de una a otra. Está pensado para funcionar con batería: el dispositivo pasa la mayor parte del tiempo en deep sleep y, al actualizar, usa refresco parcial (sin parpadeo).

## Funciones

- **Imágenes por MQTT** (`reterminal/imagen/<índice>`). Cada imagen va **retenida en su propio topic**, con el índice (0 a 9 por defecto) al final, y se guarda en ese hueco de LittleFS, sustituyendo a la anterior. Son imágenes de 800×480 a 1 bit comprimidas con zlib; el ESP32 las descomprime con el *inflate* de su ROM y descarta las que lleguen corruptas. Un retenido vacío borra esa imagen. Formato en [Formato de las imágenes](#formato-de-las-imágenes).
- **Solo descarga lo que cambia, y se recupera solo**. Los retenidos del broker son el estado completo; la sesión persistente guarda en cola los cambios mientras el dispositivo duerme. Ver [Sincronización](#sincronización).
- **Estado en MQTT** (`reterminal/estado`). En cada conexión publica, retenido, un JSON con la versión del firmware, la causa del despertar, la imagen seleccionada, los huecos ocupados, el RSSI de la WiFi, la batería (% y tensión medida en mV), la PSRAM detectada, la hora y la próxima comprobación.
- **Imagen seleccionada**. La pantalla muestra siempre la imagen del índice seleccionado, que se guarda en LittleFS y sobrevive a reinicios. Al llegar imágenes nuevas no se cambia de índice; solo se repinta si llega una nueva versión de la que se está viendo. Si se borra la seleccionada, pasa a la siguiente que exista.
- **Indicadores en pantalla**, en negro sobre un fondo blanco liso (sin contorno) para que se lean sobre cualquier imagen. Se dibujan al pintar, no se guardan en las imágenes, y si cambian se repinta:
  - **Arriba a la izquierda, «k/N»**: posición de la imagen entre las guardadas (con dos, «1/2» y «2/2»).
  - **Arriba a la derecha, WiFi**: resultado de la última conexión. Normal = suscrito al broker; tachado = no conectó a la WiFi; con «!» = WiFi bien pero el broker no responde o rechaza la suscripción. En modo batería, si la última comprobación fue bien, lleva una **luna** ☾ en la esquina del icono (donde iría el «!»): fue bien y está durmiendo hasta la siguiente. Como el equipo duerme casi siempre, indica cómo fue la última comprobación (como mucho hace 1 h, o al pulsar *refresco*), no si la radio está encendida.
  - **Arriba a la derecha, batería**: porcentaje dentro de un icono de batería. Se mide al despertar, antes de encender la WiFi (al transmitir cae la tensión), con la media recortada de 16 lecturas calibradas del ADC (GPIO1, divisor 1/2 activado por GPIO21). Se muestra en escalones de 5 % y con histéresis (solo cambia si la medida se aleja más de 3 puntos) para no repintar por el ruido. Es una estimación por la tensión de la LiPo; con el USB conectado marca ~100 %, porque la placa no indica si está cargando. «?» si la lectura no es plausible.
- **Botones físicos**. Los tres despiertan el dispositivo del deep sleep; las pulsaciones se cuentan también mientras la pantalla refresca, así que se pueden encadenar. Los GPIO se cambian en `menuconfig`.
  - *Anterior* (izquierdo, GPIO5) y *siguiente* (central, GPIO4) recorren las imágenes guardadas, saltando los huecos vacíos y dando la vuelta, sin conectarse a la WiFi. Cada pulsación repinta al momento solo el «k/N» (refresco parcial rápido) y la imagen se pinta cuando pasan 0,8 s sin pulsar: dos veces *siguiente* = dos imágenes adelante, sin esperar a la intermedia.
  - *Refresco* (verde, GPIO3): el icono de WiFi pasa al momento a «…»; luego conecta, descarga la cola de MQTT y hace un refresco completo. Varias pulsaciones mientras tanto cuentan como una.
  - Mientras dura una tanda de pulsaciones el panel se queda encendido y en modo parcial, y solo se le envía la zona que cambia; se duerme (y se guarda el cristal en `frame.bin`) al terminar. Así cada pulsación es un refresco parcial, sin reiniciar el controlador.
  - Tras despertar por un botón sigue 3 s despierto por si se pulsa otra vez.
- **Refresco parcial incluso tras deep sleep**. El último frame se guarda en LittleFS y, al despertar, se carga como imagen de referencia del controlador mediante un [parche del driver](#parche-del-driver-uc8179).
- **Refresco completo cuando conviene**. Se hace uno tras cualquier reinicio que no sea un deep sleep, cada 10 refrescos parciales, cada 24 h o si el frame guardado no es fiable; así se limpian los restos que deja el parcial.
- **Sin refrescos innecesarios**. Si la imagen resultante es idéntica a la que ya hay en pantalla, no se refresca.
- **MQTT con TLS y usuario**. Admite `mqtts://` con la CA propia del broker incrustada en el firmware ([`main/certs/`](main/certs/README.md)) o con el bundle de CAs públicas, y usuario y contraseña. Los fallos de TLS y de autenticación se explican en el log.
- **WiFi rápida**. Al despertar conecta directamente al canal y BSSID del AP de la última conexión (guardados en memoria RTC), sin escanear; admite canal fijo e IP estática para no depender del DHCP, no escribe en NVS en cada conexión y desactiva el ahorro de energía del módem durante los pocos segundos que está conectado. Si el AP guardado falla, vuelve a buscarlo.
- **Modo batería**. Despierta para comprobar a **horas exactas del reloj** (por defecto cada hora en punto; con 1800, a las :00 y :30) o al pulsar un botón. Por tiempo, se conecta a la WiFi y a MQTT con sesión persistente, guarda las imágenes en cola, pinta si hace falta y vuelve a dormir. Pasar de imagen con los botones no adelanta ni retrasa la siguiente comprobación; el botón de refresco hace una y la siguiente sigue siendo a la hora exacta.
- **Hora por SNTP** en cada conexión (servidor y zona horaria configurables; por defecto `pool.ntp.org` y la hora peninsular española con cambio de horario). El RTC la mantiene durante el deep sleep y cada conexión corrige su deriva; si despierta unos segundos antes o después, esa comprobación cuenta como la de esa hora y no se repite.
- **Modo pruebas** (opcional). No usa deep sleep: se queda conectado, guarda cada imagen en cuanto llega y atiende los botones al momento. Si no hay WiFi, la reintenta cada minuto.
- **Pantallas de estado**. «Esperando imágenes» o «Sin conexión» cuando aún no hay ninguna imagen guardada.
- **PSRAM de 8 MB**. Activada en modo Octal; los bloques de más de 16 KB (framebuffers, mensajes MQTT) van a ella y la RAM interna queda para la WiFi y el TLS. Si no se detecta, arranca sin ella. Sin test de memoria al arrancar, que se repetiría en cada despertar. Ver [Comprobar la PSRAM](#comprobar-la-psram).
- **Volcado de fallos** en la partición `coredump`, que se lee con `idf.py coredump-info`.
- **Herramienta de PC** [`tools/enviar_imagen.py`](tools/enviar_imagen.py). Convierte cualquier imagen (PNG, JPG…) al formato del panel, con tramado o umbral, y la publica en el índice indicado; también borra índices.

## Hardware

| | |
|---|---|
| Dispositivo | [reTerminal E1001](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) — [wiki](https://wiki.seeedstudio.com/getting_started_with_reterminal_e1001/) · [serie reTerminal E](https://wiki.seeedstudio.com/reterminal_e10xx_main_page/) · [datasheet](https://files.seeedstudio.com/Bazaar/product_pdf/100073581.pdf) |
| SoC | ESP32-S3, 32 MB de flash, 8 MB de PSRAM (Octal) |
| Pantalla | ePaper 7,5" monocromo, 800×480, controlador UC8179 (panel GDEY075T7) |
| Conexión al PC | USB-C (conversor USB-serie: aparece como `/dev/ttyUSB0` en Linux) |

Pines que usa el firmware: SPI de la pantalla con MOSI en 9, SCLK en 7, CS en 10, DC en 11, RESET en 12 y BUSY en 13. La microSD comparte el bus, así que se desactiva con SD_EN en 16 y SD_CS en 14. Batería: ADC en GPIO1 con divisor 1/2, activado con GPIO21. Botones (activos a nivel bajo, con pull-up en la placa): KEY0 verde en 3, KEY1 central en 4 y KEY2 izquierdo en 5, según la [wiki de periféricos](https://wiki.seeedstudio.com/reterminal_e10xx_with_arduino_peripherals/).

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

- **WiFi**:

| Opción | Por defecto | |
|---|---|---|
| SSID / Contraseña | vacíos | solo 2,4 GHz; con contraseña se exige WPA2 o superior |
| Canal del AP | 0 (buscar en todos) | si el router tiene el canal fijo, ponlo: ahorra el escaneo cuando no hay AP guardado |
| Intentos buscando el AP | 2 | hasta 10 s cada uno, tras probar 3 s el AP guardado |
| IP estática | vacía (DHCP) | p. ej. `192.168.1.51`; tiene que estar **fuera del rango del DHCP** del router |
| Máscara / Puerta de enlace / DNS | `255.255.255.0` / `192.168.1.1` / la puerta de enlace | solo con IP estática; el DNS solo hace falta si el broker va por nombre |

- **ePaper MQTT**:

| Opción | Por defecto | |
|---|---|---|
| URI del broker MQTT | `mqtt://test.mosquitto.org` | `mqtt://host:1883` sin cifrar o `mqtts://host:8883` con TLS ([ver abajo](#broker-con-tls-y-ca-propia)) |
| Usuario MQTT | vacío (anónimo) | |
| Contraseña MQTT | vacío | se guarda en `sdkconfig` |
| Prefijo de los topics de imágenes | `reterminal/imagen` | cada imagen en `<prefijo>/<índice>` |
| Topic donde publica su estado | `reterminal/estado` | vacío = no publica |
| Alto del texto de los indicadores | 22 px | 0 = ninguno; iconos y márgenes se escalan con él (16 px = tamaño natural de la fuente) |
| Mostrar posición / WiFi / batería | sí / sí / sí | cada indicador por separado |
| Número de imágenes | 10 | índices 0 a N−1, hasta 255; 48 KB cada una |
| GPIO anterior / siguiente / refresco | 5 / 4 / 3 | si «anterior» y «siguiente» van al revés, intercambia 5 y 4 |
| Ms sin pulsar antes de pintar la imagen | 800 | anterior/siguiente: hasta entonces solo se actualiza el «k/N» |
| Ms despierto esperando otra pulsación | 3000 | solo tras despertar por un botón |
| Modo pruebas: no dormir nunca | desactivado | solo para desarrollo |
| Segundos entre comprobaciones | 3600 (1 h) | alineadas al reloj desde la medianoche local: 3600 = en punto, 1800 = :00 y :30; mejor un divisor de 24 h |
| Servidor NTP | `pool.ntp.org` | por nombre necesita DNS (con IP estática, el router); vale una IP |
| Zona horaria | `CET-1CEST,M3.5.0,M10.5.0/3` | formato POSIX TZ |
| Ms esperando mensajes | 500 | sin mensajes nuevos para dar la cola (o los retenidos) por recibida |
| Segundos máximos para conectar | 20 | |
| Parciales antes de un completo | 10 | |
| Horas máximas sin completo | 24 | |

> ⚠️ La configuración se guarda en `sdkconfig`, **que contiene las contraseñas de la WiFi y de MQTT**. Está en `.gitignore`: no lo subas al repositorio.

### Broker con TLS y CA propia

Por ejemplo, el Mosquitto de un NAS con certificado firmado por una CA propia:

1. Copia el certificado de la CA (solo `ca.crt`, nunca la clave) en **`main/certs/mqtt_ca.crt`**. Está en `.gitignore`. Al compilar debe aparecer `MQTT: CA propia …`.
2. En `menuconfig` → *ePaper MQTT*:
   - **URI:** `mqtts://<host>:8883`. El host tiene que figurar en el certificado del broker (CN o SAN). Puede ser una IP si el SAN la incluye; es lo más fiable, porque no depende del DNS de la red.
   - **Usuario y contraseña:** los de un usuario que pueda **leer** `reterminal/imagen/#` y **escribir** `reterminal/estado` (en el ACL de Mosquitto: `topic read reterminal/imagen/#` y `topic write reterminal/estado`).
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
python3 -c "import zlib,sys; sys.stdout.buffer.write(zlib.compress(b'\xff'*48000))" | mosquitto_pub -h test.mosquitto.org -t reterminal/imagen/0 -q 1 -r -s
mosquitto_pub -h test.mosquitto.org -t reterminal/imagen/0 -q 1 -r -n   # y borrarla
mosquitto_sub -h test.mosquitto.org -t reterminal/estado -C 1            # último estado del dispositivo
```

Con un broker con TLS y usuario, añade a `mosquitto_pub` las opciones `-p 8883 --cafile main/certs/mqtt_ca.crt -u <usuario> -P <contraseña>` (`enviar_imagen.py` no usa `-P`, para que la contraseña no se vea en `ps`: se la pasa a `mosquitto_pub` en un fichero de configuración temporal). El usuario que publica necesita permiso de **escritura** en `reterminal/imagen/<índice>` (en el ACL se puede limitar cada servicio a su hueco, p. ej. `topic write reterminal/imagen/0`).

**Publicar siempre con `-q 1` y `-r`.** Sin retener, un dispositivo que pierda la sesión no volvería a recibir esa imagen y la borraría en la siguiente sincronización completa. Los cambios llegan en el siguiente despertar (como mucho 1 h después, o al pulsar *refresco*).

**Botones:** *anterior* y *siguiente* actualizan al momento el «k/N» y pintan la imagen cuando dejas de pulsar (se pueden pulsar varias veces para saltar varias imágenes; refresco parcial, sin red); *refresco* marca «…» en el icono de WiFi, conecta, descarga lo pendiente y hace un refresco completo.

## Sincronización

El dispositivo se conecta siempre con el mismo *client id* (`reterminal-` + final de la MAC) y sesión persistente, y el broker guarda en la cola de esa sesión lo publicado con QoS 1 mientras duerme.

| Al conectar | Qué hace | Qué descarga |
|---|---|---|
| El broker conserva la sesión y su suscripción es la de este firmware (filtro y número de huecos, apuntados en `/lfs/sub.txt`) | **No** se vuelve a suscribir | Solo los cambios en cola; si no hay ninguno, 0 bytes |
| Cualquier otro caso: primer arranque, flash borrada, sesión caducada, broker sin persistencia, cambio de prefijo o de número de imágenes, imagen guardada corrupta | Se suscribe a `<prefijo>/+` | Todos los retenidos (el estado completo). Las que no han cambiado no se reescriben, y se borran los huecos que ya no están en el broker |

`/lfs/sub.txt` se borra al empezar una sincronización completa y solo se vuelve a escribir cuando termina con la conexión abierta y sin errores: si se corta a mitad o falta memoria para alguna imagen, no se borra ningún hueco y el siguiente despertar empieza otra. Si el broker rechaza la suscripción (SUBACK `0x80`, normalmente por el ACL), no se toca nada y el log lo dice. Solo vale un topic por hueco: `reterminal/imagen/07` se ignora (es `reterminal/imagen/7`).

En Mosquitto conviene `persistence true` (para que la cola sobreviva a un reinicio del broker) y, opcionalmente, `persistent_client_expiration 30d` para que no se acumulen sesiones de dispositivos que ya no existen: si la de este caduca, el siguiente despertar hace una sincronización completa.

### Migración desde el formato con el índice en el payload

Las versiones anteriores usaban el topic `reterminal/image` con el índice en el primer byte. Tras actualizar:

1. En `menuconfig`, cambia el prefijo a `reterminal/imagen` si tu `sdkconfig` conservaba el valor antiguo (un `sdkconfig` existente no recoge el nuevo valor por defecto).
2. Vuelve a publicar las imágenes con `tools/enviar_imagen.py` (ahora retenidas). La primera conexión hace una sincronización completa y **borra los huecos que no estén en el broker**.
3. Lo que siga llegando por la suscripción antigua de la sesión se ignora (se avisa en el log).

**Botones:** *anterior* y *siguiente* actualizan al momento el «k/N» y pintan la imagen cuando dejas de pulsar (se pueden pulsar varias veces para saltar varias imágenes; refresco parcial, sin red); *refresco* marca «…» en el icono de WiFi, conecta, descarga lo pendiente y hace un refresco completo.

## Formato de las imágenes

| | |
|---|---|
| Topic | `reterminal/imagen/<índice>` (0 a N−1, en decimal), QoS 1, **retenido** |
| Payload | zlib de 48 000 bytes, máximo 64 KB comprimido. Un retenido vacío borra esa imagen |
| Píxeles | 800×480, 1 bit por píxel, filas de arriba abajo, el bit más significativo es el píxel izquierdo, **1 = blanco** |

Es exactamente lo que produce Pillow:

```python
import zlib
from PIL import Image
payload = zlib.compress(Image.open("imagen.png").resize((800, 480)).convert("1").tobytes(), 9)
```

## Comprobar la PSRAM

El firmware la configura como **Octal** (`sdkconfig.defaults`). Para confirmarlo en tu placa:

1. Con el reTerminal conectado por USB, pregunta al chip (no borra nada; solo lo reinicia):

   ```bash
   python -m esptool --chip esp32s3 -p /dev/ttyUSB0 flash-id
   ```

   En `Features` debe aparecer `Embedded PSRAM 8MB`. Las PSRAM de 8 MB del ESP32-S3 son Octal.
2. Al arrancar, el log dice `PSRAM: 8192 KB`, y `reterminal/estado` trae `"psram":8388608`. Si sale `0`, la PSRAM no se ha inicializado: el modo no es el de la placa (en el log de arranque sale el error de `esp_psram`). En ese caso cambia en `menuconfig` → *Component config → ESP PSRAM → Mode* a *Quad* y vuelve a flashear. El equipo funciona igual sin PSRAM, solo con menos memoria.

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
| `storage` | ~19,8 MB | LittleFS: imágenes por índice (`imgNNN.bin`), índice seleccionado, último frame pintado y suscripción de la sesión (`sub.txt`) |

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
