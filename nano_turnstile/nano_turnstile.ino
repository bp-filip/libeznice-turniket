/*
 * Nano: scan a QR code -> send to ESP32 -> drive the turnstile relays for
 *       the duration the ESP32 specifies, then revert to defaults.
 *
 * The exit direction is always open. ALWAYS_OPEN_OUT below holds that relay
 * energised from boot and ignores anything the ESP32 says about it, so
 * people can leave even if the network, the server or the ESP32 is down.
 *
 * Wiring:
 *   Scanner TX  -> D5             (scanner RX unconnected)
 *   LCD SDA/SCL -> A4/A5
 *   D1 (TX) -> shifter -> ESP32 GPIO14
 *   D0 (RX) <- shifter <- ESP32 GPIO27
 *
 *   D2 open out relay
 *   D3 open in relay
 *   D4 arrow in relay
 *   D10 reserved: SoftwareSerial needs a TX pin it never uses.
 *
 * Relay board is active-LOW. Arrow out is wired permanently on.
 *
 * Behaviour:
 *   ALLOW_SINGLE      entry held 10 s, arrow steady green
 *   ALLOW_MULTI       entry held 5 min, arrow blinking every 500 ms
 *   ALLOW_MULTI_HOUR  entry held 1 hour, arrow steady green
 *   DENY              entry stays shut, arrow stays red
 *
 * A new scan does not cancel an entry in progress: the turnstile stays as
 * it is until the ESP32 answers for the new code.
 *
 * The message stays on screen exactly as long as the entry relay is held;
 * denials and local errors use MESSAGE_HOLD instead.
 *
 * Every line on the ESP32 link, in both directions, ends in "*HH": a CRC-8
 * of everything before the '*'. Lines without a valid checksum are dropped,
 * so this sketch only works with ESP32 firmware 1.0.3 or later.
 *
 * The LCD controller is resynchronised before every redraw, so a glitch
 * from relay noise or a dropped I2C transfer clears at the next message
 * instead of leaving garbage until power-off.
 *
 * Disconnect D0/D1 before uploading. No Serial Monitor - the hardware
 * UART belongs to the ESP32.
 *
 * Scanner settings: RS232 interface, 9600 baud, 8N1, Add CRLF, inverted.
 * Library: "LiquidCrystal I2C" by Frank de Brabander.
 */

#include <SoftwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "czech_lcd.h"

// --- Output pins ---------------------------------------------------------
const uint8_t PIN_OPEN_OUT = 2;
const uint8_t PIN_OPEN_IN  = 3;
const uint8_t PIN_ARROW_IN = 4;

// --- Polarity ------------------------------------------------------------
const uint8_t ACTIVE = LOW;       // active-LOW relay board
const uint8_t IDLE   = HIGH;

// Arrow relay wiring was swapped, so the coil states are inverted here.
// Side effect: with the Nano unpowered or resetting the arrow shows green
// (the entry relay is still shut).
const uint8_t ARROW_YES_STATE = IDLE;    // green
const uint8_t ARROW_NO_STATE  = ACTIVE;  // red

// --- Default states ------------------------------------------------------
const bool ALWAYS_OPEN_OUT  = true;     // exit never closes
const bool DEFAULT_OPEN_IN  = false;
const bool DEFAULT_ARROW_IN = false;    // false = red

const uint16_t BLINK_MS = 500;

// --- Serial / display ----------------------------------------------------
const uint8_t  SCANNER_RX_PIN = 5;
const uint8_t  SCANNER_TX_PIN = 10;     // unused, leave unwired
const uint32_t SCANNER_BAUD   = 9600;
const uint32_t LINK_BAUD      = 9600;

const uint8_t  LCD_ADDR = 0x27;         // try 0x3F if the screen stays blank
const uint32_t I2C_CLOCK = 100000;      // PCF8574 is rated for 100 kHz only
const uint8_t  LCD_COLS = 20;
const uint8_t  LCD_ROWS = 4;

