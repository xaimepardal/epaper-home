#pragma once

// Conexión WiFi pensada para despertares cortos: sin escaneo completo (canal
// fijo y BSSID del último AP en memoria RTC), IP estática opcional y sin
// escrituras en NVS. Sustituye a example_connect().

// Conecta y espera a tener IP. Se puede llamar una sola vez por arranque.
bool wifi_conectar();

// Apaga la WiFi si se llegó a iniciar (seguro de llamar siempre).
void wifi_desconectar();
