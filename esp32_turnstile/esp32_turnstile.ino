/*
 * ESP32: QR validation against the turnstile API
 *
 * Flow: Nano scans a code -> ESP32 POSTs it to validator.php -> ESP32 sends
 * the Nano a control line (relay states, hold time) and a message line
 * (LCD text, pre-wrapped to 20-character rows).
 *
 * Protocol to the Nano, one line each, newline terminated:
 *   CTL:OPEN_IN=YES;OPEN_OUT=NO;ARROW_IN=YES;HOLD=10000;BLINK=NO
 *   MSG:Vstupenka je platna.
 *
 * Every line in both directions ends in "*HH": a CRC-8 of everything before
 * the '*', as two uppercase hex digits. Lines that fail the check are
 * dropped on both sides. The Nano requires it, so both boards must run
 * firmware from the same release.
 *
 * HOLD is milliseconds; the Nano reverts to its defaults afterwards.
 * BLINK=YES makes the Nano blink the arrow for the duration of the hold.
 *
 * The Nano writes the screen only at boot and when it receives a MSG:
 * line; everything else on the LCD comes from here. SEND_LCD_MESSAGES
 * below turns all MSG: lines off: the relays keep working on CTL: lines
 * alone and the Nano's boot text stays on screen.
 *
 * Server messages pass through byte for byte. Nothing is added or removed
 * except line breaks and padding for the 20x4 screen.
 *
 * Requires the ArduinoJson library (Library Manager, by Benoit Blanchon).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "turnstile.h"
#include "secrets.h"

// --- Version -------------------------------------------------------------
#define FIRMWARE_VERSION "1.0.3"

// --- WiFi ----------------------------------------------------------------
// Values live in secrets.h, which is gitignored. Copy secrets.h.example
// to secrets.h and fill it in.
const char *WIFI_SSID = SECRET_WIFI_SSID;
const char *WIFI_PASS = SECRET_WIFI_PASS;

// --- Validator API -------------------------------------------------------
const char *API_URL    = "https://rezervacekluziste.cz/logic/turniket_api/validator.php";
const char *API_HEADER = "Hash";
const char *API_TOKEN  = SECRET_API_TOKEN;

// Each phase of the request has its own limit. HTTPClient's setTimeout()
// covers only waiting for the reply; connecting and the TLS handshake are
// separate, and the handshake defaults to 120 seconds.
const uint16_t API_CONNECT_TIMEOUT_MS  = 3000;
const uint8_t  API_HANDSHAKE_TIMEOUT_S = 4;     // seconds, not ms
const uint16_t API_TIMEOUT_MS          = 4000;  // waiting for the reply

// The phases can still add up past the Nano's 10 s REPLY_TIMEOUT. A result
// later than this is replaced by a denial, so a ticket never opens the
// turnstile after its holder has given up and walked away.
const uint16_t RESULT_DEADLINE_MS = 9000;

// Ignore a repeat of the same code within this window. Stops a stuttering
// scanner or a reflected byte stream from hammering the server.
const uint32_t REPEAT_LOCKOUT_MS = 2000;

// --- LCD messages --------------------------------------------------------
// true:  results, the idle prompt and the "checking" text go to the LCD.
// false: nothing is ever sent to the LCD. Turnstile and arrow work exactly
//        the same; the screen keeps showing the Nano's boot text.
const bool SEND_LCD_MESSAGES = true;

// --- Local status messages ----------------------------------------------
// The only strings this firmware writes itself; everything else comes from
// the server verbatim.
const char *MSG_IDLE       = "Přiložte QR kód";
const char *MSG_BUSY       = "Ověřuji...";
const char *MSG_NO_WIFI    = "Není připojení k síti";
const char *MSG_NO_CONNECT = "Chyba připojení";
const char *MSG_NO_SERVER  = "Server nedostupný";
const char *MSG_BAD_JSON   = "Chybná odpověď serveru";
const char *MSG_EMPTY      = "Bez zprávy";
const char *MSG_TOO_SLOW   = "Server neodpovídá";

// Leave the Nano's boot text up this long before the first idle prompt.
// Also gives the Nano time to come out of its bootloader.
const uint32_t BOOT_TEXT_MS = 5000;

// --- Display -------------------------------------------------------------
const uint8_t LCD_COLS = 20;
const uint8_t LCD_ROWS = 4;
const size_t  MAX_DISPLAY_BYTES = 200;   // trimmed before wrapping

// --- Hold times ----------------------------------------------------------
const uint32_t HOLD_SINGLE_MS = 10UL * 1000UL;         // 10 seconds
const uint32_t HOLD_MULTI_MS  = 5UL * 60UL * 1000UL;   // 5 minutes
const uint32_t HOLD_MULTI_HOUR_MS = 60UL * 60UL * 1000UL;   // 1 hour

// How long a result stays on screen before the idle prompt returns: as
// long as the entry is held, or this long when nothing is held (denials,
// errors).
const uint32_t RESULT_SHOW_MS = 15000;

// --- GitHub --------------------------------------------------------------
#define GH_OWNER "bp-filip"
#define GH_REPO  "libeznice-turniket"

const char *VERSION_URL  = "https://github.com/" GH_OWNER "/" GH_REPO
                           "/releases/latest/download/version.txt";
const char *FIRMWARE_URL = "https://github.com/" GH_OWNER "/" GH_REPO
                           "/releases/latest/download/firmware.bin";

const uint32_t CHECK_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

// --- OTA -----------------------------------------------------------------
const char *OTA_HOSTNAME = "esp32-scanner";
const char *OTA_PASSWORD = SECRET_OTA_PASSWORD;

// --- Nano link -----------------------------------------------------------
const uint32_t LINK_BAUD   = 9600;
const int      LINK_RX_PIN = 14;
const int      LINK_TX_PIN = 27;

const size_t MAX_MSG_LEN = 128;
char   msgBuf[MAX_MSG_LEN + 1];
size_t msgLen = 0;

static bool     otaStarted   = false;
static volatile bool wifiUp  = false;   // set by the WiFi event task
static uint32_t lastCheckMs  = 0;
static bool     checkedOnce  = false;

static String   lastCode;
static uint32_t lastCodeMs = 0;

// Touched only by linkTask: everything that goes to the Nano is sent from
// that one task, so lines cannot interleave and no other task drains RX.
static uint32_t busyUntilMs = BOOT_TEXT_MS;   // no idle prompt before then
static bool     idlePending = true;           // idle prompt due once quiet

void handleMessage(const char *msg);

// --- Action -> relay states ----------------------------------------------
// OPEN_OUT is always NO here: the Nano holds the exit relay open on its own
// so people can always leave, whatever the server says.
//
// ALLOW_MULTI is untested - only ALLOW_SINGLE is in use for now. It differs
// in how long the entry is held and in the blinking arrow. ALLOW_MULTI_HOUR
// is a one-hour hold with a steady green arrow.
Control controlForAction(const char *action) {
  if (strcmp(action, "ALLOW_SINGLE") == 0) {
    return { "YES", "NO", "YES", HOLD_SINGLE_MS, "NO" };
  }
  if (strcmp(action, "ALLOW_MULTI") == 0) {
    return { "YES", "NO", "YES", HOLD_MULTI_MS, "YES" };
  }
  if (strcmp(action, "ALLOW_MULTI_HOUR") == 0) {
    return { "YES", "NO", "YES", HOLD_MULTI_HOUR_MS, "NO" };
  }
  return { "NO", "NO", "NO", 0, "NO" };      // DENY and anything unknown
}

// --- Word wrapping -------------------------------------------------------
// Widths are counted in screen cells, not bytes: a UTF-8 accented letter is
// two bytes but one cell, and the Nano renders d-caron / t-caron as d' / t',
// which take two cells.

static uint16_t displayWidth(const String &s) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < s.length(); i++) {
    uint8_t b = s[i];
    if ((b & 0xC0) == 0x80) continue;      // continuation byte
    n++;
    if (i + 1 < s.length()) {
      uint8_t b2 = s[i + 1];
      if ((b == 0xC4 && (b2 == 0x8F || b2 == 0x8E)) ||
          (b == 0xC5 && (b2 == 0xA5 || b2 == 0xA4))) n++;
    }
  }
  return n;
}

// Pull the first n display cells off s, leaving the remainder behind.
static String takeChars(String &s, uint16_t n) {
  uint16_t count = 0, i = 0;
  while (i < s.length() && count < n) {
    uint8_t b = s[i];
    uint16_t adv = 1;
    if ((b & 0xF0) == 0xE0)      adv = 3;
    else if ((b & 0xE0) == 0xC0) adv = 2;

    uint16_t w = 1;
    if (adv == 2 && i + 1 < s.length()) {
      uint8_t b2 = s[i + 1];
      if ((b == 0xC4 && (b2 == 0x8F || b2 == 0x8E)) ||
          (b == 0xC5 && (b2 == 0xA5 || b2 == 0xA4))) w = 2;
    }
    if (count + w > n) break;
    count += w;
    i += adv;
  }
  String head = s.substring(0, i);
  s = s.substring(i);
  return head;
}

static String padLine(const String &s) {
  String r = s;
  for (uint16_t w = displayWidth(s); w < LCD_COLS; w++) r += ' ';
  return r;
}

// Break on spaces so words stay whole; pad every row to full width so the
// Nano's row-filling loop lands each line where we want it. Messages that
// do not fill the screen start on the second row.
String wrapWords(const String &text) {
  String out, line;
  uint8_t  rows = 0;
  uint16_t pos  = 0;

  while (pos <= text.length() && rows < LCD_ROWS) {
    int sp = text.indexOf(' ', pos);
    String word = (sp < 0) ? text.substring(pos) : text.substring(pos, sp);
    pos = (sp < 0) ? text.length() + 1 : sp + 1;
    if (word.length() == 0) continue;

    // A word too long for one row has to be broken somewhere.
    while (displayWidth(word) > LCD_COLS && rows < LCD_ROWS) {
      if (line.length()) { out += padLine(line); line = ""; rows++; }
      if (rows >= LCD_ROWS) break;
      out += padLine(takeChars(word, LCD_COLS));
      rows++;
    }
    if (word.length() == 0 || rows >= LCD_ROWS) continue;

    uint16_t need = displayWidth(word) + (line.length() ? 1 : 0);
    if (displayWidth(line) + need > LCD_COLS) {
      out += padLine(line);
      line = "";
      rows++;
      if (rows >= LCD_ROWS) break;
    }
    if (line.length()) line += ' ';
    line += word;
  }

  if (rows < LCD_ROWS && line.length()) { out += padLine(line); rows++; }

  // Short messages sit one row down so they look centred rather than
  // pinned to the top of the screen.
  if (rows > 0 && rows < LCD_ROWS) out = padLine("") + out;

  return out;
}

// Strip line breaks and trim overlong text without splitting a UTF-8
// sequence. The server's own characters are otherwise left alone.
String tidyMessage(String text) {
  text.replace("\r", " ");
  text.replace("\n", " ");

  if (text.length() > MAX_DISPLAY_BYTES) {
    size_t cut = MAX_DISPLAY_BYTES;
    while (cut > 0 && ((uint8_t)text[cut] & 0xC0) == 0x80) cut--;
    text = text.substring(0, cut);
  }
  return text;
}

// --- Link checksum -------------------------------------------------------
// CRC-8, polynomial 0x07. The Nano uses the same function.
static uint8_t crc8(const char *s, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= (uint8_t)*s++;
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
  }
  return c;
}

static int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Check and remove a trailing "*HH". Returns false if it is missing or wrong.
static bool stripChecksum(char *line) {
  size_t len = strlen(line);
  if (len < 3 || line[len - 3] != '*') return false;
  int hi = hexDigit(line[len - 2]), lo = hexDigit(line[len - 1]);
  if (hi < 0 || lo < 0) return false;
  if (crc8(line, len - 3) != (uint8_t)((hi << 4) | lo)) return false;
  line[len - 3] = '\0';
  return true;
}

// Write one line to the Nano with its checksum appended.
static void sendLine(const String &line) {
  char tail[4];
  snprintf(tail, sizeof(tail), "*%02X", crc8(line.c_str(), line.length()));
  Serial2.print(line);
  Serial2.println(tail);
}

void sendMessageOnly(const String &text);

// --- Nano link task, pinned to core 0 ------------------------------------
// Stack is generous because the TLS handshake for the API call runs here.
// This is the only task that reads or writes Serial2.
void linkTask(void *pv) {
  for (;;) {
    while (Serial2.available()) {
      char c = Serial2.read();

      if (c == '\r' || c == '\n') {
        if (msgLen > 0) {
          msgBuf[msgLen] = '\0';
          // A corrupted line is dropped silently. Answering it could start
          // a loop on the crosstalking link; the Nano times out instead.
          if (stripChecksum(msgBuf)) {
            handleMessage(msgBuf);
          } else {
            Serial.printf("! bad checksum: %s\n", msgBuf);
          }
          msgLen = 0;
        }
      } else if ((uint8_t)c >= 0x20) {
        if (msgLen < MAX_MSG_LEN) {
          msgBuf[msgLen++] = c;
        } else {
          msgLen = 0;
          Serial.println("! message too long");
        }
      }
    }

    // Put the idle prompt up once the result has had its time. Sent once
    // per result, not repeated: the Nano keeps whatever it was last sent.
    if (SEND_LCD_MESSAGES && idlePending &&
        (int32_t)(millis() - busyUntilMs) >= 0) {
      idlePending = false;
      sendMessageOnly(MSG_IDLE);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Send display text with no control line attached.
void sendMessageOnly(const String &text) {
  if (!SEND_LCD_MESSAGES) return;
  sendLine("MSG:" + wrapWords(text));
  Serial2.flush();
  while (Serial2.available()) Serial2.read();   // discard our own echo
}

void sendToNano(const Control &ctl, const String &message) {
  String line = "CTL:OPEN_IN=";
  line += ctl.openIn;
  line += ";OPEN_OUT="; line += ctl.openOut;
  line += ";ARROW_IN="; line += ctl.arrowIn;
  line += ";HOLD=";     line += ctl.holdMs;
  line += ";BLINK=";    line += ctl.blink;

  // Drop anything that arrived while we were transmitting: on a crosstalking
  // link that is our own output coming back.
  sendLine(line);
  if (SEND_LCD_MESSAGES) sendLine("MSG:" + wrapWords(message));
  Serial2.flush();
  while (Serial2.available()) Serial2.read();

  // Leave the result on screen, then put the idle prompt back once.
  busyUntilMs = millis() + (ctl.holdMs > 0 ? ctl.holdMs : RESULT_SHOW_MS);
  idlePending = true;

  Serial.println(line);
  Serial.print(SEND_LCD_MESSAGES ? "MSG:" : "MSG (not sent):");
  Serial.println(message);
}

// POST the scanned code and act on the response.
void validateCode(const char *code) {
  uint32_t startedMs = millis();

  if (!wifiUp) {
    sendToNano(controlForAction("DENY"), MSG_NO_WIFI);
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();          // no cert validation - trusted LAN only
  client.setHandshakeTimeout(API_HANDSHAKE_TIMEOUT_S);

  HTTPClient http;
  http.setConnectTimeout(API_CONNECT_TIMEOUT_MS);
  http.setTimeout(API_TIMEOUT_MS);

  if (!http.begin(client, API_URL)) {
    sendToNano(controlForAction("DENY"), MSG_NO_CONNECT);
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader(API_HEADER, API_TOKEN);

  JsonDocument reqDoc;
  reqDoc["qr_code"] = code;
  String body;
  serializeJson(reqDoc, body);

  int status = http.POST(body);

  // The API returns JSON on error codes too (400, 401, 405), so parse the
  // body whenever there is one rather than trusting the status code alone.
  if (status <= 0) {
    Serial.printf("api: transport error %d\n", status);
    http.end();
    sendToNano(controlForAction("DENY"), MSG_NO_SERVER);
    return;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("api: HTTP %d %s\n", status, payload.c_str());

  uint32_t tookMs = millis() - startedMs;
  if (tookMs > RESULT_DEADLINE_MS) {
    Serial.printf("api: answer took %u ms, too late - denying\n", (unsigned)tookMs);
    sendToNano(controlForAction("DENY"), MSG_TOO_SLOW);
    return;
  }

  JsonDocument resDoc;
  if (deserializeJson(resDoc, payload)) {
    sendToNano(controlForAction("DENY"), MSG_BAD_JSON);
    return;
  }

  const char *action = resDoc["action"] | "DENY";
  const char *state  = resDoc["status"] | "";
  String message     = resDoc["message"] | "";

  Serial.printf("api: status=%s action=%s\n", state, action);

  message = tidyMessage(message);
  if (message.length() == 0) message = MSG_EMPTY;

  sendToNano(controlForAction(action), message);
}

// Anything arriving from the Nano that is not a plausible QR code gets
// dropped without touching the network.
void handleMessage(const char *msg) {
  // Our own protocol coming back at us - a reflected or crosstalking link.
  if (strncmp(msg, "CTL:", 4) == 0 || strncmp(msg, "MSG:", 4) == 0 ||
      strstr(msg, "OPEN_IN=") || strstr(msg, "ARROW_IN=") ||
      strstr(msg, "HOLD=") || strstr(msg, "BLINK=")) {
    Serial.println("! ignoring echoed protocol line");
    return;
  }

  // Codes are printable ASCII; a run of high bytes means line noise.
  for (const char *p = msg; *p; p++) {
    if ((uint8_t)*p < 0x20 || (uint8_t)*p > 0x7E) {
      Serial.println("! ignoring noisy line");
      return;
    }
  }

  if (strlen(msg) < 4) {
    Serial.println("! ignoring short line");
    return;
  }

  // Same code twice in quick succession: one scan, not two.
  uint32_t now = millis();
  if (lastCode == msg && (now - lastCodeMs) < REPEAT_LOCKOUT_MS) {
    Serial.println("! ignoring repeat scan");
    return;
  }
  lastCode   = msg;
  lastCodeMs = now;

  Serial.print("from Nano: ");
  Serial.println(msg);

  // "Checking" text while the request runs. The result replaces it, and
  // sendToNano() sets the real quiet period.
  busyUntilMs = millis() + RESULT_DEADLINE_MS + RESULT_SHOW_MS;
  sendMessageOnly(MSG_BUSY);

  validateCode(msg);
}

// --- GitHub update -------------------------------------------------------
bool fetchLatestVersion(String &out) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  if (!http.begin(client, VERSION_URL)) {
    Serial.println("update: cannot start request");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("update: version fetch failed, HTTP %d\n", code);
    http.end();
    return false;
  }

  out = http.getString();
  out.trim();
  http.end();
  return out.length() > 0;
}

void checkForUpdate() {
  lastCheckMs = millis();
  checkedOnce = true;

  String latest;
  if (!fetchLatestVersion(latest)) return;

  Serial.printf("update: running %s, latest %s\n", FIRMWARE_VERSION, latest.c_str());
  if (latest == FIRMWARE_VERSION) {
    Serial.println("update: already current");
    return;
  }

  Serial.println("update: downloading new firmware");

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return result = httpUpdate.update(client, FIRMWARE_URL);

  switch (result) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("update: failed (%d) %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("update: server reported nothing new");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("update: installed, rebooting");
      break;
  }
}

// --- Local OTA -----------------------------------------------------------
void startOta() {
  if (otaStarted) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]()   { Serial.println("OTA start"); });
  ArduinoOTA.onEnd([]()     { Serial.println("\nOTA done, rebooting"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("OTA %u%%\r", (done * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error %u\n", error);
  });

  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("OTA ready at %s.local\n", OTA_HOSTNAME);
}

void onNetworkEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi associated");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi got IP: ");
      Serial.println(WiFi.localIP());
      wifiUp = true;
      startOta();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi disconnected, retrying");
      wifiUp = false;
      WiFi.reconnect();
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nESP32 starting, firmware %s\n", FIRMWARE_VERSION);

  Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

  WiFi.onEvent(onNetworkEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(linkTask, "link", 16384, NULL, 1, NULL, 0);
}

void loop() {
  if (otaStarted) {
    ArduinoOTA.handle();
  }

  if (wifiUp) {
    bool firstCheck = !checkedOnce;
    bool dueAgain   = checkedOnce && (millis() - lastCheckMs) > CHECK_INTERVAL_MS;
    if (firstCheck || dueAgain) {
      checkForUpdate();
    }
  }

  // The idle prompt is sent from linkTask, which owns the Nano link.

  delay(10);
}
