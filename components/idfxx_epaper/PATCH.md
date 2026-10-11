# Copia modificada

Copia de `cleishm/idfxx_epaper` / `cleishm/idfxx_epaper_uc8179` v1.0.0
(Apache-2.0, https://github.com/cleishm/idfxx), modificada para epaper_mqtt.

Cambios (buscar `[epaper_mqtt patch]`):
- `panel::restore_baseline()` / `try_restore_baseline()` y el hook
  `do_write_baseline()`: tras despertar el panel, carga la imagen que hay en
  el cristal en la RAM de "imagen anterior" del controlador para que el
  siguiente refresco pueda ser parcial.
- `mono_framebuffer::data()` no-const, para cargar un frame guardado.
- `uc8179::do_write_baseline()`: escribe el frame con el comando 0x10.

Si se actualiza la librería, hay que volver a aplicar estos cambios.
