// SPDX-License-Identifier: BSD-3-Clause
/*
  BitChat ESP32 TemperatureBroadcaster example

  Periodically broadcasts a temperature sample as a signed public BitChat
  message.

  Flow:
  1. Flash this example and open Serial Monitor at 115200 baud.
  2. Set epoch time after every reset:

       /time EPOCH_MS

  3. The board announces as @esp32-temp.
  4. Every BROADCAST_INTERVAL_MS it sends:

       temperature sample: 24.3 C

  By default this uses a synthetic sample so the example runs without external
  hardware. Set TEMPERATURE_PIN to an ADC pin to map analog input to 0..50 C.
*/

#include <BitChatESP32.h>
#include <math.h>

static const int TEMPERATURE_PIN = -1;
static const uint32_t BROADCAST_INTERVAL_MS = 60000;

BitChatESP32 bitchat;
static uint32_t lastBroadcastAt = 0;
static uint32_t lastClockReminderAt = 0;
static bool sentFirstSample = false;

static float readTemperatureC() {
  if (TEMPERATURE_PIN >= 0) {
    int raw = analogRead(TEMPERATURE_PIN);
    return (raw / 4095.0f) * 50.0f;
  }

  float phase = millis() / 60000.0f;
  return 24.0f + 2.0f * sinf(phase);
}

static void broadcastTemperature() {
  float temperatureC = readTemperatureC();
  char message[96];
  snprintf(message, sizeof(message), "temperature sample: %.1f C", temperatureC);
  bitchat.sendPublicMessage(String(message));
}

void setup() {
  if (TEMPERATURE_PIN >= 0) {
    analogReadResolution(12);
  }

  bitchat.begin("esp32-temp");

  Serial.println();
  Serial.println("TemperatureBroadcaster ready.");
  Serial.println("Set time with /time EPOCH_MS; broadcasts start after the clock is set.");
}

void loop() {
  bitchat.loop();

  if (!bitchat.isClockSet()) {
    if (millis() - lastClockReminderAt >= 10000) {
      lastClockReminderAt = millis();
      Serial.println("Set /time EPOCH_MS before broadcasting temperature samples.");
    }
    return;
  }

  uint32_t now = millis();
  if (!sentFirstSample || now - lastBroadcastAt >= BROADCAST_INTERVAL_MS) {
    sentFirstSample = true;
    lastBroadcastAt = now;
    broadcastTemperature();
  }
}
