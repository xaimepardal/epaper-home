#!/usr/bin/python3
"""Convierte una imagen al formato del reTerminal E1001 y la publica por MQTT.

Formato que espera el firmware (topic reterminal/imagen/<índice>):
    zlib( 800x480 a 1 bit por píxel )
    48000 bytes sin comprimir, filas de arriba abajo, MSB = píxel de la
    izquierda, bit 1 = blanco: lo que da Pillow con Image.convert("1").tobytes().
Un retenido vacío borra esa imagen del broker y del dispositivo.

Se publica retenida y con QoS 1: el broker guarda el estado completo (lo que
recibe un dispositivo nuevo o con la flash borrada) y además pone el cambio en
la cola de la sesión del reTerminal, que lo recoge en su siguiente despertar.

Requiere Pillow (sudo pacman -S python-pillow) y mosquitto_pub. Usa el Python del
sistema a propósito: con export.sh de ESP-IDF cargado, "python3" es el venv de IDF.

Broker con TLS y usuario (p. ej. el Mosquitto del NAS): --puerto 8883 --ca ca.crt
--usuario <u>. La contraseña se toma de MQTT_PASS o se pide por teclado.

Ejemplos:
    ./enviar_imagen.py 0 foto.jpg                     # tramado, en el índice 0
    ./enviar_imagen.py 1 foto.jpg --host 192.168.1.50 --ca ../main/certs/mqtt_ca.crt --usuario gatomarmota
    ./enviar_imagen.py 2 logo.png --threshold 128        # blanco/negro sin tramado
    ./enviar_imagen.py 2 --borrar                     # borra la imagen 2
    ./enviar_imagen.py 0 foto.jpg --previa prev.png   # sólo genera la previsualización
"""

import argparse
import getpass
import os
import subprocess
import sys
import tempfile
import zlib

from PIL import Image, ImageOps

WIDTH, HEIGHT = 800, 480


def convert_image(path, threshold=None, invert=False, rotate=0):
    img = Image.open(path)
    img = ImageOps.exif_transpose(img)  # respeta la orientación de las fotos de móvil
    if rotate:
        img = img.rotate(rotate, expand=True)
    # Transparencia sobre fondo blanco
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        background = Image.new("RGBA", img.size, "white")
        img = Image.alpha_composite(background, img)
    img = img.convert("L")
    # Encajar en 800x480 sin deformar, centrada sobre blanco
    img = ImageOps.contain(img, (WIDTH, HEIGHT), Image.LANCZOS)
    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    canvas.paste(img, ((WIDTH - img.width) // 2, (HEIGHT - img.height) // 2))
    if invert:
        canvas = ImageOps.invert(canvas)
    if threshold is None:
        return canvas.convert("1")  # tramado Floyd-Steinberg
    return canvas.point(lambda p: 255 if p >= threshold else 0).convert("1", dither=Image.NONE)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("index", metavar="indice", type=int, help="hueco de la imagen en el dispositivo (0-254)")
    parser.add_argument("image", metavar="imagen", nargs="?")
    parser.add_argument("--borrar", dest="delete", action="store_true", help="borra la imagen de ese índice")
    parser.add_argument("--host", default="test.mosquitto.org")
    parser.add_argument("--puerto", dest="port", type=int, help="por defecto 1883, o 8883 con --ca")
    parser.add_argument("--ca", help="CA del broker para TLS (mqtts)")
    parser.add_argument("--usuario", dest="user", help="usuario MQTT (contraseña: MQTT_PASS o se pide)")
    parser.add_argument("--topic", default="reterminal/imagen", help="prefijo: se publica en <topic>/<índice>")
    parser.add_argument("--umbral", dest="threshold", type=int, help="0-255: blanco/negro puro en vez de tramado")
    parser.add_argument("--invertir", dest="invert", action="store_true")
    parser.add_argument("--rotar", dest="rotate", type=int, default=0, help="grados (90, 180, 270)")
    parser.add_argument("--previa", dest="preview", help="guarda la imagen resultante en PNG y no publica")
    args = parser.parse_args()
    if not 0 <= args.index <= 254:
        parser.error("el índice va de 0 a 254 (y por debajo del máximo configurado en el firmware)")
    if args.delete == (args.image is not None):
        parser.error("indica una imagen o --borrar")

    payload = b""
    if args.image:
        img = convert_image(args.image, args.threshold, args.invert, args.rotate)
        raw = img.tobytes()
        assert len(raw) == WIDTH * HEIGHT // 8, len(raw)
        compressed = zlib.compress(raw, 9)
        print(f"{len(raw)} bytes -> {len(compressed)} comprimidos ({100 * len(compressed) / len(raw):.1f} %)")
        if args.preview:
            img.save(args.preview)
            print(f"Previsualización en {args.preview}")
            return
        payload = compressed

    connection = ["-h", args.host, "-p", str(args.port or (8883 if args.ca else 1883))]
    if args.ca:
        connection += ["--cafile", args.ca]

    topic = f"{args.topic}/{args.index}"
    publish = ["mosquitto_pub", *connection, "-t", topic, "-q", "1", "-r"]
    with tempfile.TemporaryDirectory() as tmp:
        env = None
        if args.user:
            password = os.environ.get("MQTT_PASS") or getpass.getpass(f"Contraseña MQTT de {args.user}: ")
            # Con -P la contraseña se vería en la lista de procesos (ps). mosquitto_pub
            # lee además $XDG_CONFIG_HOME/mosquitto_pub, una opción por línea.
            config = os.path.join(tmp, "mosquitto_pub")
            with open(os.open(config, os.O_WRONLY | os.O_CREAT, 0o600), "w") as f:
                f.write(f"-u {args.user}\n-P {password}\n")
            env = {**os.environ, "XDG_CONFIG_HOME": tmp}
        if args.delete:
            subprocess.run([*publish, "-n"], check=True, env=env)
        else:
            data_path = os.path.join(tmp, "imagen.z")
            with open(data_path, "wb") as f:
                f.write(payload)
            subprocess.run([*publish, "-f", data_path], check=True, env=env)
    action = "Borrado" if args.delete else "Publicada"
    print(f"{action} el índice {args.index} en {args.host} {topic}")


if __name__ == "__main__":
    sys.exit(main())
