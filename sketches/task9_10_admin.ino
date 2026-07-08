#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>

// ───── WIFI ─────
const char* ssid     = "Clarissa";
const char* password = "clar1234";

// ───── SERVER ─────
const char* server_ip  = "172.20.10.3";
const int   server_port = 8081;

// ───── OLED (SW_I2C, SDA=5, SCL=6) ─────
U8G2_SSD1306_72X40_ER_F_SW_I2C u8g2(U8G2_R0, 6, 5, U8X8_PIN_NONE);

// ───── PINS ─────
#define BUTTON_PIN 3

// ───── WEBSOCKET ─────
WebSocketsClient webSocket;

// ───── BUTTON STATE ─────
bool lastButtonState = HIGH;
unsigned long lastPressTime = 0;

// ───── OLED HELPER ─────
void showMsg(const char* a, const char* b = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 15, a);
  u8g2.drawStr(2, 28, b);
  u8g2.sendBuffer();
}

// ───── BIG LETTER DISPLAY ─────
void showBigLetter(const char* letter, const char* label) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr); // big 30px font
  // Center the letter on the 72x40 screen
  int x = (72 - u8g2.getStrWidth(letter)) / 2;
  u8g2.drawStr(x, 36, letter);
  u8g2.sendBuffer();
}

// ───── WEBSOCKET EVENTS ─────
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.println("WebSocket Connected");
      showMsg("WS Connected", "Sending HELLO");
      webSocket.sendTXT("{\"type\":\"HELLO\",\"role\":\"ADMIN\"}");
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      Serial.println("Server: " + msg);

      // ── Task 10: LED commands → show W or L on OLED ──
      if (msg.indexOf("\"type\":\"LED\"") >= 0) {

        if (msg.indexOf("\"command\":\"WIN\"") >= 0) {
          showBigLetter("W", "CORRECT");

        } else if (msg.indexOf("\"command\":\"LOSE\"") >= 0) {
          showBigLetter("L", "WRONG");

        } else if (msg.indexOf("\"command\":\"OFF\"") >= 0) {
          showMsg("Admin Ready", "Press button");
        }

      // ── Task 9: state updates ──
      } else if (msg.indexOf("\"type\":\"STATE\"") >= 0) {

        if (msg.indexOf("\"state\":\"IDLE\"") >= 0) {
          showMsg("Admin Ready", "Press button");

        } else if (msg.indexOf("\"state\":\"QUESTION\"") >= 0) {
          showMsg("Question sent", "Press again");

        } else if (msg.indexOf("\"state\":\"BUZZABLE\"") >= 0) {
          showMsg("Buzzer open", "Waiting...");

        } else if (msg.indexOf("\"state\":\"ANSWERED\"") >= 0) {
          showMsg("Answered", "Press to cont.");
        }

      // ── Show who buzzed ──
      } else if (msg.indexOf("\"type\":\"BUZZED\"") >= 0) {
        int idx = msg.indexOf("\"name\":\"");
        if (idx >= 0) {
          int start = idx + 8;
          int end = msg.indexOf("\"", start);
          String name = msg.substring(start, end);
          showMsg("Buzzed:", name.c_str());
        } else {
          showMsg("Player buzzed!", "Pick answer");
        }
      }
      break;
    }

    case WStype_DISCONNECTED:
      Serial.println("WS Disconnected");
      showMsg("Disconnected", "Retrying...");
      break;
  }
}

// ───── SETUP ─────
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  u8g2.begin();
  showMsg("Booting...");

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(ssid, password);
  showMsg("Connecting", "WiFi...");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
    attempts++;
    if (attempts > 20) {
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(ssid, password);
      attempts = 0;
    }
  }

  Serial.println("\nWiFi OK");
  showMsg("WiFi OK!", WiFi.localIP().toString().c_str());
  delay(1000);

  webSocket.begin(server_ip, server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(2000);
  showMsg("Connecting", "Server...");
}

// ───── LOOP ─────
void loop() {
  webSocket.loop();

  // ── Task 9: admin button press → advance game state ──
  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > 200) {
      lastPressTime = now;
      Serial.println("Admin button pressed");
      webSocket.sendTXT("{\"type\":\"ADMIN\"}");
      showMsg("Admin press", "Sent!");
    }
  }

  lastButtonState = currentButtonState;
}
