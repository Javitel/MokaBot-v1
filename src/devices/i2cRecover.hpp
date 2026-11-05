#pragma once

#include <Arduino.h>

// Frees an I2C bus left stuck by a reset in the middle of a transfer: a
// device (e.g. the ADS1115) still sending holds SDA low, and only a power
// cycle would release it otherwise (every read then fails with "i2cRead
// returned Error 263"). Up to 9 clock pulses let it finish its byte, then a
// STOP condition ends the transfer. Call before Wire.begin().
static inline void i2cBusRecover(int sda, int scl){
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delayMicroseconds(10);
  if(digitalRead(sda) == HIGH) return;   // bus free
  pinMode(scl, OUTPUT_OPEN_DRAIN);
  for(int k = 0; k < 9 && digitalRead(sda) == LOW; k++){
    digitalWrite(scl, LOW);
    delayMicroseconds(5);
    digitalWrite(scl, HIGH);
    delayMicroseconds(5);
  }
  // STOP: SDA low -> high while SCL is high.
  pinMode(sda, OUTPUT_OPEN_DRAIN);
  digitalWrite(sda, LOW);
  delayMicroseconds(5);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  digitalWrite(sda, HIGH);
  delayMicroseconds(5);
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  Serial.printf("I2C: bus was stuck (SDA low), recovered: SDA %s\n", digitalRead(sda) ? "free" : "still low");
}
