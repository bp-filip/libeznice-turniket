# Liběznice turnstile

QR-code ticket validation for an ice rink turnstile.

A visitor scans a ticket; an Arduino Nano reads the code and passes it to an
ESP32, which validates it against the rink's server. The Nano then drives
the turnstile relays and shows the server's message on a 20x4 LCD.

## Layout

- `nano_turnstile/` — Arduino Nano: scanner, relays, LCD
- `esp32_turnstile/` — ESP32: WiFi, validator API, OTA updates
- `docs/` — pin maps, wiring, serial protocol, server endpoint

`CLAUDE.md` has the working context: hardware constraints found the hard
way, current state, and known rough edges. Read it before changing firmware.

## Setup

**Libraries.** Install through the Arduino IDE Library Manager:

- `ArduinoJson` by Benoit Blanchon — ESP32
- `LiquidCrystal I2C` by Frank de Brabander — Nano

**Board support.** ESP32 core v3.x or later, via Boards Manager.

**Credentials.** Copy `esp32_turnstile/secrets.h.example` to
`esp32_turnstile/secrets.h` and fill it in. `secrets.h` is gitignored — this
repository has to stay public for OTA release downloads to work without
authentication, so nothing secret can live in it.

## Flashing

**Nano** — disconnect D0 and D1 first, or the upload fails to sync and
silently leaves the previous sketch in place. Reconnect afterwards.

**ESP32 over USB** — as normal.

**ESP32 over the network** — once it has an IP, it appears under
Tools → Port as a network port named `esp32-scanner`. Uploading asks for the
OTA password. Any firmware flashed this way must itself contain the OTA
code, or the only way back is a USB cable.

**ESP32 from GitHub** — the device checks this repository's latest release
for `version.txt` on boot and every 6 hours, and installs `firmware.bin` if
the version differs from `FIRMWARE_VERSION`. To publish: bump the version,
Sketch → Export Compiled Binary, rename the `.bin` to `firmware.bin`, and
attach it to a new release along with a `version.txt` containing the matching
version string.

## Debugging

The Nano has no serial monitor — its hardware UART belongs to the ESP32, so
its only output is the LCD. The ESP32's USB monitor at 115200 shows traffic
in both directions and is the place to watch.