const uint8_t  SCREEN_CHARS  = LCD_COLS * LCD_ROWS;   // 80
const uint8_t  MAX_CODE_LEN  = 80;
const uint16_t LINE_BUF_LEN  = 200;     // UTF-8 needs 2 bytes per accent
const uint16_t REPLY_TIMEOUT = 10000;   // ms to wait for the ESP32
const uint32_t MESSAGE_HOLD  = 15000;   // used when no relay hold is running

// The ESP32 silently drops a repeat of the same code within its
// REPEAT_LOCKOUT_MS (2 s). Drop it here first, with a margin, so the Nano
// never waits for a reply that is not coming and then times out.
const uint16_t REPEAT_LOCKOUT_MS = 2500;

const char IDLE_TEXT[]    = "Prilozte QR kod";
const char BUSY_TEXT[]    = "Overuji...";
const char TIMEOUT_TEXT[] = "Server neodpovida";
const char TOOLONG_TEXT[] = "Kod je prilis dlouhy";

SoftwareSerial scanner(SCANNER_RX_PIN, SCANNER_TX_PIN, true);  // inverted
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

char    codeBuf[MAX_CODE_LEN + 1];
uint8_t codeLen = 0;

char     lineBuf[LINE_BUF_LEN];
uint16_t lineLen = 0;

uint8_t displayBuf[SCREEN_CHARS];

bool     awaitingReply  = false;
uint32_t sentAtMs       = 0;

bool     showingMessage = false;
uint32_t messageUntilMs = 0;

// Relay hold, independent of how long the message stays on screen.
bool     holdActive   = false;
uint32_t holdUntilMs  = 0;
bool     blinkActive  = false;
uint32_t nextBlinkMs  = 0;
bool     blinkOn      = false;

// Last code sent, kept as a hash to save RAM.
bool     haveLastCode = false;
uint32_t lastCodeHash = 0;
uint32_t lastCodeMs   = 0;

void showUtf8(const char *text);
void applyDefaults();
void setArrowIn(bool yes);
void handleLine(const char *line);
void sendToEsp(const char *code);

void setup() {
  pinMode(PIN_OPEN_OUT, OUTPUT);
  pinMode(PIN_OPEN_IN,  OUTPUT);
  pinMode(PIN_ARROW_IN, OUTPUT);
  applyDefaults();

  lcd.init();                           // calls Wire.begin() itself
  Wire.setClock(I2C_CLOCK);             // after init(), which resets it
  lcd.backlight();
  showUtf8(IDLE_TEXT);

  scanner.begin(SCANNER_BAUD);
  Serial.begin(LINK_BAUD);              // hardware UART -> ESP32
}

void loop() {
  readScanner();
  readLink();

  // Blink the arrow while a blinking hold is running.
  if (holdActive && blinkActive && (int32_t)(millis() - nextBlinkMs) >= 0) {
    nextBlinkMs += BLINK_MS;
    blinkOn = !blinkOn;
    setArrowIn(blinkOn);
  }

  // Relay hold expired: back to defaults.
  if (holdActive && (int32_t)(millis() - holdUntilMs) >= 0) {
    holdActive  = false;
    blinkActive = false;
    applyDefaults();
  }

  // The ESP32 never answered.
  if (awaitingReply && (millis() - sentAtMs) > REPLY_TIMEOUT) {
    awaitingReply = false;
    lineLen = 0;
    holdActive  = false;
    blinkActive = false;
    applyDefaults();
    showUtf8(TIMEOUT_TEXT);
    showingMessage = true;
    messageUntilMs = millis() + MESSAGE_HOLD;
  }

  // Message expired: back to the idle prompt.
  if (showingMessage && (int32_t)(millis() - messageUntilMs) >= 0) {
    showingMessage = false;
    showUtf8(IDLE_TEXT);
  }
}

// --- Relay control -------------------------------------------------------
void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? ACTIVE : IDLE);
}

void setArrowIn(bool yes) {
  digitalWrite(PIN_ARROW_IN, yes ? ARROW_YES_STATE : ARROW_NO_STATE);
}

