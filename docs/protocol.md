# Serial protocol

9600 baud, 8N1, newline-terminated lines, between Nano D0/D1 and ESP32
GPIO14/27.

## Nano to ESP32

The raw scanned code, one line:

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
CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000
```

| Field | Values | Meaning |
|-------|--------|---------|
| `OPEN_IN` | `YES` / `NO` | entry relay |
| `OPEN_OUT` | `YES` / `NO` | **ignored** while `ALWAYS_OPEN_OUT` is set |
| `ARROW_IN` | `YES` / `NO` | green / red |
| `HOLD` | milliseconds | how long to hold before reverting to defaults |

Parsing is by substring search, so field order does not matter and unknown
fields are ignored. A missing field falls back to its default.

`HOLD=0` means no timed hold - used for denials, where nothing needs
reverting.

### Message

```
MSG:Vstupenka je platna.
```

UTF-8, already wrapped and padded by the ESP32 to exactly 20 characters per
row so the Nano's row loop breaks lines correctly. Messages shorter than
four rows are padded with a leading blank row so they start on row 2.

A `MSG:` with no preceding `CTL:` just updates the display - used for the
idle prompt and the "checking" text.

## Timing

| Constant | Where | Value | Purpose |
|----------|-------|-------|---------|
| `API_TIMEOUT_MS` | ESP32 | 4000 | must stay below the Nano's `REPLY_TIMEOUT` |
| `REPLY_TIMEOUT` | Nano | 10000 | gives up and shows "Server neodpovídá" |
| `REPEAT_LOCKOUT_MS` | ESP32 | 2000 | same code twice in a row is one scan |
| `IDLE_REFRESH_MS` | ESP32 | 12000 | must stay below the Nano's `MESSAGE_HOLD` |
| `MESSAGE_HOLD` | Nano | 15000 | how long a message stays without a relay hold |
| `HOLD_SINGLE_MS` | ESP32 | 10000 | entry hold for a single ticket |
| `HOLD_MULTI_MS` | ESP32 | 300000 | entry hold for a group ticket, untested |

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

`action` is one of `ALLOW_SINGLE`, `ALLOW_MULTI`, `DENY`. Anything
unrecognised is treated as `DENY`.

The API returns JSON on error codes too (400, 401, 405), so the response
body is parsed whenever one exists rather than trusting the status code.
