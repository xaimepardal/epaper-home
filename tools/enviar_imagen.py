#!/usr/bin/python3
"""Convierte una imagen al formato del reTerminal E1001 y la publica por MQTT.

Formato que espera el firmware (topic reterminal/imagen):
    zlib( 800x480 a 1 bit por píxel )  -> 48000 bytes sin comprimir
    filas de arriba abajo, MSB = píxel de la izquierda, bit 1 = blanco
que es exactamente lo que da Pillow con Image.convert("1").tobytes().

Requiere Pillow (sudo pacman -S python-pillow) y mosquitto_pub. Usa el Python del
sistema a propósito: con export.sh de ESP-IDF cargado, "python3" es el venv de IDF.

Ejemplos:
    ./enviar_imagen.py foto.jpg                     # tramado, publica
    ./enviar_imagen.py logo.png --umbral 128        # blanco/negro sin tramado
    ./enviar_imagen.py foto.jpg --previa prev.png   # sólo genera la previsualización
"""

import argparse
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
    ap.add_argument("imagen")
    ap.add_argument("--host", default="test.mosquitto.org")
    ap.add_argument("--topic", default="reterminal/imagen")
    ap.add_argument("--topic-texto", default="reterminal/texto",
                    help="se borra su mensaje retenido para que no compita con la imagen")
    ap.add_argument("--umbral", type=int, help="0-255: blanco/negro puro en vez de tramado")
    ap.add_argument("--invertir", action="store_true")
    ap.add_argument("--rotar", type=int, default=0, help="grados (90, 180, 270)")
    ap.add_argument("--previa", help="guarda la imagen resultante en PNG y no publica")
    args = ap.parse_args()

    img = convertir(args.imagen, args.umbral, args.invertir, args.rotar)
    crudo = img.tobytes()
    assert len(crudo) == ANCHO * ALTO // 8, len(crudo)
    comprimido = zlib.compress(crudo, 9)
    print(f"{len(crudo)} bytes -> {len(comprimido)} comprimidos ({100 * len(comprimido) / len(crudo):.1f} %)")

    if args.previa:
        img.save(args.previa)
        print(f"Previsualización en {args.previa}")
        return

    with tempfile.NamedTemporaryFile(suffix=".z") as f:
        f.write(comprimido)
        f.flush()
        subprocess.run(["mosquitto_pub", "-h", args.host, "-t", args.topic, "-r", "-q", "1", "-f", f.name],
                       check=True)
    if args.topic_texto:
        subprocess.run(["mosquitto_pub", "-h", args.host, "-t", args.topic_texto, "-r", "-n"], check=True)
    print(f"Publicada en {args.host} {args.topic}")


if __name__ == "__main__":
    sys.exit(main())
