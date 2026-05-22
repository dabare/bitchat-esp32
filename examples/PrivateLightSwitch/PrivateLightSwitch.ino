// SPDX-License-Identifier: BSD-3-Clause
/*
  BitChat ESP32 PrivateLightSwitch example

  Private-message control for a GPIO light or relay.

  Flow:
  1. Flash this example and open Serial Monitor at 115200 baud.
  2. Set epoch time after every reset so phone clients accept packets:

       /time EPOCH_MS

  3. In BitChat on a phone, find @esp32-light.
  4. Send a private message to the ESP32:

       on
       off
       toggle
       status

  5. The ESP32 switches LIGHT_PIN and replies privately.

  Change LIGHT_PIN and LIGHT_ON_LEVEL below for your board or relay module.
*/

#include <BitChatESP32.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static const uint8_t LIGHT_PIN = LED_BUILTIN;
static const uint8_t LIGHT_ON_LEVEL = HIGH;
static const uint8_t LIGHT_OFF_LEVEL = LIGHT_ON_LEVEL == HIGH ? LOW : HIGH;

BitChatESP32 bitchat;
static bool lightOn = false;
static uint32_t lastSyncAt = 0;

static void applyLight(bool on) {
  lightOn = on;
  digitalWrite(LIGHT_PIN, lightOn ? LIGHT_ON_LEVEL : LIGHT_OFF_LEVEL);
}

static void handlePrivateMessage(const BitChatPeer &peer, const String &message, const String &messageId) {
  (void)messageId;

  String command = message;
  command.trim();
  command.toLowerCase();

  if (command == "on" || command == "light on") {
    applyLight(true);
    bitchat.replyPrivate(peer, "light is on");
  } else if (command == "off" || command == "light off") {
    applyLight(false);
    bitchat.replyPrivate(peer, "light is off");
  } else if (command == "toggle") {
    applyLight(!lightOn);
    bitchat.replyPrivate(peer, lightOn ? "light is on" : "light is off");
  } else if (command == "status") {
    bitchat.replyPrivate(peer, lightOn ? "light is on" : "light is off");
  } else {
    bitchat.replyPrivate(peer, "commands: on, off, toggle, status");
  }
}

static void handlePeer(const BitChatPeer &peer) {
  Serial.printf("automation peer: %s (%s)\n", peer.nick.c_str(), peer.id.c_str());
}

void setup() {
  pinMode(LIGHT_PIN, OUTPUT);
  applyLight(false);

  bitchat.onPrivateMessage(handlePrivateMessage);
  bitchat.onPeer(handlePeer);
  bitchat.begin("esp32-light");

  Serial.println();
  Serial.println("PrivateLightSwitch ready.");
  Serial.println("Set time with /time EPOCH_MS, then send private commands: on, off, toggle, status");
}

void loop() {
  bitchat.loop();

  if (bitchat.isClockSet() && (lastSyncAt == 0 || millis() - lastSyncAt >= 30000)) {
    lastSyncAt = millis();
    bitchat.requestSync();
  }
}
