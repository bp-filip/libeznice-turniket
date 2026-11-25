# CLAUDE.md

Context for working on this repository. Read this before changing firmware.

## What this is

A QR-code turnstile for an ice rink in Liběznice. A visitor scans a ticket
QR code; the system validates it against a server API and, if valid, lets
them through the entry turnstile.

Two microcontrollers, deliberately:

- **Arduino Nano** — owns the hardware. Reads the scanner, drives the relays,
  writes the LCD. Has no network access.
- **ESP32** — owns the network. Talks to the validator API over WiFi and
  receives firmware updates over the air.

They are joined by a 9600-baud UART link through a level shifter. The Nano
is the one that would keep the exit working if everything else died, which
is why the exit relay is held open by the Nano alone.

## Repository layout

```
esp32_turnstile/
  esp32_turnstile.ino   ESP32 firmware
  turnstile.h           Control struct (see "IDE quirk" below)
  secrets.h             credentials - GITIGNORED, not in the repo
  secrets.h.example     template
nano_turnstile/
  nano_turnstile.ino    Nano firmware
  czech_lcd.h           Czech diacritics on an HD44780
docs/
  hardware.md           pin maps, wiring, scanner configuration
  protocol.md           the serial protocol between the boards
  validator.php         reference copy of the server endpoint
```

Both sketch folders open directly in the Arduino IDE.

## Hard-won constraints

These were all found the slow way. Changing them will break things.

**The scanner outputs inverted TTL.** The RTScan RT830A in "RS232 interface"
mode inverts its logic while staying at TTL voltage. Hence the `true` third
argument to `SoftwareSerial`. The AVR hardware UART cannot invert, so the
scanner is stuck on SoftwareSerial and the ESP32 link gets the real UART.

**The scanner must be at 9600.** Its factory default is 115200, which
SoftwareSerial cannot receive on a 16 MHz AVR. Configuration is done by
scanning setup barcodes from the manual, not in software.

**Relays are active-LOW.** `ACTIVE` is `LOW` in the Nano sketch. The arrow
relay is wired inverted on top of that: energised = red. See
`docs/hardware.md`.

**The Nano has no Serial Monitor.** Its hardware UART belongs to the ESP32.
All Nano debugging happens on the LCD. The ESP32's USB monitor at 115200
shows both sides of the link.

**Uploading to the Nano requires D0 and D1 disconnected.** Otherwise avrdude
fails to sync and silently leaves the old sketch in flash. This has already
caused one long debugging session - if Nano behaviour does not match the
source, suspect this first.

**The link crosstalks.** The bidirectional BSS138-style level shifter is
designed for open-drain I2C, not push-pull UART. The ESP32 has been seen
receiving its own transmissions, which once caused a runaway loop of API
calls. Both sketches now drain their RX after transmitting, the ESP32
rejects any line containing its own protocol markers, and every line
carries a CRC-8 checksum that the receiver checks (see `docs/protocol.md`).
The Nano accepts only checksummed lines, so **both boards must be flashed
from the same release** - a Nano update means an ESP32 update too. **The underlying
hardware issue is unfixed.** The intended fix is to drop the shifter on the
ESP32 to Nano direction entirely (the Nano reads 3.3V as HIGH) and use a
simple 1k/2k divider for Nano to ESP32.

**The LCD resyncs before every redraw.** Random characters on the LCD are
the 4-bit interface falling half a byte out of step. `lcdResync()` in the
Nano sketch writes the 3-3-3-2 nibble sequence straight to the PCF8574
(the library's `command()` is `inline` in its `.cpp` and cannot be called
from outside). It costs about 10 ms of `delay()` per redraw. Redraws
also wait until `RELAY_SETTLE_MS` (50 ms) after the last relay change, so an
LCD transfer never overlaps relay switching noise - without this, the end
of every entry hold released the relays and redrew the idle prompt in the
same pass. All relay writes must go through `writeRelayPin()` for the
timer to see them. See `docs/hardware.md` for the hardware causes.

**The LCD cannot show diacritics natively.** `czech_lcd.h` decodes UTF-8,
loads up to 8 accented letters into the display's custom character memory
per message, and falls back to unaccented ASCII beyond that. Messages pass
through the ESP32 byte for byte; nothing transliterates them.

