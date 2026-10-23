// Hello, emulator: prints over UART0, blinks the module LED on GPIO2 and
// switches GPIO5 while the GPIO0 (FLASH) button is held low.
#include <Arduino.h>

const int LED = 2;     // on-module LED, lit when LOW
const int BUTTON = 0;  // FLASH button, LOW when pressed
const int RELAY = 5;   // any output you want to watch

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  pinMode(RELAY, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  Serial.println();
  Serial.println("hello from the emulator");
}

void loop() {
  static unsigned long last;
  static bool wasPressed;
  if (millis() - last >= 500) {
    last = millis();
    digitalWrite(LED, !digitalRead(LED));
    Serial.printf("uptime %lu ms\n", millis());
  }
  bool pressed = digitalRead(BUTTON) == LOW;
  if (pressed != wasPressed) {
    wasPressed = pressed;
    digitalWrite(RELAY, pressed ? HIGH : LOW);
    Serial.println(pressed ? "button pressed" : "button released");
  }
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    Serial.printf("you typed: %s\n", line.c_str());
  }
}
