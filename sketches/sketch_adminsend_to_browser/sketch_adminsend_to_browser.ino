#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WebSocketsClient.h>
// ───── WIFI ─────
const char* ssid = "PUT SSID";
const char* password = "PUT PASSWORD";
// ───── SERVER ─────
const char* server_ip = "192.168.1.110";  // CHANGE THIS
const int server_port = 8081;
// ───── OLED ─────
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
// ───── WEBSOCKET ─────
WebSocketsClient webSocket;
// ───── OLED helper ─────
void showMsg(const char* a, const char* b = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 15, a);
  u8g2.drawStr(2, 28, b);
  u8g2.sendBuffer();
}
// ───── WEBSOCKET EVENTS ─────
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      Serial.println("WebSocket Connected");
      showMsg("WS Connected", "Sending HELLO");
      {String hello="{\"type\":\"HELLO\",\"role\":\"ADMIN\"}";
      webSocket.sendTXT(hello)
      ;}
      break;
    case WStype_TEXT:
      Serial.printf("Server: %s\n", payload);
      showMsg("Server msg", (char*)payload);
      break;
    case WStype_DISCONNECTED:
      Serial.println("WS Disconnected");
      showMsg("Disconnected");
      break;
  }
}
// ───── SETUP ─────
void setup() {
  Serial.begin(115200);
  u8g2.begin();
  showMsg("Booting...");
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
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
  // WebSocket connect
  webSocket.begin(server_ip, server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(2000);
}
// ───── LOOP ─────
void loop() {
  webSocket.loop();
}