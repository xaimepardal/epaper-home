#pragma once

// Tensión de la batería del reTerminal E1001: divisor 1/2 en GPIO1 (ADC1),
// activado con GPIO21. Fuente: wiki de Seeed, "Arduino Cookbook: Onboard
// Peripherals".

// Tensión de la batería en mV (media recortada de varias lecturas calibradas),
// o -1 si no se pudo medir. Conviene medir con la WiFi apagada: al transmitir
// la tensión cae y saldría un porcentaje más bajo.
int battery_mv();

// Estado de carga estimado (0-100) a partir de la tensión en reposo de una
// LiPo, o -1 si `mv` no es plausible. Con el USB conectado se lee la tensión de
// carga (~4,2 V) y sale 100: la placa no expone si está cargando.
int battery_percent(int mv);
