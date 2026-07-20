#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>

// ───── WIFI ─────
const char* ssid     = "PUT SSID";
const char* password = "PUT PASSWORD";

// ───── SERVER ─────
const char* server_ip  = "192.168.1.110"; // CHANGE THIS
const int   server_port = 8081;

// ───── PLAYER ID ─────
// Change this for each ESP32: "P1", "P2", "P3", or "P4"
const char* playerID = "P1";

// ───── PINS ─────
#define BUTTON_PIN 3   // button switch → pin 3 → GND (INPUT_PULLUP)
#define LED_PIN    10  // button's integrated LED via transistor

// ───── OLED (hardware I2C, SDA=5, SCL=6) ─────
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

// Animation center — proportionally scaled for 72x40 screen
const int cx = 36;
const int cy = 26;

// ───── WEBSOCKET ─────
WebSocketsClient webSocket;

// ───── BUTTON STATE ─────
bool lastButtonState = HIGH;
unsigned long lastPressTime = 0;

// ───── TASK 18 — RECONNECTION STATE ─────
unsigned long lastWifiCheckTime = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000; // check every 5s
bool wifiWasConnected = true; // assume connected once setup() finishes

// ───── OLED HELPERS ─────
void showMsg(const char* a, const char* b = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 15, a);
  u8g2.drawStr(2, 28, b);
  u8g2.sendBuffer();
}

void showIdleScreen() {
  showMsg("Waiting...", playerID);
}

void animHeader(const char* label) {
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 7, label);
}

// ───── TASK 18 — WIFI RECONNECTION ─────
// Non-blocking check, run from loop() every WIFI_CHECK_INTERVAL ms.
// If WiFi has dropped, kicks off a reconnect without freezing the
// rest of the sketch (button reads, webSocket.loop(), etc. keep going).
void checkWifiConnection() {
  unsigned long now = millis();
  if (now - lastWifiCheckTime < WIFI_CHECK_INTERVAL) return;
  lastWifiCheckTime = now;

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      // Just dropped — log/show once, don't spam every check.
      Serial.println("WiFi lost. Attempting reconnect...");
      showMsg("WiFi Lost", "Reconnecting");
      digitalWrite(LED_PIN, LOW); // safety: don't leave button LED stuck on
      wifiWasConnected = false;
    }
    WiFi.reconnect();
  } else {
    if (!wifiWasConnected) {
      Serial.println("WiFi restored.");
      showMsg("WiFi OK", "Resuming");
      delay(500);
      showIdleScreen();
      wifiWasConnected = true;
      // webSocket auto-reconnects on its own retry interval, and
      // WStype_CONNECTED re-sends HELLO so the server re-registers
      // this player automatically — no extra code needed here.
    }
  }
}

// ───── TASK 15 — RESULT ANIMATIONS ─────

void playCorrectAnimation() {
  const char* label = "CORRECT!";

  // Expanding ring
  for (int r = 2; r <= 10; r += 2) {
    u8g2.clearBuffer();
    animHeader(label);
    u8g2.drawCircle(cx, cy, r);
    u8g2.sendBuffer();
    delay(45);
  }

  // Checkmark — short leg then long leg
  int x1 = cx - 6, y1 = cy;
  int x2 = cx - 1, y2 = cy + 4;
  int x3 = cx + 6, y3 = cy - 5;

  for (int i = 0; i <= 8; i++) {
    u8g2.clearBuffer();
    animHeader(label);
    u8g2.drawCircle(cx, cy, 10);
    int mx = x1 + (x2 - x1) * i / 8;
    int my = y1 + (y2 - y1) * i / 8;
    u8g2.drawLine(x1, y1, mx, my);
    u8g2.sendBuffer();
    delay(30);
  }
  for (int i = 0; i <= 8; i++) {
    u8g2.clearBuffer();
    animHeader(label);
    u8g2.drawCircle(cx, cy, 10);
    u8g2.drawLine(x1, y1, x2, y2);
    int mx = x2 + (x3 - x2) * i / 8;
    int my = y2 + (y3 - y2) * i / 8;
    u8g2.drawLine(x2, y2, mx, my);
    u8g2.sendBuffer();
    delay(30);
  }

  delay(900); // hold finished checkmark
}

void playWrongAnimation() {
  const char* label = "WRONG";
  int size = 8;
  int ax1 = cx - size, ay1 = cy - size;
  int ax2 = cx + size, ay2 = cy + size;
  int bx1 = cx + size, by1 = cy - size;
  int bx2 = cx - size, by2 = cy + size;

  for (int i = 0; i <= 8; i++) {
    u8g2.clearBuffer();
    animHeader(label);
    int mx = ax1 + (ax2 - ax1) * i / 8;
    int my = ay1 + (ay2 - ay1) * i / 8;
    u8g2.drawLine(ax1, ay1, mx, my);
    u8g2.sendBuffer();
    delay(30);
  }
  for (int i = 0; i <= 8; i++) {
    u8g2.clearBuffer();
    animHeader(label);
    u8g2.drawLine(ax1, ay1, ax2, ay2);
    int mx = bx1 + (bx2 - bx1) * i / 8;
    int my = by1 + (by2 - by1) * i / 8;
    u8g2.drawLine(bx1, by1, mx, my);
    u8g2.sendBuffer();
    delay(30);
  }

  delay(900); // hold finished X
}

