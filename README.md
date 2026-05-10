# Workspace PlatformIO para ESP32 y ESP32-C3

Este proyecto ya viene listo para compilar y subir firmware a:

- ESP32 (`env:esp32`)
- ESP32-C3 (`env:esp32c3`)

## Requisitos

- VS Code
- Extensión **PlatformIO IDE**
- Python (instalado automáticamente por PlatformIO si hace falta)

## Uso rápido en terminal

Compilar para ESP32:

```bash
pio run -e esp32
```

Compilar para ESP32-C3:

```bash
pio run -e esp32c3
```

Subir a placa ESP32:

```bash
pio run -e esp32 -t upload
```

Subir a placa ESP32-C3:

```bash
pio run -e esp32c3 -t upload
```

Abrir monitor serie:

```bash
pio device monitor -b 115200
```

## Nota de pines LED

Se define por entorno en `platformio.ini`:

- ESP32: `LED_PIN=2`
- ESP32-C3: `LED_PIN=8`

Si tu placa usa otro pin para LED interno, cambia el valor en `build_flags`.
