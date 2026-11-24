# Hardware

## Arduino Nano pins

| Pin | Use | Notes |
|-----|-----|-------|
| D0 (RX) | ESP32 GPIO27 via shifter | disconnect before uploading |
| D1 (TX) | ESP32 GPIO14 via shifter | disconnect before uploading |
| D2 | open out relay | exit, held open permanently |
| D3 | open in relay | entry |
| D4 | arrow in relay | red / green |
| D5 | scanner TX | inverted TTL, SoftwareSerial RX |
| D10 | reserved | SoftwareSerial demands a TX pin it never uses |
| A4 / A5 | LCD SDA / SCL | I2C at 100 kHz, the PCF8574's rated maximum |

The relay board is **active-LOW**: writing `LOW` energises a coil.

The pin numbers in the sketch reflect the wiring as built.

The arrow relay's contacts are wired the other way round from the original
design, so the sketch inverts it: the coil is **energised for red** and
released for green (`ARROW_YES_STATE` / `ARROW_NO_STATE`). A consequence is
that the arrow shows green whenever the Nano is unpowered or resetting. The
entry relay is released at those times too, so the turnstile stays shut.

## ESP32 pins

| Pin | Use |
|-----|-----|
| GPIO14 | Serial2 RX, from Nano D1 |
| GPIO27 | Serial2 TX, to Nano D0 |

Avoid GPIO12 for RX: it is a strapping pin that sets flash voltage at boot,
and an idle UART line holds it high, which stops the board booting.

GPIO 6-11 are wired to flash. GPIO 34-39 are input-only.

## Level shifter

Bidirectional BSS138-style board. HV to Nano 5V, LV to ESP32 3V3, all three
grounds bonded.

This part is a known weak point - see the crosstalk note in CLAUDE.md.

## Display

20x4 character LCD on a PCF8574 I2C backpack, address `0x27` (some boards
are `0x3F`). Contrast is set by the trimpot on the backpack.

The controller has no accented glyphs. `czech_lcd.h` loads up to 8 accented
letters per message into custom character memory.

### Random characters on screen

The backpack drives the controller four bits at a time. If one transfer is
lost, every later character is read half a byte off and the screen shows
garbage. The Nano resynchronises the controller before every redraw, so
this now clears at the next message (at most about 12 seconds, the idle
refresh). Redraws are also held back until 50 ms after any relay switches,
so the noise of a switching relay never coincides with an LCD transfer.
Neither stops a glitch caused by a supply dip affecting the LCD on its
own. Usual causes, and the
hardware fixes for them:

- **Relay switching noise or supply dips.** Power the relay board's coil
  side separately (remove the JD-VCC jumper on boards that have one, so the
  optocouplers isolate it). Put 100 nF plus 220 uF across the backpack's
  5V and GND, close to the backpack.
- **Long or badly routed I2C wires.** Keep SDA/SCL short and away from relay
  and 24V wiring; twist each with a ground wire if they run any distance.
- **Inductive loads on the relay contacts** need a flyback diode (DC) or an
  RC snubber (AC) across the load.

Wrong letters that are not garbage (a missing word, rows shifted) point at
the ESP32 link instead. Those lines are now caught by the checksum, see
`protocol.md`.

I2C speed: `lcd.init()` calls `Wire.begin()`, which resets the bus to
100 kHz, so `Wire.setClock()` only has an effect after `init()`.

## Scanner: RTScan RT830A

Wired directly, 5V, TX only - the Nano never talks to it.

Configured by scanning setup barcodes from the manual
(`User-Manual-RT830A-RT870-RT860_V1.2.3.pdf`):

| Setting | Section | Why |
|---------|---------|-----|
| RS232 interface | 2.7 | default is USB HID keyboard; the TTL pins stay silent otherwise |
| 9600 baud | 2.2 | default 115200 is too fast for SoftwareSerial on a 16 MHz AVR |
| 8N1 | 2.3 | only needed if framing was changed by accident |
| Add CRLF | 4.1 | no suffix by default, so codes have no terminator |

"RS232 interface" is RTScan's name for TTL-level output on the 12-pin
header. It inverts the logic, which is why `SoftwareSerial` is constructed
with `true`. Do not connect the development kit's DB9 - that is true RS232
at ±12V and would damage the Nano.

Avoid the "Restore All Factory Defaults" barcode: it returns the module to
USB keyboard mode and everything above has to be redone.

## Turnstile

Entry and exit are separate relay-driven inputs. The arrow indicator is
24V, driven through the third relay: NO contact shows one colour, NC the
other, so a single relay covers both states.

The panic input is unused - that motor is broken.

An earlier attempt used SSRs for the arrows. They stayed permanently lit,
most likely because AC-output SSRs (triac) cannot switch DC off. Mechanical
relays replaced them.