// ───── WEBSOCKET EVENTS ─────
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.println("WebSocket Connected");
      showMsg("WS Connected", "HELLO");
      webSocket.sendTXT(
        String("{\"type\":\"HELLO\",\"role\":\"PLAYER\",\"player\":\"") +
        playerID +
        "\"}"
      );
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      Serial.println(msg);

      if (msg.indexOf("\"type\":\"QUESTION\"") >= 0) {
        // Task 13: show category on OLED
        int idx = msg.indexOf("\"category\":\"");
        if (idx >= 0) {
          int start = idx + 12;
          int end   = msg.indexOf("\"", start);
          String cat = msg.substring(start, end);
          showMsg("Category:", cat.c_str());
        }

      } else if (msg.indexOf("\"type\":\"BUZZED_IN\"") >= 0) {
        // Task 14: this player won the buzzer
        digitalWrite(LED_PIN, HIGH); // keep LED on persistently
        showMsg("YOUR TURN", "Answer!");

      } else if (msg.indexOf("\"type\":\"LOCKED\"") >= 0) {
        // Task 14: another player buzzed in — lock this one out
        digitalWrite(LED_PIN, LOW);
        showMsg("LOCKED", "");

      } else if (msg.indexOf("\"type\":\"RESULT\"") >= 0) {
        // Task 15: this player buzzed in and the admin selected an answer
        bool isCorrect = (msg.indexOf("\"correct\":true") >= 0);
        if (isCorrect) {
          playCorrectAnimation();
        } else {
          playWrongAnimation();
        }
        // Screen resets when ROUND_OVER or STATE:IDLE arrives next

      } else if (msg.indexOf("\"type\":\"ROUND_OVER\"") >= 0) {
        // Task 15: this player did not buzz in — round ended
        digitalWrite(LED_PIN, LOW);
        showIdleScreen();

      } else if (msg.indexOf("\"type\":\"STATE\"") >= 0 &&
                 msg.indexOf("\"IDLE\"") >= 0) {
        // Round fully reset by admin
        digitalWrite(LED_PIN, LOW);
        showIdleScreen();

      } else {
        // Fallback: show any text field if present
        int idx = msg.indexOf("\"text\":\"");
        if (idx >= 0) {
          int start = idx + 8;
          int end   = msg.indexOf("\"", start);
          showMsg("MSG:", msg.substring(start, end).c_str());
        } else {
          showMsg("SERVER:", msg.c_str());
        }
      }
      break;
    }

    case WStype_DISCONNECTED:
      Serial.println("WS Disconnected");
      showMsg("Disconnected");
      digitalWrite(LED_PIN, LOW);
      break;
  }
}

// ───── SETUP ─────
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  u8g2.begin();
  showMsg("Booting...");

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setAutoReconnect(true); // TASK 18 — built-in reconnect safety net
  WiFi.persistent(true);
  WiFi.begin(ssid, password);
  showMsg("Connecting WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    attempts++;
    if (attempts > 20) {
      Serial.println("\nRetry WiFi");
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(ssid, password);
      attempts = 0;
    }
  }

  Serial.println("\nWiFi OK");
  Serial.println(WiFi.localIP());
  showMsg("WiFi OK", WiFi.localIP().toString().c_str());

  webSocket.begin(server_ip, server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(2000);
}

// ───── LOOP ─────
void loop() {
  checkWifiConnection(); // TASK 18 — WiFi reconnection watchdog
  webSocket.loop();

  bool currentButtonState = digitalRead(BUTTON_PIN);

  // Button just pressed (HIGH → LOW)
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > 200) {
      lastPressTime = now;

      digitalWrite(LED_PIN, HIGH); // immediate feedback
      Serial.println("Button pressed");
      webSocket.sendTXT(
        String("{\"type\":\"PRESS\",\"player\":\"") +
        playerID +
        "\"}"
      );
      showMsg("Pressed!", playerID);
    }
  }

  // Button just released (LOW → HIGH)
  // Only turn LED off on release if not in BUZZED_IN state.
  // The server will send BUZZED_IN to keep the LED on if this
  // player won — otherwise it turns off naturally on release.
  if (lastButtonState == LOW && currentButtonState == HIGH) {
    // We don't force LOW here because BUZZED_IN handler already
    // sets it HIGH persistently. LOCKED/ROUND_OVER/STATE:IDLE
    // handlers are responsible for turning it off.
    // For a normal missed press, the LED was already turned off
    // by the LOCKED message the server sends to this player.
  }

  lastButtonState = currentButtonState;
}