void applyDefaults() {
  setRelay(PIN_OPEN_IN,  DEFAULT_OPEN_IN);
  setRelay(PIN_OPEN_OUT, ALWAYS_OPEN_OUT);
  setArrowIn(DEFAULT_ARROW_IN);
}

// Read "KEY=" from the control line; YES is true, NO is false.
bool parseYesNo(const char *line, const char *key, bool fallback) {
  const char *p = strstr(line, key);
  if (!p) return fallback;
  p += strlen(key);
  if (strncmp(p, "YES", 3) == 0) return true;
  if (strncmp(p, "NO",  2) == 0) return false;
  return fallback;
}

uint32_t parseUint(const char *line, const char *key, uint32_t fallback) {
  const char *p = strstr(line, key);
  if (!p) return fallback;
  p += strlen(key);
  if (*p < '0' || *p > '9') return fallback;
  return strtoul(p, NULL, 10);
}

// CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000;BLINK=NO
void applyControl(const char *line) {
  bool openIn   = parseYesNo(line, "OPEN_IN=",  DEFAULT_OPEN_IN);
  bool arrowIn  = parseYesNo(line, "ARROW_IN=", DEFAULT_ARROW_IN);
  uint32_t hold = parseUint(line, "HOLD=", 0);
  bool blink    = parseYesNo(line, "BLINK=", false);

  setRelay(PIN_OPEN_IN, openIn);
  setArrowIn(arrowIn);

  // OPEN_OUT from the ESP32 is deliberately ignored while ALWAYS_OPEN_OUT
  // is set: the exit must work even when nothing else does.
  if (ALWAYS_OPEN_OUT) {
    setRelay(PIN_OPEN_OUT, true);
  } else {
    setRelay(PIN_OPEN_OUT, parseYesNo(line, "OPEN_OUT=", false));
  }

  if (hold > 0) {
    holdActive  = true;
    holdUntilMs = millis() + hold;
    blinkActive = blink;
    blinkOn     = arrowIn;
    nextBlinkMs = millis() + BLINK_MS;
  } else {
    holdActive  = false;
    blinkActive = false;
  }
}

// --- Link checksum -------------------------------------------------------
// CRC-8, polynomial 0x07. The ESP32 uses the same function.
uint8_t crc8(const char *s, uint16_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= (uint8_t)*s++;
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
  }
  return c;
}

int8_t hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Check and remove a trailing "*HH". Returns false if it is missing or wrong.
bool stripChecksum(char *line, uint16_t len) {
  if (len < 3 || line[len - 3] != '*') return false;
  int8_t hi = hexDigit(line[len - 2]), lo = hexDigit(line[len - 1]);
  if (hi < 0 || lo < 0) return false;
  if (crc8(line, len - 3) != (uint8_t)((hi << 4) | lo)) return false;
  line[len - 3] = '\0';
  return true;
}

// --- Scanner -------------------------------------------------------------
// FNV-1a: enough to tell "same code again" from "different code".
uint32_t hashCode(const char *s) {
  uint32_t h = 2166136261UL;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619UL;
  }
  return h;
}

void readScanner() {
  while (scanner.available()) {
    char c = scanner.read();

    if (c == '\r' || c == '\n') {
      if (codeLen > 0) {
        codeBuf[codeLen] = '\0';
        codeLen = 0;

        // Same code again within the lockout: one scan, not two. The
        // window runs from the last code actually sent, as on the ESP32.
        uint32_t h = hashCode(codeBuf);
        if (haveLastCode && h == lastCodeHash &&
            (millis() - lastCodeMs) < REPEAT_LOCKOUT_MS) {
          continue;
        }
        haveLastCode = true;
        lastCodeHash = h;
        lastCodeMs   = millis();

        sendToEsp(codeBuf);
      }
    } else if (c >= 0x20) {
      if (codeLen < MAX_CODE_LEN) {
        codeBuf[codeLen++] = c;
      } else {
        codeLen = 0;
        showUtf8(TOOLONG_TEXT);
        showingMessage = true;
        messageUntilMs = millis() + MESSAGE_HOLD;
      }
    }
  }
}

