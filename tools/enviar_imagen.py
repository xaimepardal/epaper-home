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
    ./enviar_imagen.py 2 logo.png --umbral 128        # blanco/negro sin tramado
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

ANCHO, ALTO = 800, 480


def convertir(ruta, umbral=None, invertir=False, rotar=0):
    img = Image.open(ruta)
    img = ImageOps.exif_transpose(img)  # respeta la orientación de las fotos de móvil
    if rotar:
        img = img.rotate(rotar, expand=True)
    # Transparencia sobre fondo blanco
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        fondo = Image.new("RGBA", img.size, "white")
        img = Image.alpha_composite(fondo, img)
    img = img.convert("L")
    # Encajar en 800x480 sin deformar, centrada sobre blanco
    img = ImageOps.contain(img, (ANCHO, ALTO), Image.LANCZOS)
    lienzo = Image.new("L", (ANCHO, ALTO), 255)
    lienzo.paste(img, ((ANCHO - img.width) // 2, (ALTO - img.height) // 2))
    if invertir:
        lienzo = ImageOps.invert(lienzo)
    if umbral is None:
        return lienzo.convert("1")  # tramado Floyd-Steinberg
    return lienzo.point(lambda p: 255 if p >= umbral else 0).convert("1", dither=Image.NONE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("indice", type=int, help="hueco de la imagen en el dispositivo (0-254)")
    ap.add_argument("imagen", nargs="?")
    ap.add_argument("--borrar", action="store_true", help="borra la imagen de ese índice")
    ap.add_argument("--host", default="test.mosquitto.org")
    ap.add_argument("--puerto", type=int, help="por defecto 1883, o 8883 con --ca")
    ap.add_argument("--ca", help="CA del broker para TLS (mqtts)")
    ap.add_argument("--usuario", help="usuario MQTT (contraseña: MQTT_PASS o se pide)")
    ap.add_argument("--topic", default="reterminal/imagen", help="prefijo: se publica en <topic>/<índice>")
    ap.add_argument("--umbral", type=int, help="0-255: blanco/negro puro en vez de tramado")
    ap.add_argument("--invertir", action="store_true")
    ap.add_argument("--rotar", type=int, default=0, help="grados (90, 180, 270)")
    ap.add_argument("--previa", help="guarda la imagen resultante en PNG y no publica")
    args = ap.parse_args()
    if not 0 <= args.indice <= 254:
        ap.error("el índice va de 0 a 254 (y por debajo del máximo configurado en el firmware)")
    if args.borrar == (args.imagen is not None):
        ap.error("indica una imagen o --borrar")

    payload = b""
    if args.imagen:
        img = convertir(args.imagen, args.umbral, args.invertir, args.rotar)
        crudo = img.tobytes()
        assert len(crudo) == ANCHO * ALTO // 8, len(crudo)
        comprimido = zlib.compress(crudo, 9)
        print(f"{len(crudo)} bytes -> {len(comprimido)} comprimidos ({100 * len(comprimido) / len(crudo):.1f} %)")
        if args.previa:
            img.save(args.previa)
            print(f"Previsualización en {args.previa}")
            return
        payload = comprimido

    conexion = ["-h", args.host, "-p", str(args.puerto or (8883 if args.ca else 1883))]
    if args.ca:
        conexion += ["--cafile", args.ca]
    if args.usuario:
        clave = os.environ.get("MQTT_PASS") or getpass.getpass(f"Contraseña MQTT de {args.usuario}: ")
        conexion += ["-u", args.usuario, "-P", clave]

    topic = f"{args.topic}/{args.indice}"
    publicar = ["mosquitto_pub", *conexion, "-t", topic, "-q", "1", "-r"]
    if args.borrar:
        subprocess.run([*publicar, "-n"], check=True)
    else:
        with tempfile.NamedTemporaryFile(suffix=".z") as f:
            f.write(payload)
            f.flush()
            subprocess.run([*publicar, "-f", f.name], check=True)
    accion = "Borrado" if args.borrar else "Publicada"
    print(f"{accion} el índice {args.indice} en {args.host} {topic}")


if __name__ == "__main__":
    sys.exit(main())