**The Nano never writes the screen on its own** after the boot text. All
LCD content after boot comes from the ESP32 as `MSG:` lines, and a `CTL:`
line is a complete answer without one. `SEND_LCD_MESSAGES` in the ESP32
sketch switches every message off.

**Word wrapping happens on the ESP32**, not the Nano, by padding each line
to exactly 20 cells. Widths are counted in display cells rather than bytes,
because an accented letter is 2 bytes but 1 cell.

## Current state

Working: scanning, validation, entry relay with a 10-second hold, red/green
arrow, always-open exit, LCD messages with Czech diacritics and word wrap,
local OTA over the IDE network port, GitHub-hosted pull updates.

Only `ALLOW_SINGLE` is being tested. `ALLOW_MULTI` is implemented (5-minute
hold, blinking arrow) but unexercised. `ALLOW_MULTI_HOUR` (1-hour hold,
steady green arrow) was added in 1.0.2; the reference `docs/validator.php`
does not return it yet.

A new scan does **not** cancel an entry in progress: the turnstile stays as
it is until the ESP32 answers for the new code. This is deliberate.

### Known rough edges, roughly in priority order

1. **Level shifter crosstalk** — see above. Hardware fix outstanding.
2. **`setInsecure()` on all TLS** — certificate validation is off. This was
   acceptable on a bench; it is not for a device pulling firmware from the
   internet, because a MITM on the update path gets to run their own code on
   the board. The likely fix is an NTP sync (`configTime`) before the first
   update check, since an unset clock makes every certificate look invalid.
3. **The screen is never refreshed.** The Nano writes the LCD only at boot
   and on a `MSG:` line, and the ESP32 sends the idle prompt once per
   result, so a glitched screen stays glitched until the next message. With
   `SEND_LCD_MESSAGES = false` (ESP32) the LCD is written only at boot -
   meant as a stopgap while the screen hardware is unreliable.
4. **No rollback protection.** A firmware build that crashes before reaching
   `checkForUpdate()` bricks the device until someone attaches USB. Requires
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, which the Arduino IDE does not
   expose - would need PlatformIO or ESP-IDF.
5. **`FIRMWARE_VERSION` must match the latest release.** GitHub OTA only
   checks that it *differs* from `version.txt` in the latest release, not
   that it is newer. A build whose version differs from the release will
   "update" itself to the release on every boot, including downgrading.
   Bump it together with publishing a release (currently 1.0.3). Because the
   Nano requires checksums, an ESP32 that downgrades itself to 1.0.2 or
   earlier loses contact with the Nano entirely.
6. **Panic output removed.** The panic motor is broken. `ALLOW_MULTI`
   therefore differs from `ALLOW_SINGLE` only in hold duration and the
   blinking arrow.
7. **Credentials end up in the public release binary.** `secrets.h` keeps
   them out of the source, but `firmware.bin` is attached to a public
   release and the WiFi password, API token and OTA password can be read out
   of it with `strings`.

## Conventions

- Plain Arduino C++, no external build system. Libraries: `ArduinoJson`
  (ESP32) and `LiquidCrystal I2C` by Frank de Brabander (Nano).
- The Nano has 2 KB of RAM. Prefer fixed buffers over `String`; `String` is
  used freely on the ESP32 only.
- `loop()` on the Nano must not block. SoftwareSerial drops scanner bytes
  during any `delay()`.
- **IDE quirk:** the Arduino IDE injects function prototypes at the top of a
  `.ino`, above anything defined in it. A custom type used in a function
  signature must therefore live in a `.h`, which is why `turnstile.h` exists
  for a six-field struct. Do not move it back into the `.ino`.

## Testing

There is no automated test setup; verification is by hand on the bench.
Useful sequence after a change:

1. Flash the Nano over USB with D0/D1 disconnected, reconnect them.
2. Flash the ESP32, watch its monitor at 115200.
3. Scan a valid code: expect `from Nano:` then `api: HTTP 200`, a `CTL:`
   line, a `MSG:` line, entry relay closed for 10 s, arrow green.
4. Scan an unknown code: expect `DENY`, arrow stays red.
5. Scan a second code mid-entry: the entry stays as it is until the new
   result arrives, then follows it. Use a *different* code -
   `REPEAT_LOCKOUT_MS` drops the same code within 2 seconds (2.5 s on the
   Nano, which drops it before sending, so the screen does not change).

If Nano behaviour does not match the source, re-check that the upload
actually succeeded before debugging anything else.
