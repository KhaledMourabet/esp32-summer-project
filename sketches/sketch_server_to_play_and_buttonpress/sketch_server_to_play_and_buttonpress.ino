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
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

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
      int idx = msg.indexOf("\"text\":\"");
      if (idx >= 0) {
        int start = idx + 8;
        int end = msg.indexOf("\"", start);
        showMsg("MSG:", msg.substring(start, end).c_str());
      } else {
        showMsg("SERVER:", msg.c_str());
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