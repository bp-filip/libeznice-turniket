/*
 * Nano: scan a QR code -> send to ESP32 -> drive the turnstile relays for
 *       the duration the ESP32 specifies, then revert to defaults.
 *
 * A new scan cancels whatever entry is in progress: the turnstile closes
 * and the arrow goes red before the new code is sent for validation.
 *
 * The exit direction is always open. ALWAYS_OPEN_OUT holds that relay
 * energised from boot and ignores anything the ESP32 says about it, so
 * people can leave even if the network, the server or the ESP32 is down.
 *
 * Wiring:
 *   Scanner TX  -> D5             (scanner RX unconnected)
 *   LCD SDA/SCL -> A4/A5
 *   D1 (TX) -> shifter -> ESP32 GPIO14
 *   D0 (RX) <- shifter <- ESP32 GPIO27
 *
 *   D2 arrow in relay
 *   D3 open in relay
 *   D4 open out relay
 *   D10 reserved: SoftwareSerial needs a TX pin it never uses.
 *
 * Relay board is active-LOW. Arrow out is wired permanently on.
 *
 * Behaviour:
 *   ALLOW_SINGLE  entry held 10 s, arrow green
 *   DENY          entry stays shut, arrow stays red
 *
 * The message stays on screen exactly as long as the entry relay is held;
 * denials and local errors use MESSAGE_HOLD instead.
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
const uint8_t PIN_ARROW_IN = 2;
const uint8_t PIN_OPEN_IN  = 3;
const uint8_t PIN_OPEN_OUT = 4;

// --- Polarity ------------------------------------------------------------
const uint8_t ACTIVE = LOW;       // active-LOW relay board
const uint8_t IDLE   = HIGH;

// Swap these two if the arrow shows the wrong colour.
const uint8_t ARROW_YES_STATE = ACTIVE;  // green
const uint8_t ARROW_NO_STATE  = IDLE;    // red

// --- Default states ------------------------------------------------------
const bool ALWAYS_OPEN_OUT  = true;     // exit never closes
const bool DEFAULT_OPEN_IN  = false;
const bool DEFAULT_ARROW_IN = false;    // false = red

// --- Serial / display ----------------------------------------------------
const uint8_t  SCANNER_RX_PIN = 5;
const uint8_t  SCANNER_TX_PIN = 10;     // unused, leave unwired
const uint32_t SCANNER_BAUD   = 9600;
const uint32_t LINK_BAUD      = 9600;

const uint8_t  LCD_ADDR = 0x27;         // try 0x3F if the screen stays blank
const uint8_t  LCD_COLS = 20;
const uint8_t  LCD_ROWS = 4;

const uint8_t  SCREEN_CHARS  = LCD_COLS * LCD_ROWS;   // 80
const uint8_t  MAX_CODE_LEN  = 80;
const uint16_t LINE_BUF_LEN  = 200;     // UTF-8 needs 2 bytes per accent
const uint16_t REPLY_TIMEOUT = 10000;   // ms to wait for the ESP32
const uint32_t MESSAGE_HOLD  = 15000;   // used when no relay hold is running

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
bool     holdActive  = false;
uint32_t holdUntilMs = 0;

void showUtf8(const char *text);
void applyDefaults();
void setArrowIn(bool yes);
void handleLine(const char *line);
void sendToEsp(const char *code);

void setup() {
  pinMode(PIN_ARROW_IN, OUTPUT);
  pinMode(PIN_OPEN_IN,  OUTPUT);
  pinMode(PIN_OPEN_OUT, OUTPUT);
  applyDefaults();

  Wire.begin();
  Wire.setClock(400000);
  lcd.init();
  lcd.backlight();
  showUtf8(IDLE_TEXT);

  scanner.begin(SCANNER_BAUD);
  Serial.begin(LINK_BAUD);              // hardware UART -> ESP32
}

void loop() {
  readScanner();
  readLink();

  // Relay hold expired: back to defaults.
  if (holdActive && (int32_t)(millis() - holdUntilMs) >= 0) {
    holdActive = false;
    applyDefaults();
  }

  // The ESP32 never answered.
  if (awaitingReply && (millis() - sentAtMs) > REPLY_TIMEOUT) {
    awaitingReply = false;
    lineLen = 0;
    holdActive = false;
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

// CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000
void applyControl(const char *line) {
  bool openIn   = parseYesNo(line, "OPEN_IN=",  DEFAULT_OPEN_IN);
  bool arrowIn  = parseYesNo(line, "ARROW_IN=", DEFAULT_ARROW_IN);
  uint32_t hold = parseUint(line, "HOLD=", 0);

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
  } else {
    holdActive  = false;
  }
}

// --- Scanner -------------------------------------------------------------
void readScanner() {
  while (scanner.available()) {
    char c = scanner.read();

    if (c == '\r' || c == '\n') {
      if (codeLen > 0) {
        codeBuf[codeLen] = '\0';
        sendToEsp(codeBuf);
        codeLen = 0;
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
  // A new scan cancels any entry in progress: close up and start fresh.
  holdActive = false;
  applyDefaults();

  Serial.println(code);
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
        handleLine(lineBuf);
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
// Decode UTF-8, load any accented letters into CGRAM, then write four rows.
// Text arrives from the ESP32 already wrapped and padded to 20 columns.
void showUtf8(const char *text) {
  uint8_t len = czechPrepare(lcd, text, displayBuf, SCREEN_CHARS);

  for (uint8_t row = 0; row < LCD_ROWS; row++) {
    lcd.setCursor(0, row);
    for (uint8_t col = 0; col < LCD_COLS; col++) {
      uint8_t i = row * LCD_COLS + col;
      lcd.write(i < len ? displayBuf[i] : ' ');
    }
  }
}
