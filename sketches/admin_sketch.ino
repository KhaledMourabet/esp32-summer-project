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

// ───── PINS ─────
#define BUTTON_PIN 3   // admin button → pin 3 → GND (INPUT_PULLUP)
// External LEDs are driven via transistors — pins match original wiring
#define LED_C0 0   // corner top-left
#define LED_C1 1   // corner top-right
#define LED_C2 4   // corner bottom-right
#define LED_C3 7   // corner bottom-left
#define LED_CENTER 10 // center LED

// ───── OLED (hardware I2C, SDA=5, SCL=6) ─────
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

// ───── BIG LETTER DISPLAY ─────
// Shows W (win) or L (lose) in a large font on the OLED
void showBigLetter(const char* letter) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr);
  int x = (72 - u8g2.getStrWidth(letter)) / 2;
  u8g2.drawStr(x, 36, letter);
  u8g2.sendBuffer();
}

// ───── LED HELPERS ─────
void allLedsOff() {
  digitalWrite(LED_C0,     LOW);
  digitalWrite(LED_C1,     LOW);
  digitalWrite(LED_C2,     LOW);
  digitalWrite(LED_C3,     LOW);
  digitalWrite(LED_CENTER, LOW);
}

void ledWin() {
  // Correct answer: light 4 corner LEDs only
  allLedsOff();
  digitalWrite(LED_C0, HIGH);
  digitalWrite(LED_C1, HIGH);
  digitalWrite(LED_C2, HIGH);
  digitalWrite(LED_C3, HIGH);
}

void ledLose() {
  // Wrong answer: light all 5 LEDs
  digitalWrite(LED_C0,     HIGH);
  digitalWrite(LED_C1,     HIGH);
  digitalWrite(LED_C2,     HIGH);
  digitalWrite(LED_C3,     HIGH);
  digitalWrite(LED_CENTER, HIGH);
}

// ───── WEBSOCKET EVENTS ─────
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.println("WebSocket Connected");
      showMsg("WS Connected", "Sending HELLO");
      {
        String hello = "{\"type\":\"HELLO\",\"role\":\"ADMIN\"}";
        webSocket.sendTXT(hello);
      }
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      Serial.println("Server: " + msg);

      // ── LED commands (Task 10) ──
      if (msg.indexOf("\"type\":\"LED\"") >= 0) {

        if (msg.indexOf("\"command\":\"WIN\"") >= 0) {
          ledWin();
          showBigLetter("W");

        } else if (msg.indexOf("\"command\":\"LOSE\"") >= 0) {
          ledLose();
          showBigLetter("L");

        } else if (msg.indexOf("\"command\":\"OFF\"") >= 0) {
          allLedsOff();
          showMsg("Admin Ready", "Press button");
        }

      // ── State updates (Task 9) ──
      } else if (msg.indexOf("\"type\":\"STATE\"") >= 0) {

        if (msg.indexOf("\"state\":\"IDLE\"") >= 0) {
          allLedsOff();
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
          int end   = msg.indexOf("\"", start);
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
      allLedsOff();
      break;
  }
}

// ───── SETUP ─────
void setup() {
  Serial.begin(115200);

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // External LEDs
  pinMode(LED_C0,     OUTPUT); digitalWrite(LED_C0,     LOW);
  pinMode(LED_C1,     OUTPUT); digitalWrite(LED_C1,     LOW);
  pinMode(LED_C2,     OUTPUT); digitalWrite(LED_C2,     LOW);
  pinMode(LED_C3,     OUTPUT); digitalWrite(LED_C3,     LOW);
  pinMode(LED_CENTER, OUTPUT); digitalWrite(LED_CENTER, LOW);

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

  // Admin button press → advance game state (Task 9)
  bool currentButtonState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();
    if (now - lastPressTime > 200) {
      lastPressTime = now;
      Serial.println("Admin button pressed");
      String msg = "{\"type\":\"ADMIN\"}";
      webSocket.sendTXT(msg);
      showMsg("Admin press", "Sent!");
    }
  }

  lastButtonState = currentButtonState;
}
