// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <Arduino.h>

struct BitChatPeer {
  String id;
  String nick;
  uint64_t lastSeenMs = 0;
};

struct BitChatConfig {
  const char *nickname = nullptr;
  uint32_t baud = 115200;
  bool serialOutput = true;
  bool serialCommands = true;
  bool waitForSerial = true;
  bool printStartupGuide = true;
  bool autoAnnounce = true;
  bool persistNickname = true;
};

typedef void (*BitChatPublicMessageCallback)(const BitChatPeer &peer, const String &message);
typedef void (*BitChatPrivateMessageCallback)(const BitChatPeer &peer, const String &message, const String &messageId);
typedef void (*BitChatPeerCallback)(const BitChatPeer &peer);

class BitChatESP32 {
public:
  void begin(uint32_t baud = 115200);
  void begin(const char *nickname, uint32_t baud = 115200);
  void begin(const String &nickname, uint32_t baud = 115200);
  void begin(const BitChatConfig &config);
  void loop();
  void printAbout();

  bool sendPublicMessage(const String &message);
  bool sendPrivateMessage(const String &peerSelector, const String &message);
  bool sendPrivateMessage(const BitChatPeer &peer, const String &message);
  bool replyPrivate(const BitChatPeer &peer, const String &message);
  bool requestSync(const String &peerSelector = "");
  void announce();

  void setNickname(const String &name);
  void setEpochMs(uint64_t epochMs);
  void setTime(uint64_t epochMs);
  uint64_t now() const;
  bool isClockSet() const;
  bool hasTime() const;

  String peerId() const;
  String nickname() const;
  size_t peerCount() const;
  bool getPeer(size_t index, BitChatPeer &peer) const;

  void enableSerialOutput(bool enabled);
  void enableSerialCommands(bool enabled);
  void setDebug(bool enabled, bool persist = false);
  void setAlive(bool enabled, bool persist = false);
  void setQuiet(bool enabled, bool persist = false);
  void setTimezoneMinutes(int16_t minutes, bool persist = true);

  void onPublicMessage(BitChatPublicMessageCallback callback);
  void onPrivateMessage(BitChatPrivateMessageCallback callback);
  void onPeer(BitChatPeerCallback callback);
};
