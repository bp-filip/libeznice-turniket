# Serial protocol

9600 baud, 8N1, newline-terminated lines, between Nano D0/D1 and ESP32
GPIO14/27.

## Checksum

Every line in both directions ends in `*HH`: a CRC-8 (polynomial `0x07`,
initial value 0, as CRC-8/SMBUS; `"123456789"` gives `F4`) of every byte
before the `*`, as two uppercase hex digits. The examples below omit it;
on the wire the control line reads

```
CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000;BLINK=NO*7C
```

The receiver checks the last three bytes and drops the line if the
checksum is missing or wrong. A `*` inside the payload is harmless, because
only the final three bytes are read as the checksum.

Dropped lines are not answered: on the crosstalking link a reply to garbage
could start a loop. A dropped scan or result therefore ends in the Nano's
10-second reply timeout, which closes the entry.

The Nano accepts **only** lines with a valid checksum. Nano firmware with
checksums needs ESP32 firmware 1.0.3 or later, and the other way round, so
the two boards must be flashed together.

## Nano to ESP32

The raw scanned code, one line (plus the checksum):

```
PEVc5LvcCDeOcvwtlOcHY2weAU
```

Nothing else is ever sent in this direction. The ESP32 rejects anything
that looks like its own protocol, contains non-printable bytes, or is
shorter than 4 characters - all symptoms of link crosstalk rather than a
real scan.

## ESP32 to Nano

Two line types.

### Control

```
CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000;BLINK=NO
```

| Field | Values | Meaning |
|-------|--------|---------|
| `OPEN_IN` | `YES` / `NO` | entry relay |
| `OPEN_OUT` | `YES` / `NO` | **ignored** while `ALWAYS_OPEN_OUT` is set |
| `ARROW_IN` | `YES` / `NO` | green / red |
| `HOLD` | milliseconds | how long to hold before reverting to defaults |
| `BLINK` | `YES` / `NO` | blink the arrow every 500 ms for the duration of the hold |

Parsing is by substring search, so field order does not matter and unknown
fields are ignored. A missing field falls back to its default.

`HOLD=0` means no timed hold - used for denials, where nothing needs
reverting. `BLINK` only has an effect while a hold is running.

| Action | `OPEN_IN` | `ARROW_IN` | `HOLD` | `BLINK` |
|--------|-----------|------------|--------|---------|
| `ALLOW_SINGLE` | YES | YES | 10000 | NO |
| `ALLOW_MULTI` | YES | YES | 300000 | YES |
| `ALLOW_MULTI_HOUR` | YES | YES | 3600000 | NO |
| `DENY` / unknown | NO | NO | 0 | NO |

### Message

```
MSG:Vstupenka je platna.
```

UTF-8, already wrapped and padded by the ESP32 to exactly 20 characters per
row so the Nano's row loop breaks lines correctly. Messages shorter than
four rows are padded with a leading blank row so they start on row 2.

A `MSG:` with no preceding `CTL:` just updates the display - used for the
idle prompt and the "checking" text. Equally, a `CTL:` needs no `MSG:`
after it: the control line alone is the answer to a scan, and only it ends
the Nano's wait for a reply.

### Who writes the screen

The Nano writes the LCD exactly once by itself, at boot ("Líbeznice
turniket", `BOOT_TEXT`). After that it changes the screen only when a `MSG:`
line arrives, and whatever it was last sent stays up. It has no idle,
"checking" or error texts of its own.

The ESP32 sends the "checking" text on each scan, the result, and - once
the result has had its time (the entry hold, or `RESULT_SHOW_MS` when
nothing is held) - the idle prompt, once. First idle prompt after
`BOOT_TEXT_MS`.

With `SEND_LCD_MESSAGES = false` in the ESP32 sketch, no `MSG:` line is ever
sent. Relays and arrow work the same; the boot text stays on screen.

## Timing

| Constant | Where | Value | Purpose |
|----------|-------|-------|---------|
| `API_CONNECT_TIMEOUT_MS` | ESP32 | 3000 | TCP connect to the validator |
| `API_HANDSHAKE_TIMEOUT_S` | ESP32 | 4 s | TLS handshake (library default is 120 s) |
| `API_TIMEOUT_MS` | ESP32 | 4000 | waiting for the validator's reply |
| `RESULT_DEADLINE_MS` | ESP32 | 9000 | a result later than this is sent as DENY ("Server neodpovídá"); must stay below the Nano's `REPLY_TIMEOUT` |
| `REPLY_TIMEOUT` | Nano | 10000 | gives up waiting for a `CTL:` and closes the entry; the screen is left alone |
| `REPEAT_LOCKOUT_MS` | ESP32 | 2000 | same code twice in a row is one scan |
| `REPEAT_LOCKOUT_MS` | Nano | 2500 | drops the repeat before sending, so the Nano never waits for a reply the ESP32 will not give; kept above the ESP32's |
| `BOOT_TEXT_MS` | ESP32 | 5000 | Nano's boot text stays this long before the first idle prompt |
| `RESULT_SHOW_MS` | ESP32 | 15000 | how long a result stays when no entry is held, before the idle prompt |
| `HOLD_SINGLE_MS` | ESP32 | 10000 | entry hold for a single ticket |
| `HOLD_MULTI_MS` | ESP32 | 300000 | entry hold for a group ticket, untested |
| `HOLD_MULTI_HOUR_MS` | ESP32 | 3600000 | entry hold for a one-hour ticket |
| `BLINK_MS` | Nano | 500 | arrow blink period during a `BLINK=YES` hold |

Both boards flush and drain their RX after transmitting, to discard anything
the crosstalking link reflects back during their own transmission.

## Server API

`POST` to the validator with a `Hash` header carrying the API token.

Request:

```json
{ "qr_code": "PEVc5LvcCDeOcvwtlOcHY2weAU" }
```

Response:

```json
{ "status": "valid", "action": "ALLOW_SINGLE", "message": "Vstupenka je platna." }
```

`action` is one of `ALLOW_SINGLE`, `ALLOW_MULTI`, `ALLOW_MULTI_HOUR`,
`DENY`. Anything
unrecognised is treated as `DENY`.

The API returns JSON on error codes too (400, 401, 405), so the response
body is parsed whenever one exists rather than trusting the status code.
