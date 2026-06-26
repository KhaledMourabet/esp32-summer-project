#include <Wire.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("Scanning I2C pins...");
  
  int sda_pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  int scl_pins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  
  for (int sda : sda_pins) {
    for (int scl : scl_pins) {
      if (sda == scl) continue;
      
      Wire.begin(sda, scl);
      Wire.beginTransmission(0x3C);  // common OLED address
      if (Wire.endTransmission() == 0) {
        Serial.print("Found! SDA=");
        Serial.print(sda);
        Serial.print(" SCL=");
        Serial.println(scl);
      }
      Wire.end();
    }
  }
  Serial.println("Done.");
}

void loop() {}