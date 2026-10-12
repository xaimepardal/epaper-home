# Certificado de la CA del broker MQTT

Para un broker con certificado de una **CA propia** (por ejemplo el Mosquitto del NAS), copia aquí su certificado con este nombre exacto:

```
main/certs/mqtt_ca.crt
```

Si el fichero existe, se incrusta en el firmware y es la única CA en la que confía la conexión `mqtts://`. Si no existe, `mqtts://` valida con el bundle de CAs públicas de ESP-IDF, que sirve para brokers con certificado de Let's Encrypt u otra CA pública.

`*.crt` está en `.gitignore`: el certificado no se sube al repositorio.
