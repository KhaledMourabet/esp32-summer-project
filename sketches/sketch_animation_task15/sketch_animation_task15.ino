#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>

// ───── WIFI ─────
const char* ssid     = "Clarissa";
const char* password = "clar1234";

// ───── SERVER ─────
const char* server_ip  = "172.20.10.2"; // CHANGE THIS
const int   server_port = 8081;

// ───── PLAYER ID ─────
// Change this for each ESP32: "P1", "P2", "P3", or "P4"
const char* playerID = "P1";

// ───── PINS ─────
#define BUTTON_PIN 3   // button switch → pin 3 → GND (INPUT_PULLUP)
#define LED_PIN    10  // button's integrated LED via transistor

// ───── OLED ─────
// Same U8g2 object as the rest of the project (Task 3/5/13 etc.)
// so every unit runs one consistent display library at merge time.
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

// Animation center. These proportions were converted from the
// Adafruit 128x64 geometry you confirmed looked right (cx=64, cy=42,
// i.e. center + ~16% of height), scaled down to this 72x40 screen.
// If it still looks a hair off, cy is the first thing to nudge.
const int cx = 36;
const int cy = 26;

// ───── WEBSOCKET ─────
WebSocketsClient webSocket;

// ───── BUTTON STATE ─────
bool lastButtonState = HIGH;
unsigned long lastPressTime = 0;

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

// =========================================================
// TASK 15 — RESULT ANIMATIONS
// (geometry proportionally scaled from your tested Adafruit values)
// =========================================================

void playCorrectAnimation() {
  const char* label = "CORRECT!";

  // expanding ring (scaled from radius 18 max on 128x64 -> ~10 on 72x40)
  for (int r = 2; r <= 10; r += 2) {
    u8g2.clearBuffer();
    animHeader(label);
    u8g2.drawCircle(cx, cy, r);
    u8g2.sendBuffer();
    delay(45);
  }

  // checkmark drawn stroke by stroke (short leg then long leg)
  // scaled from (x1=-10,y1=0) (x2=-2,y2=+7) (x3=+11,y3=-8)
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

  delay(900); // hold the finished checkmark on screen
}

void playWrongAnimation() {
  const char* label = "WRONG";
  // scaled from size=14 on 128x64 -> ~8 on 72x40
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

  delay(900); // hold the finished X on screen
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

    case WStype_TEXT:
{
  String msg = (char*)payload;
  Serial.println(msg);

  if (msg.indexOf("\"type\":\"QUESTION\"") >= 0) {
    // Extract category from the JSON
    int idx = msg.indexOf("\"category\":\"");
    if (idx >= 0) {
      int start = idx + 12;
      int end = msg.indexOf("\"", start);
      String category = msg.substring(start, end);
      showMsg("Category:", category.c_str());
    }

  } else if (msg.indexOf("\"type\":\"RESULT\"") >= 0) {
    // TASK 15 — this player buzzed in and got a result back.
    bool isCorrect = (msg.indexOf("\"correct\":true") >= 0);
    if (isCorrect) {
      playCorrectAnimation();
    } else {
      playWrongAnimation();
    }
    // Animation holds itself; screen resets when ROUND_OVER/STATE arrives.

  } else if (msg.indexOf("\"type\":\"ROUND_OVER\"") >= 0) {
    // TASK 15 — this player did NOT buzz in; someone else got the result.
    showIdleScreen();

  } else if (msg.indexOf("\"type\":\"STATE\"") >= 0 &&
             msg.indexOf("\"IDLE\"") >= 0) {
    // Round fully reset by the server — back to idle for everyone.
    showIdleScreen();

  } else {
    // Keep existing handler for other messages
    int idx = msg.indexOf("\"text\":\"");
    if (idx >= 0) {
      int start = idx + 8;
      int end = msg.indexOf("\"", start);
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
      // Turn off LED if connection drops mid-game
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
  WiFi.begin(ssid, password);

  showMsg("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  Serial.println("WiFi OK");
  Serial.println(WiFi.localIP());
  showMsg("WiFi OK");

  webSocket.begin(server_ip, server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(2000);
}

// ───── LOOP ─────
void loop() {
  webSocket.loop();

  bool currentButtonState = digitalRead(BUTTON_PIN);

  // Button just pressed (HIGH → LOW)
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > 200) {
      lastPressTime = now;

      digitalWrite(LED_PIN, HIGH); // light button LED immediately

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
  if (lastButtonState == LOW && currentButtonState == HIGH) {
    digitalWrite(LED_PIN, LOW); // turn off button LED on release
  }

  lastButtonState = currentButtonState;
}