void sendToEsp(const char *code) {
  const char HEX_DIGITS[] = "0123456789ABCDEF";
  uint8_t c = crc8(code, strlen(code));
  Serial.print(code);
  Serial.write('*');
  Serial.write(HEX_DIGITS[c >> 4]);
  Serial.write(HEX_DIGITS[c & 0x0F]);
  Serial.println();
  Serial.flush();
  while (Serial.available()) Serial.read();   // discard our own echo

  awaitingReply = true;
  sentAtMs = millis();
  showUtf8(BUSY_TEXT);
  showingMessage = false;               // status, not a message: no hold
}

// --- ESP32 link ----------------------------------------------------------
void readLink() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\r' || c == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        // Corrupted or unchecked lines are dropped, not displayed. A lost
        // result shows as the reply timeout.
        if (stripChecksum(lineBuf, lineLen)) handleLine(lineBuf);
        lineLen = 0;
      }
    } else {
      if (lineLen < LINE_BUF_LEN - 1) {
        lineBuf[lineLen++] = c;         // keep UTF-8 bytes intact
      } else {
        lineLen = 0;
      }
    }
  }
}

void handleLine(const char *line) {
  if (strncmp(line, "CTL:", 4) == 0) {
    applyControl(line + 4);
  } else if (strncmp(line, "MSG:", 4) == 0) {
    awaitingReply = false;
    showUtf8(line + 4);
    showingMessage = true;

    // Match the message to the entry hold; fall back to MESSAGE_HOLD when
    // nothing is being held open (denials, errors).
    messageUntilMs = holdActive ? holdUntilMs : (millis() + MESSAGE_HOLD);
  }
  // Anything else is not our protocol - ignore it rather than display it.
}

// --- Display -------------------------------------------------------------
// The backpack drives the HD44780 four bits at a time. If one transfer is
// lost (relay noise, a brown-out of the LCD alone), the controller reads
// every later byte half a byte off and shows garbage until re-initialised.
//
// Nibbles 3, 3, 3, 2 bring it back to 4-bit mode from any state: synced,
// half a byte off, or reset to 8-bit. Written straight to the PCF8574
// because the library's command() is not callable from outside it.
// Pin mapping as in LiquidCrystal_I2C: P0 RS, P2 EN, P3 backlight, P4-7 data.
const uint8_t LCD_EN = 0x04;
const uint8_t LCD_BL = 0x08;

void lcdNibble(uint8_t nibble) {
  uint8_t v = (nibble << 4) | LCD_BL;   // RS = 0: command
  Wire.beginTransmission(LCD_ADDR);
  Wire.write(v);                        // RS settles before EN rises
  Wire.write(v | LCD_EN);
  Wire.write(v);
  Wire.endTransmission();
  delayMicroseconds(50);
}

void lcdCommand(uint8_t cmd) {
  lcdNibble(cmd >> 4);
  lcdNibble(cmd & 0x0F);
}

void lcdResync() {
  lcdNibble(0x3); delay(5);
  lcdNibble(0x3); delay(1);
  lcdNibble(0x3); delay(1);
  lcdNibble(0x2); delay(1);
  lcdCommand(0x28);                     // 4-bit, 2 lines, 5x8 font
  lcdCommand(0x0C);                     // display on, no cursor, no blink
  lcdCommand(0x06);                     // increment, no shift
}

// Resync, decode UTF-8, load any accented letters into CGRAM, then write
// four rows. Text arrives from the ESP32 already wrapped and padded to 20
// columns. About 10 ms of the time is the resync.
void showUtf8(const char *text) {
  lcdResync();                          // before CGRAM is loaded
  uint8_t len = czechPrepare(lcd, text, displayBuf, SCREEN_CHARS);

  for (uint8_t row = 0; row < LCD_ROWS; row++) {
    lcd.setCursor(0, row);
    for (uint8_t col = 0; col < LCD_COLS; col++) {
      uint8_t i = row * LCD_COLS + col;
      lcd.write(i < len ? displayBuf[i] : ' ');
    }
  }
}
