// SPDX-License-Identifier: BSD-3-Clause
/*
  BitChat ESP32 Arduino port.

  Scope:
  - BLE GATT service/client compatibility with current iOS/Android BitChat UUIDs.
  - v1 BitChat packet encode/decode.
  - Signed announce packets and signed public messages.
  - Peer announcement verification and public message verification.
  - Noise_XX_25519_ChaChaPoly_SHA256 private messages.

  Not implemented yet:
  - Compression/decompression.
  - Full fragment send/reassembly for large packets.

  ESP32 Arduino core 3.x provides BLEDevice and libsodium. The current upstream
  BitChat apps reject unsigned peers, so Ed25519 signing is mandatory.
*/

#include "BitChatESP32.h"
#include "BitChatESP32Config.h"

#include <Preferences.h>
#include <sodium.h>
#include <new>
#include <stdarg.h>
#include <vector>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#if BITCHAT_USE_TESTNET
static const char *BITCHAT_SERVICE_UUID = "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5A";
#else
static const char *BITCHAT_SERVICE_UUID = "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C";
#endif
static const char *BITCHAT_CHARACTERISTIC_UUID = "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D";

static const uint8_t MSG_ANNOUNCE = 0x01;
static const uint8_t MSG_MESSAGE = 0x02;
static const uint8_t MSG_LEAVE = 0x03;
static const uint8_t MSG_NOISE_HANDSHAKE = 0x10;
static const uint8_t MSG_NOISE_ENCRYPTED = 0x11;
static const uint8_t MSG_FRAGMENT = 0x20;
static const uint8_t MSG_REQUEST_SYNC = 0x21;
static const uint8_t MSG_FILE_TRANSFER = 0x22;

static const uint8_t NOISE_PAYLOAD_PRIVATE_MESSAGE = 0x01;
static const uint8_t NOISE_PAYLOAD_READ_RECEIPT = 0x02;
static const uint8_t NOISE_PAYLOAD_DELIVERED = 0x03;
static const uint8_t NOISE_PAYLOAD_VERIFY_CHALLENGE = 0x10;
static const uint8_t NOISE_PAYLOAD_VERIFY_RESPONSE = 0x11;
static const uint8_t NOISE_PAYLOAD_FILE_TRANSFER = 0x20;

static const uint8_t FLAG_HAS_RECIPIENT = 0x01;
static const uint8_t FLAG_HAS_SIGNATURE = 0x02;
static const uint8_t FLAG_IS_COMPRESSED = 0x04;
static const uint8_t FLAG_HAS_ROUTE = 0x08;
static const uint8_t FLAG_IS_RSR = 0x10;

static const uint8_t MESSAGE_TTL = 7;
static const size_t SENDER_ID_SIZE = 8;
static const size_t SIGNATURE_SIZE = 64;
static const size_t PUBLIC_KEY_SIZE = 32;
static const size_t SIGN_SECRET_KEY_SIZE = crypto_sign_SECRETKEYBYTES;
static const size_t MAX_PUBLIC_TEXT_BYTES = 300;  // Keeps signed frames below negotiated 517-byte BLE MTU.
static const size_t MAX_PRIVATE_TEXT_BYTES = 255; // PrivateMessagePacket TLV length is one byte.
static const size_t MAX_NOISE_SESSIONS = 8;
static const size_t MAX_PENDING_NOISE_PAYLOADS = 12;
static const size_t MAX_TRACKED_PRIVATE_MESSAGES = 16;
static const uint32_t ANNOUNCE_INTERVAL_MS = 30000;
static const uint32_t SYNC_LOG_INTERVAL_MS = 15000;

static Preferences prefs;

static uint8_t noisePublicKey[PUBLIC_KEY_SIZE];
static uint8_t noiseSecretKey[crypto_box_SECRETKEYBYTES];
static uint8_t signingPublicKey[PUBLIC_KEY_SIZE];
static uint8_t signingSecretKey[SIGN_SECRET_KEY_SIZE];
static uint8_t myPeerID[SENDER_ID_SIZE];

static String nickname = "esp32";
static uint64_t timeBaseMs = 0;
static int16_t timeZoneOffsetMinutes = 0;
static bool debugMode = false;
static bool aliveMode = false;
static bool quietMode = false;
static bool serialOutputEnabled = true;
static bool serialCommandsEnabled = true;
static bool timeSetThisBoot = false;
static bool timeLoadedFromPrefs = false;

static BLECharacteristic *localCharacteristic = nullptr;
static BLEScan *bleScan = nullptr;
static BLEClient *remoteClient = nullptr;
static BLERemoteCharacteristic *remoteCharacteristic = nullptr;
static BLEAdvertisedDevice *pendingDevice = nullptr;
static bool serverPeerConnected = false;
static bool remoteConnected = false;
static bool connectInProgress = false;
static volatile bool announceRequested = false;
static volatile bool restartAdvertisingRequested = false;
static volatile bool stopScanRequested = false;
static volatile bool serverConnectedNotice = false;
static volatile bool serverDisconnectedNotice = false;
static volatile bool remoteDisconnectedNotice = false;
static uint32_t lastAnnounceAt = 0;
static uint32_t lastScanAt = 0;
static uint32_t lastAliveAt = 0;
static uint32_t lastSyncLogAt = 0;
static uint32_t sentMessageCounter = 0;
static int pendingDeviceRssi = 0;

static std::vector<uint8_t> incomingWriteBuffer;
static std::vector<uint8_t> incomingNotifyBuffer;

struct RxChunk {
  uint8_t stream = 0;
  size_t len = 0;
  uint8_t *data = nullptr;
};

static QueueHandle_t rxQueue = nullptr;
static const size_t MAX_CALLBACK_CHUNK = 1024;

struct Packet {
  uint8_t version = 1;
  uint8_t type = 0;
  uint8_t ttl = MESSAGE_TTL;
  uint64_t timestamp = 0;
  uint8_t senderID[SENDER_ID_SIZE] = {0};
  bool hasRecipient = false;
  uint8_t recipientID[SENDER_ID_SIZE] = {0};
  bool hasSignature = false;
  uint8_t signature[SIGNATURE_SIZE] = {0};
  bool isCompressed = false;
  bool isRSR = false;
  std::vector<uint8_t> payload;
};

struct RequestSyncInfo {
  bool wantsAnnounce = true;
};

struct PeerInfo {
  uint8_t peerID[SENDER_ID_SIZE];
  uint8_t noiseKey[PUBLIC_KEY_SIZE];
  uint8_t signingKey[PUBLIC_KEY_SIZE];
  String nick;
  uint64_t lastSeenMs = 0;
};

struct NoiseCipherState {
  bool hasKey = false;
  uint8_t key[32] = {0};
  uint32_t nonce = 0;
};

struct NoiseSymmetricState {
  uint8_t chainingKey[32] = {0};
  uint8_t hash[32] = {0};
  NoiseCipherState cipher;
};

struct NoiseSession {
  bool used = false;
  bool initiator = false;
  bool established = false;
  uint8_t pattern = 0;
  uint8_t peerID[SENDER_ID_SIZE] = {0};
  uint8_t localEphemeralPublic[PUBLIC_KEY_SIZE] = {0};
  uint8_t localEphemeralSecret[crypto_box_SECRETKEYBYTES] = {0};
  uint8_t remoteEphemeralPublic[PUBLIC_KEY_SIZE] = {0};
  uint8_t remoteStaticPublic[PUBLIC_KEY_SIZE] = {0};
  NoiseSymmetricState symmetric;
  NoiseCipherState sendCipher;
  NoiseCipherState receiveCipher;
  uint32_t startedAt = 0;
};

struct PendingNoisePayload {
  bool used = false;
  uint8_t peerID[SENDER_ID_SIZE] = {0};
  uint8_t type = 0;
  String messageID;
  String preview;
  std::vector<uint8_t> payload;
};

struct TrackedPrivateMessage {
  bool used = false;
  uint8_t peerID[SENDER_ID_SIZE] = {0};
  String messageID;
  uint64_t sentAt = 0;
  bool delivered = false;
  bool read = false;
};

struct DecodedAnnouncement {
  String nick;
  uint8_t noiseKey[PUBLIC_KEY_SIZE] = {0};
  uint8_t signingKey[PUBLIC_KEY_SIZE] = {0};
  bool hasNick = false;
  bool hasNoiseKey = false;
  bool hasSigningKey = false;
};

static std::vector<PeerInfo> peers;
static NoiseSession noiseSessions[MAX_NOISE_SESSIONS];
static PendingNoisePayload pendingNoisePayloads[MAX_PENDING_NOISE_PAYLOADS];
static TrackedPrivateMessage trackedPrivateMessages[MAX_TRACKED_PRIVATE_MESSAGES];
static BitChatPublicMessageCallback publicMessageCallback = nullptr;
static BitChatPrivateMessageCallback privateMessageCallback = nullptr;
static BitChatPeerCallback peerCallback = nullptr;

static void appendU16BE(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back((uint8_t)((value >> 8) & 0xFF));
  out.push_back((uint8_t)(value & 0xFF));
}

static void appendU32BE(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back((uint8_t)((value >> 24) & 0xFF));
  out.push_back((uint8_t)((value >> 16) & 0xFF));
  out.push_back((uint8_t)((value >> 8) & 0xFF));
  out.push_back((uint8_t)(value & 0xFF));
}

static void appendU64BE(std::vector<uint8_t> &out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back((uint8_t)((value >> shift) & 0xFF));
  }
}

static uint16_t readU16BE(const uint8_t *data) {
  return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t readU32BE(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t readU64BE(const uint8_t *data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

static String hexOf(const uint8_t *data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

static uint64_t nowMs();

static String twoDigits(uint8_t value) {
  String out;
  if (value < 10) out += '0';
  out += String(value);
  return out;
}

static String timeOf(uint64_t epochMs) {
  int64_t seconds = (int64_t)(epochMs / 1000ULL) + (int64_t)timeZoneOffsetMinutes * 60;
  int32_t daySeconds = (int32_t)(seconds % 86400);
  if (daySeconds < 0) daySeconds += 86400;
  uint8_t hour = daySeconds / 3600;
  uint8_t minute = (daySeconds % 3600) / 60;
  uint8_t second = daySeconds % 60;
  return twoDigits(hour) + ":" + twoDigits(minute) + ":" + twoDigits(second);
}

static void debugPrintf(const char *fmt, ...) {
  if (!serialOutputEnabled) return;
  if (!debugMode) return;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%s] # %s", timeOf(nowMs()).c_str(), buf);
}

static void systemPrintf(const char *fmt, ...) {
  if (!serialOutputEnabled) return;
  if (quietMode) return;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%s] * %s", timeOf(nowMs()).c_str(), buf);
}

static void errorPrintf(const char *fmt, ...) {
  if (!serialOutputEnabled) return;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%s] ! %s", timeOf(nowMs()).c_str(), buf);
}

static void chatPrintf(uint64_t timestampMs, const char *sender, const char *content) {
  if (!serialOutputEnabled) return;
  Serial.printf("[%s] %s: %s\n", timeOf(timestampMs).c_str(), sender, content);
}

static bool sameID(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, SENDER_ID_SIZE) == 0;
}

static uint64_t compileEpochSeconds() {
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monthStr[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  int month = 0;
  const char *p = strstr(months, monthStr);
  if (p) month = (int)((p - months) / 3) + 1;
  int day = atoi(__DATE__ + 4);
  int year = atoi(__DATE__ + 7);
  int hour = atoi(__TIME__);
  int minute = atoi(__TIME__ + 3);
  int second = atoi(__TIME__ + 6);

  // Civil date to days since Unix epoch. Valid for modern dates.
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + (int64_t)doe - 719468;
  return (uint64_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

static uint64_t nowMs() {
  return timeBaseMs + millis();
}

static void setEpochMs(uint64_t epochMs) {
  timeBaseMs = epochMs - millis();
  timeSetThisBoot = true;
  prefs.putULong64("timeBaseMs", timeBaseMs);
}

static void derivePeerID() {
  uint8_t digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, noisePublicKey, sizeof(noisePublicKey));
  memcpy(myPeerID, digest, SENDER_ID_SIZE);
}

static bool loadOrCreateIdentity() {
  prefs.begin("bitchat", false);

  bool haveKeys =
      prefs.getBytesLength("noisePub") == sizeof(noisePublicKey) &&
      prefs.getBytesLength("noiseSec") == sizeof(noiseSecretKey) &&
      prefs.getBytesLength("signPub") == sizeof(signingPublicKey) &&
      prefs.getBytesLength("signSec") == sizeof(signingSecretKey);

  if (haveKeys) {
    prefs.getBytes("noisePub", noisePublicKey, sizeof(noisePublicKey));
    prefs.getBytes("noiseSec", noiseSecretKey, sizeof(noiseSecretKey));
    prefs.getBytes("signPub", signingPublicKey, sizeof(signingPublicKey));
    prefs.getBytes("signSec", signingSecretKey, sizeof(signingSecretKey));
  } else {
    crypto_box_keypair(noisePublicKey, noiseSecretKey);
    crypto_sign_keypair(signingPublicKey, signingSecretKey);
    prefs.putBytes("noisePub", noisePublicKey, sizeof(noisePublicKey));
    prefs.putBytes("noiseSec", noiseSecretKey, sizeof(noiseSecretKey));
    prefs.putBytes("signPub", signingPublicKey, sizeof(signingPublicKey));
    prefs.putBytes("signSec", signingSecretKey, sizeof(signingSecretKey));
  }

  String storedNick = prefs.getString("nick", "");
  if (storedNick.length() > 0) nickname = storedNick;
  timeZoneOffsetMinutes = prefs.getShort("tzMin", 0);
  debugMode = prefs.getBool("debug", false);
  aliveMode = prefs.getBool("alive", false);
  quietMode = prefs.getBool("quiet", false);

  uint64_t storedBase = prefs.getULong64("timeBaseMs", 0);
  if (storedBase != 0) {
    timeBaseMs = storedBase;
    timeLoadedFromPrefs = true;
  } else {
    timeBaseMs = compileEpochSeconds() * 1000ULL;
    timeLoadedFromPrefs = false;
  }

  derivePeerID();
  return true;
}

static size_t optimalBlockSize(size_t dataSize) {
  static const size_t blockSizes[] = {256, 512, 1024, 2048};
  size_t totalSize = dataSize + 16;
  for (size_t blockSize : blockSizes) {
    if (totalSize <= blockSize) return blockSize;
  }
  return dataSize;
}

static void padToSize(std::vector<uint8_t> &data, size_t targetSize) {
  if (data.size() >= targetSize) return;
  size_t needed = targetSize - data.size();
  if (needed == 0 || needed > 255) return;
  data.insert(data.end(), needed, (uint8_t)needed);
}

static std::vector<uint8_t> unpadCopy(const std::vector<uint8_t> &data) {
  if (data.empty()) return data;
  uint8_t pad = data.back();
  if (pad == 0 || pad > data.size()) return data;
  size_t start = data.size() - pad;
  for (size_t i = start; i < data.size(); ++i) {
    if (data[i] != pad) return data;
  }
  return std::vector<uint8_t>(data.begin(), data.begin() + start);
}

static std::vector<uint8_t> encodePacket(const Packet &packet, bool padding) {
  std::vector<uint8_t> out;
  if (packet.version != 1 && packet.version != 2) return out;
  if (packet.version == 1 && packet.payload.size() > 65535) return out;

  out.reserve(14 + 8 + packet.payload.size() + SIGNATURE_SIZE + 16);
  out.push_back(packet.version);
  out.push_back(packet.type);
  out.push_back(packet.ttl);
  appendU64BE(out, packet.timestamp);

  uint8_t flags = 0;
  if (packet.hasRecipient) flags |= FLAG_HAS_RECIPIENT;
  if (packet.hasSignature) flags |= FLAG_HAS_SIGNATURE;
  if (packet.isCompressed) flags |= FLAG_IS_COMPRESSED;
  if (packet.isRSR) flags |= FLAG_IS_RSR;
  out.push_back(flags);

  if (packet.version == 2) {
    appendU32BE(out, (uint32_t)packet.payload.size());
  } else {
    appendU16BE(out, (uint16_t)packet.payload.size());
  }

  out.insert(out.end(), packet.senderID, packet.senderID + SENDER_ID_SIZE);
  if (packet.hasRecipient) {
    out.insert(out.end(), packet.recipientID, packet.recipientID + SENDER_ID_SIZE);
  }
  out.insert(out.end(), packet.payload.begin(), packet.payload.end());
  if (packet.hasSignature) {
    out.insert(out.end(), packet.signature, packet.signature + SIGNATURE_SIZE);
  }

  if (padding) {
    padToSize(out, optimalBlockSize(out.size()));
  }
  return out;
}

static bool decodePacketCore(const std::vector<uint8_t> &raw, Packet &packet) {
  if (raw.size() < 22) return false;
  size_t offset = 0;

  uint8_t version = raw[offset++];
  if (version != 1 && version != 2) return false;
  size_t headerSize = version == 2 ? 16 : 14;
  size_t lengthFieldSize = version == 2 ? 4 : 2;
  if (raw.size() < headerSize + SENDER_ID_SIZE) return false;

  packet = Packet();
  packet.version = version;
  packet.type = raw[offset++];
  packet.ttl = raw[offset++];
  packet.timestamp = readU64BE(&raw[offset]);
  offset += 8;

  uint8_t flags = raw[offset++];
  packet.hasRecipient = (flags & FLAG_HAS_RECIPIENT) != 0;
  packet.hasSignature = (flags & FLAG_HAS_SIGNATURE) != 0;
  packet.isCompressed = (flags & FLAG_IS_COMPRESSED) != 0;
  packet.isRSR = (flags & FLAG_IS_RSR) != 0;
  bool hasRoute = version >= 2 && (flags & FLAG_HAS_ROUTE) != 0;

  uint32_t payloadLength = 0;
  if (version == 2) {
    payloadLength = readU32BE(&raw[offset]);
    offset += 4;
  } else {
    payloadLength = readU16BE(&raw[offset]);
    offset += 2;
  }

  if (offset + SENDER_ID_SIZE > raw.size()) return false;
  memcpy(packet.senderID, &raw[offset], SENDER_ID_SIZE);
  offset += SENDER_ID_SIZE;

  if (packet.hasRecipient) {
    if (offset + SENDER_ID_SIZE > raw.size()) return false;
    memcpy(packet.recipientID, &raw[offset], SENDER_ID_SIZE);
    offset += SENDER_ID_SIZE;
  }

  if (hasRoute) {
    if (offset + 1 > raw.size()) return false;
    uint8_t routeCount = raw[offset++];
    size_t routeBytes = (size_t)routeCount * SENDER_ID_SIZE;
    if (offset + routeBytes > raw.size()) return false;
    offset += routeBytes;
  }

  if (payloadLength > 1024 * 1024) return false;
  if (offset + payloadLength > raw.size()) return false;

  if (packet.isCompressed && payloadLength < lengthFieldSize) return false;
  packet.payload.assign(raw.begin() + offset, raw.begin() + offset + payloadLength);
  offset += payloadLength;

  if (packet.hasSignature) {
    if (offset + SIGNATURE_SIZE > raw.size()) return false;
    memcpy(packet.signature, &raw[offset], SIGNATURE_SIZE);
    offset += SIGNATURE_SIZE;
  }

  return true;
}

static bool decodePacket(const std::vector<uint8_t> &raw, Packet &packet) {
  if (decodePacketCore(raw, packet)) return true;
  std::vector<uint8_t> unpadded = unpadCopy(raw);
  if (unpadded.size() == raw.size()) return false;
  return decodePacketCore(unpadded, packet);
}

static bool signPacket(Packet &packet) {
  Packet canonical = packet;
  canonical.ttl = 0;
  canonical.hasSignature = false;
  canonical.isRSR = false;
  std::vector<uint8_t> toSign = encodePacket(canonical, true);
  if (toSign.empty()) return false;

  unsigned long long sigLen = 0;
  if (crypto_sign_detached(packet.signature, &sigLen, toSign.data(), toSign.size(), signingSecretKey) != 0) {
    return false;
  }
  packet.hasSignature = sigLen == SIGNATURE_SIZE;
  return packet.hasSignature;
}

static bool verifyPacketSignature(const Packet &packet, const uint8_t *publicKey) {
  if (!packet.hasSignature) return false;
  Packet canonical = packet;
  canonical.ttl = 0;
  canonical.hasSignature = false;
  canonical.isRSR = false;
  std::vector<uint8_t> toVerify = encodePacket(canonical, true);
  if (toVerify.empty()) return false;
  return crypto_sign_verify_detached(packet.signature, toVerify.data(), toVerify.size(), publicKey) == 0;
}

static std::vector<uint8_t> buildAnnouncementPayload() {
  std::vector<uint8_t> payload;
  String nick = nickname;
  if (nick.length() > 255) nick = nick.substring(0, 255);

  payload.push_back(0x01);
  payload.push_back((uint8_t)nick.length());
  payload.insert(payload.end(), (const uint8_t *)nick.c_str(), (const uint8_t *)nick.c_str() + nick.length());

  payload.push_back(0x02);
  payload.push_back(PUBLIC_KEY_SIZE);
  payload.insert(payload.end(), noisePublicKey, noisePublicKey + PUBLIC_KEY_SIZE);

  payload.push_back(0x03);
  payload.push_back(PUBLIC_KEY_SIZE);
  payload.insert(payload.end(), signingPublicKey, signingPublicKey + PUBLIC_KEY_SIZE);

  return payload;
}

static Packet makeBasePacket(uint8_t type, const std::vector<uint8_t> &payload) {
  Packet packet;
  packet.version = 1;
  packet.type = type;
  packet.ttl = MESSAGE_TTL;
  packet.timestamp = nowMs();
  memcpy(packet.senderID, myPeerID, SENDER_ID_SIZE);
  packet.payload = payload;
  return packet;
}

static void appendIncoming(std::vector<uint8_t> &buffer, const uint8_t *data, size_t len, uint8_t stream);
static void processPacket(const Packet &packet, uint8_t stream);
static void sendAnnounce(bool force);

static void waitForSerialMonitor() {
  uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < 3000) {
    delay(10);
  }
}

static void bootLog(const char *message) {
  if (!serialOutputEnabled) return;
  if (!debugMode) return;
  Serial.printf("[boot %lu ms] %s\n", (unsigned long)millis(), message);
  Serial.flush();
}

static void requestAnnounceFromCallback() {
  announceRequested = true;
}

static void queueRxFromCallback(uint8_t stream, const uint8_t *data, size_t len) {
  if (rxQueue == nullptr || data == nullptr || len == 0) return;
  if (len > MAX_CALLBACK_CHUNK) len = MAX_CALLBACK_CHUNK;

  RxChunk *chunk = new (std::nothrow) RxChunk();
  if (chunk == nullptr) return;

  chunk->data = new (std::nothrow) uint8_t[len];
  if (chunk->data == nullptr) {
    delete chunk;
    return;
  }

  chunk->stream = stream;
  chunk->len = len;
  memcpy(chunk->data, data, len);

  if (xQueueSend(rxQueue, &chunk, 0) != pdTRUE) {
    delete[] chunk->data;
    delete chunk;
  }
}

static void drainRxQueue() {
  if (rxQueue == nullptr) return;

  RxChunk *chunk = nullptr;
  while (xQueueReceive(rxQueue, &chunk, 0) == pdTRUE) {
    if (chunk != nullptr && chunk->data != nullptr && chunk->len > 0) {
      if (chunk->stream == 0) {
        appendIncoming(incomingWriteBuffer, chunk->data, chunk->len, chunk->stream);
      } else {
        appendIncoming(incomingNotifyBuffer, chunk->data, chunk->len, chunk->stream);
      }
    }
    if (chunk != nullptr) {
      delete[] chunk->data;
      delete chunk;
    }
    chunk = nullptr;
  }
}

static void sendFrame(const std::vector<uint8_t> &frame, int8_t targetStream = -1) {
  if (frame.empty()) return;

  if ((targetStream < 0 || targetStream == 0) && serverPeerConnected && localCharacteristic != nullptr) {
    localCharacteristic->setValue(frame.data(), frame.size());
    localCharacteristic->notify();
    delay(8);
  }

  if ((targetStream < 0 || targetStream == 1) && remoteConnected && remoteCharacteristic != nullptr) {
    remoteCharacteristic->writeValue((uint8_t *)frame.data(), frame.size(), false);
    delay(8);
  }
}

static bool sendAnnouncePacket(bool force, uint8_t ttl, bool isRSR, int8_t targetStream, bool logSend) {
  uint32_t now = millis();
  if (!force && now - lastAnnounceAt < ANNOUNCE_INTERVAL_MS) return false;
  lastAnnounceAt = now;

  Packet packet = makeBasePacket(MSG_ANNOUNCE, buildAnnouncementPayload());
  packet.ttl = ttl;
  packet.isRSR = isRSR;
  if (!signPacket(packet)) {
    errorPrintf("announce signing failed\n");
    return false;
  }
  std::vector<uint8_t> frame = encodePacket(packet, false);
  sendFrame(frame, targetStream);
  if (logSend) {
    debugPrintf("announce sent as %s (%u bytes)\n", hexOf(myPeerID, SENDER_ID_SIZE).c_str(), (unsigned)frame.size());
  }
  return true;
}

static void sendAnnounce(bool force) {
  sendAnnouncePacket(force, MESSAGE_TTL, false, -1, true);
}

static bool sendPublicMessage(const String &line) {
  if (line.length() == 0) return false;
  if (line.length() > MAX_PUBLIC_TEXT_BYTES) {
    errorPrintf("message too long (%u > %u bytes); fragmentation is not implemented yet\n",
                (unsigned)line.length(), (unsigned)MAX_PUBLIC_TEXT_BYTES);
    return false;
  }

  std::vector<uint8_t> payload((const uint8_t *)line.c_str(), (const uint8_t *)line.c_str() + line.length());
  Packet packet = makeBasePacket(MSG_MESSAGE, payload);
  if (!signPacket(packet)) {
    errorPrintf("message signing failed\n");
    return false;
  }
  std::vector<uint8_t> frame = encodePacket(packet, false);
  sendFrame(frame);
  sentMessageCounter++;
  chatPrintf(packet.timestamp, nickname.c_str(), line.c_str());
  return true;
}

static bool decodeAnnouncement(const std::vector<uint8_t> &payload, DecodedAnnouncement &out) {
  size_t offset = 0;
  while (offset + 2 <= payload.size()) {
    uint8_t type = payload[offset++];
    uint8_t length = payload[offset++];
    if (offset + length > payload.size()) return false;

    if (type == 0x01) {
      out.nick = String((const char *)&payload[offset], length);
      out.hasNick = true;
    } else if (type == 0x02 && length == PUBLIC_KEY_SIZE) {
      memcpy(out.noiseKey, &payload[offset], PUBLIC_KEY_SIZE);
      out.hasNoiseKey = true;
    } else if (type == 0x03 && length == PUBLIC_KEY_SIZE) {
      memcpy(out.signingKey, &payload[offset], PUBLIC_KEY_SIZE);
      out.hasSigningKey = true;
    }
    offset += length;
  }
  return out.hasNick && out.hasNoiseKey && out.hasSigningKey;
}

static PeerInfo *findPeer(const uint8_t *peerID) {
  for (auto &peer : peers) {
    if (sameID(peer.peerID, peerID)) return &peer;
  }
  return nullptr;
}

static BitChatPeer makePeerSnapshot(const PeerInfo &peer) {
  BitChatPeer out;
  out.id = hexOf(peer.peerID, SENDER_ID_SIZE);
  out.nick = peer.nick;
  out.lastSeenMs = peer.lastSeenMs;
  return out;
}

static BitChatPeer makePeerSnapshot(const uint8_t *peerID) {
  PeerInfo *peer = findPeer(peerID);
  if (peer != nullptr) return makePeerSnapshot(*peer);

  BitChatPeer out;
  out.id = hexOf(peerID, SENDER_ID_SIZE);
  out.nick = out.id;
  out.lastSeenMs = 0;
  return out;
}

static String peerDisplayName(const uint8_t *peerID) {
  PeerInfo *peer = findPeer(peerID);
  if (peer != nullptr && peer->nick.length() > 0) return peer->nick;
  return hexOf(peerID, SENDER_ID_SIZE);
}

static bool setNicknameInternal(const String &name, bool persist) {
  String next = name;
  next.trim();
  if (next.length() == 0) return false;
  if (next.length() > 32) next = next.substring(0, 32);
  nickname = next;
  if (persist) prefs.putString("nick", nickname);
  return true;
}

static void privateChatPrintf(uint64_t timestampMs, const char *left, const char *right, const char *content) {
  if (!serialOutputEnabled) return;
  Serial.printf("[%s] %s -> %s: %s\n", timeOf(timestampMs).c_str(), left, right, content);
}

static String stringFromBytes(const uint8_t *data, size_t len) {
  if (len == 0) return "";
  return String((const char *)data, len);
}

static void resetNoiseCipher(NoiseCipherState &cipher) {
  if (cipher.hasKey) sodium_memzero(cipher.key, sizeof(cipher.key));
  cipher.hasKey = false;
  cipher.nonce = 0;
}

static void clearNoiseSession(NoiseSession &session) {
  sodium_memzero(session.localEphemeralSecret, sizeof(session.localEphemeralSecret));
  sodium_memzero(session.symmetric.chainingKey, sizeof(session.symmetric.chainingKey));
  sodium_memzero(session.symmetric.hash, sizeof(session.symmetric.hash));
  resetNoiseCipher(session.symmetric.cipher);
  resetNoiseCipher(session.sendCipher);
  resetNoiseCipher(session.receiveCipher);
  session = NoiseSession();
}

static void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data, size_t dataLen, uint8_t out[32]) {
  static const uint8_t empty = 0;
  crypto_auth_hmacsha256_state state;
  crypto_auth_hmacsha256_init(&state, key, keyLen);
  crypto_auth_hmacsha256_update(&state, dataLen > 0 ? data : &empty, dataLen);
  crypto_auth_hmacsha256_final(&state, out);
}

static void noiseHKDF(const uint8_t chainingKey[32], const uint8_t *inputKeyMaterial, size_t inputLen,
                      uint8_t outputs[][32], uint8_t outputCount) {
  uint8_t tempKey[32];
  hmacSha256(chainingKey, 32, inputKeyMaterial, inputLen, tempKey);

  uint8_t previous[32];
  size_t previousLen = 0;
  for (uint8_t i = 0; i < outputCount; ++i) {
    uint8_t buffer[33];
    if (previousLen > 0) memcpy(buffer, previous, previousLen);
    buffer[previousLen] = i + 1;
    hmacSha256(tempKey, sizeof(tempKey), buffer, previousLen + 1, outputs[i]);
    memcpy(previous, outputs[i], 32);
    previousLen = 32;
  }
  sodium_memzero(tempKey, sizeof(tempKey));
  sodium_memzero(previous, sizeof(previous));
}

static void noiseMixHash(NoiseSymmetricState &state, const uint8_t *data, size_t len) {
  crypto_hash_sha256_state hashState;
  uint8_t nextHash[32];
  crypto_hash_sha256_init(&hashState);
  crypto_hash_sha256_update(&hashState, state.hash, sizeof(state.hash));
  if (len > 0) crypto_hash_sha256_update(&hashState, data, len);
  crypto_hash_sha256_final(&hashState, nextHash);
  memcpy(state.hash, nextHash, sizeof(state.hash));
}

static void noiseMixKey(NoiseSymmetricState &state, const uint8_t *inputKeyMaterial, size_t len) {
  uint8_t outputs[2][32];
  noiseHKDF(state.chainingKey, inputKeyMaterial, len, outputs, 2);
  memcpy(state.chainingKey, outputs[0], 32);
  memcpy(state.cipher.key, outputs[1], 32);
  state.cipher.hasKey = true;
  state.cipher.nonce = 0;
  sodium_memzero(outputs, sizeof(outputs));
}

static void noiseInitSymmetric(NoiseSymmetricState &state) {
  static const char protocolName[] = "Noise_XX_25519_ChaChaPoly_SHA256";
  memset(&state, 0, sizeof(state));
  size_t nameLen = strlen(protocolName);
  if (nameLen <= 32) {
    memcpy(state.hash, protocolName, nameLen);
  } else {
    crypto_hash_sha256(state.hash, (const uint8_t *)protocolName, nameLen);
  }
  memcpy(state.chainingKey, state.hash, 32);
  noiseMixHash(state, nullptr, 0);  // Empty prologue, as in the upstream implementations.
}

static void noiseNonce12(uint64_t counter, uint8_t nonce[12]) {
  memset(nonce, 0, 12);
  for (uint8_t i = 0; i < 8; ++i) {
    nonce[4 + i] = (uint8_t)((counter >> (8 * i)) & 0xFF);
  }
}

static bool chachaEncrypt(const uint8_t key[32], uint64_t nonceCounter, const uint8_t *ad, size_t adLen,
                          const uint8_t *plaintext, size_t plaintextLen, std::vector<uint8_t> &ciphertext) {
  static const uint8_t empty = 0;
  uint8_t nonce[12];
  noiseNonce12(nonceCounter, nonce);
  ciphertext.assign(plaintextLen + crypto_aead_chacha20poly1305_ietf_ABYTES, 0);
  unsigned long long cipherLen = 0;
  int rc = crypto_aead_chacha20poly1305_ietf_encrypt(
      ciphertext.data(), &cipherLen,
      plaintextLen > 0 ? plaintext : &empty, plaintextLen,
      adLen > 0 ? ad : &empty, adLen,
      nullptr, nonce, key);
  if (rc != 0) return false;
  ciphertext.resize((size_t)cipherLen);
  return true;
}

static bool chachaDecrypt(const uint8_t key[32], uint64_t nonceCounter, const uint8_t *ad, size_t adLen,
                          const uint8_t *ciphertext, size_t ciphertextLen, std::vector<uint8_t> &plaintext) {
  static const uint8_t empty = 0;
  if (ciphertextLen < crypto_aead_chacha20poly1305_ietf_ABYTES) return false;
  uint8_t nonce[12];
  noiseNonce12(nonceCounter, nonce);
  size_t maxPlainLen = ciphertextLen - crypto_aead_chacha20poly1305_ietf_ABYTES;
  plaintext.assign(maxPlainLen == 0 ? 1 : maxPlainLen, 0);
  unsigned long long plainLen = 0;
  int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
      plaintext.data(), &plainLen, nullptr,
      ciphertext, ciphertextLen,
      adLen > 0 ? ad : &empty, adLen,
      nonce, key);
  if (rc != 0) return false;
  plaintext.resize((size_t)plainLen);
  return true;
}

static bool noiseCipherEncrypt(NoiseCipherState &cipher, const uint8_t *plaintext, size_t plaintextLen,
                               const uint8_t *ad, size_t adLen, bool prefixNonce,
                               std::vector<uint8_t> &out) {
  if (!cipher.hasKey) {
    out.clear();
    if (plaintextLen > 0) out.insert(out.end(), plaintext, plaintext + plaintextLen);
    return true;
  }

  uint32_t currentNonce = cipher.nonce;
  std::vector<uint8_t> sealed;
  if (!chachaEncrypt(cipher.key, currentNonce, ad, adLen, plaintext, plaintextLen, sealed)) return false;
  cipher.nonce++;

  out.clear();
  out.reserve((prefixNonce ? 4 : 0) + sealed.size());
  if (prefixNonce) appendU32BE(out, currentNonce);
  out.insert(out.end(), sealed.begin(), sealed.end());
  return true;
}

static bool noiseCipherDecrypt(NoiseCipherState &cipher, const uint8_t *ciphertext, size_t ciphertextLen,
                               const uint8_t *ad, size_t adLen, bool prefixNonce,
                               std::vector<uint8_t> &out) {
  if (!cipher.hasKey) {
    out.clear();
    if (ciphertextLen > 0) out.insert(out.end(), ciphertext, ciphertext + ciphertextLen);
    return true;
  }

  uint32_t nonce = cipher.nonce;
  const uint8_t *sealed = ciphertext;
  size_t sealedLen = ciphertextLen;
  if (prefixNonce) {
    if (ciphertextLen < 4 + crypto_aead_chacha20poly1305_ietf_ABYTES) return false;
    nonce = readU32BE(ciphertext);
    sealed = ciphertext + 4;
    sealedLen = ciphertextLen - 4;
  }

  if (!chachaDecrypt(cipher.key, nonce, ad, adLen, sealed, sealedLen, out)) return false;
  cipher.nonce++;
  return true;
}

static bool noiseEncryptAndHash(NoiseSymmetricState &state, const uint8_t *plaintext, size_t plaintextLen,
                                std::vector<uint8_t> &out) {
  if (!noiseCipherEncrypt(state.cipher, plaintext, plaintextLen, state.hash, sizeof(state.hash), false, out)) {
    return false;
  }
  noiseMixHash(state, out.empty() ? nullptr : out.data(), out.size());
  return true;
}

static bool noiseDecryptAndHash(NoiseSymmetricState &state, const uint8_t *ciphertext, size_t ciphertextLen,
                                std::vector<uint8_t> &out, bool allowMissingEmptyPayload = false) {
  if (state.cipher.hasKey && ciphertextLen == 0 && allowMissingEmptyPayload) {
    out.clear();
    return true;
  }
  if (!noiseCipherDecrypt(state.cipher, ciphertext, ciphertextLen, state.hash, sizeof(state.hash), false, out)) {
    return false;
  }
  noiseMixHash(state, ciphertextLen == 0 ? nullptr : ciphertext, ciphertextLen);
  return true;
}

static bool noiseDH(const uint8_t secret[32], const uint8_t publicKey[32], uint8_t shared[32]) {
  if (crypto_scalarmult_curve25519(shared, secret, publicKey) != 0) return false;
  return true;
}

static bool peerIDMatchesNoiseKey(const uint8_t *peerID, const uint8_t *publicKey) {
  uint8_t digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, publicKey, PUBLIC_KEY_SIZE);
  return memcmp(digest, peerID, SENDER_ID_SIZE) == 0;
}

static NoiseSession *findNoiseSession(const uint8_t *peerID) {
  for (auto &session : noiseSessions) {
    if (session.used && sameID(session.peerID, peerID)) return &session;
  }
  return nullptr;
}

static NoiseSession *allocateNoiseSession(const uint8_t *peerID, bool initiator) {
  NoiseSession *slot = nullptr;
  for (auto &session : noiseSessions) {
    if (!session.used) {
      slot = &session;
      break;
    }
  }
  if (slot == nullptr) {
    for (auto &session : noiseSessions) {
      if (!session.established) {
        slot = &session;
        break;
      }
    }
  }
  if (slot == nullptr) slot = &noiseSessions[0];

  clearNoiseSession(*slot);
  slot->used = true;
  slot->initiator = initiator;
  slot->established = false;
  slot->pattern = 0;
  slot->startedAt = millis();
  memcpy(slot->peerID, peerID, SENDER_ID_SIZE);
  noiseInitSymmetric(slot->symmetric);
  return slot;
}

static void clearNoiseSessionForPeer(const uint8_t *peerID) {
  NoiseSession *session = findNoiseSession(peerID);
  if (session != nullptr) clearNoiseSession(*session);
}

static bool noiseWriteMessage1(NoiseSession &session, std::vector<uint8_t> &message) {
  if (!session.initiator || session.pattern != 0) return false;
  crypto_box_keypair(session.localEphemeralPublic, session.localEphemeralSecret);
  message.clear();
  message.insert(message.end(), session.localEphemeralPublic, session.localEphemeralPublic + PUBLIC_KEY_SIZE);
  noiseMixHash(session.symmetric, session.localEphemeralPublic, PUBLIC_KEY_SIZE);
  std::vector<uint8_t> encryptedPayload;
  if (!noiseEncryptAndHash(session.symmetric, nullptr, 0, encryptedPayload)) return false;
  message.insert(message.end(), encryptedPayload.begin(), encryptedPayload.end());
  session.pattern = 1;
  return true;
}

static bool noiseReadMessage1(NoiseSession &session, const std::vector<uint8_t> &message) {
  if (session.initiator || session.pattern != 0 || message.size() < PUBLIC_KEY_SIZE) return false;
  size_t offset = 0;
  memcpy(session.remoteEphemeralPublic, message.data(), PUBLIC_KEY_SIZE);
  offset += PUBLIC_KEY_SIZE;
  noiseMixHash(session.symmetric, session.remoteEphemeralPublic, PUBLIC_KEY_SIZE);
  std::vector<uint8_t> payload;
  if (!noiseDecryptAndHash(session.symmetric, message.data() + offset, message.size() - offset, payload)) return false;
  session.pattern = 1;
  return payload.empty();
}

static bool noiseWriteMessage2(NoiseSession &session, std::vector<uint8_t> &message) {
  if (session.initiator || session.pattern != 1) return false;
  message.clear();

  crypto_box_keypair(session.localEphemeralPublic, session.localEphemeralSecret);
  message.insert(message.end(), session.localEphemeralPublic, session.localEphemeralPublic + PUBLIC_KEY_SIZE);
  noiseMixHash(session.symmetric, session.localEphemeralPublic, PUBLIC_KEY_SIZE);

  uint8_t shared[32];
  if (!noiseDH(session.localEphemeralSecret, session.remoteEphemeralPublic, shared)) return false; // ee
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> encryptedStatic;
  if (!noiseEncryptAndHash(session.symmetric, noisePublicKey, PUBLIC_KEY_SIZE, encryptedStatic)) return false; // s
  message.insert(message.end(), encryptedStatic.begin(), encryptedStatic.end());

  if (!noiseDH(noiseSecretKey, session.remoteEphemeralPublic, shared)) return false; // es, responder
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> encryptedPayload;
  if (!noiseEncryptAndHash(session.symmetric, nullptr, 0, encryptedPayload)) return false;
  message.insert(message.end(), encryptedPayload.begin(), encryptedPayload.end());

  sodium_memzero(shared, sizeof(shared));
  session.pattern = 2;
  return true;
}

static bool noiseReadMessage2(NoiseSession &session, const std::vector<uint8_t> &message) {
  if (!session.initiator || session.pattern != 1 || message.size() < 32 + 48) return false;
  size_t offset = 0;
  memcpy(session.remoteEphemeralPublic, message.data(), PUBLIC_KEY_SIZE);
  offset += PUBLIC_KEY_SIZE;
  noiseMixHash(session.symmetric, session.remoteEphemeralPublic, PUBLIC_KEY_SIZE);

  uint8_t shared[32];
  if (!noiseDH(session.localEphemeralSecret, session.remoteEphemeralPublic, shared)) return false; // ee
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> remoteStatic;
  if (!noiseDecryptAndHash(session.symmetric, message.data() + offset, 48, remoteStatic)) return false; // s
  if (remoteStatic.size() != PUBLIC_KEY_SIZE) return false;
  memcpy(session.remoteStaticPublic, remoteStatic.data(), PUBLIC_KEY_SIZE);
  offset += 48;

  if (!noiseDH(session.localEphemeralSecret, session.remoteStaticPublic, shared)) return false; // es, initiator
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> payload;
  if (!noiseDecryptAndHash(session.symmetric, message.data() + offset, message.size() - offset, payload)) return false;

  sodium_memzero(shared, sizeof(shared));
  session.pattern = 2;
  return payload.empty();
}

static bool noiseWriteMessage3(NoiseSession &session, std::vector<uint8_t> &message) {
  if (!session.initiator || session.pattern != 2) return false;
  message.clear();

  std::vector<uint8_t> encryptedStatic;
  if (!noiseEncryptAndHash(session.symmetric, noisePublicKey, PUBLIC_KEY_SIZE, encryptedStatic)) return false; // s
  message.insert(message.end(), encryptedStatic.begin(), encryptedStatic.end());

  uint8_t shared[32];
  if (!noiseDH(noiseSecretKey, session.remoteEphemeralPublic, shared)) return false; // se, initiator
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> encryptedPayload;
  if (!noiseEncryptAndHash(session.symmetric, nullptr, 0, encryptedPayload)) return false;
  message.insert(message.end(), encryptedPayload.begin(), encryptedPayload.end());

  sodium_memzero(shared, sizeof(shared));
  session.pattern = 3;
  return true;
}

static bool noiseReadMessage3(NoiseSession &session, const std::vector<uint8_t> &message) {
  if (session.initiator || session.pattern != 2 || message.size() < 48) return false;
  size_t offset = 0;

  std::vector<uint8_t> remoteStatic;
  if (!noiseDecryptAndHash(session.symmetric, message.data(), 48, remoteStatic)) return false; // s
  if (remoteStatic.size() != PUBLIC_KEY_SIZE) return false;
  memcpy(session.remoteStaticPublic, remoteStatic.data(), PUBLIC_KEY_SIZE);
  offset += 48;

  uint8_t shared[32];
  if (!noiseDH(session.localEphemeralSecret, session.remoteStaticPublic, shared)) return false; // se, responder
  noiseMixKey(session.symmetric, shared, sizeof(shared));

  std::vector<uint8_t> payload;
  if (!noiseDecryptAndHash(session.symmetric, message.data() + offset, message.size() - offset, payload, true)) {
    return false;
  }

  sodium_memzero(shared, sizeof(shared));
  session.pattern = 3;
  return payload.empty();
}

static bool noiseSplit(NoiseSession &session) {
  if (session.pattern != 3) return false;
  uint8_t outputs[2][32];
  noiseHKDF(session.symmetric.chainingKey, nullptr, 0, outputs, 2);

  session.sendCipher.hasKey = true;
  session.receiveCipher.hasKey = true;
  session.sendCipher.nonce = 0;
  session.receiveCipher.nonce = 0;
  if (session.initiator) {
    memcpy(session.sendCipher.key, outputs[0], 32);
    memcpy(session.receiveCipher.key, outputs[1], 32);
  } else {
    memcpy(session.sendCipher.key, outputs[1], 32);
    memcpy(session.receiveCipher.key, outputs[0], 32);
  }

  session.established = true;
  sodium_memzero(outputs, sizeof(outputs));
  sodium_memzero(session.symmetric.chainingKey, sizeof(session.symmetric.chainingKey));
  sodium_memzero(session.symmetric.hash, sizeof(session.symmetric.hash));
  resetNoiseCipher(session.symmetric.cipher);
  return true;
}

static bool sendNoiseHandshakePacket(const uint8_t *peerID, const std::vector<uint8_t> &payload) {
  Packet packet = makeBasePacket(MSG_NOISE_HANDSHAKE, payload);
  packet.hasRecipient = true;
  memcpy(packet.recipientID, peerID, SENDER_ID_SIZE);
  std::vector<uint8_t> frame = encodePacket(packet, false);
  sendFrame(frame);
  debugPrintf("noise handshake to %s (%u bytes)\n", hexOf(peerID, SENDER_ID_SIZE).c_str(), (unsigned)payload.size());
  return true;
}

static bool sendNoiseEncryptedPacket(const uint8_t *peerID, const std::vector<uint8_t> &payload) {
  Packet packet = makeBasePacket(MSG_NOISE_ENCRYPTED, payload);
  packet.hasRecipient = true;
  memcpy(packet.recipientID, peerID, SENDER_ID_SIZE);
  std::vector<uint8_t> frame = encodePacket(packet, false);
  sendFrame(frame);
  return true;
}

static bool startNoiseHandshake(const uint8_t *peerID, bool force = false) {
  NoiseSession *existing = findNoiseSession(peerID);
  if (existing != nullptr && existing->established && !force) return true;
  if (existing != nullptr && !existing->established && !force) return true;

  NoiseSession *session = allocateNoiseSession(peerID, true);
  if (session == nullptr) {
    errorPrintf("no noise session slots available\n");
    return false;
  }

  std::vector<uint8_t> message;
  if (!noiseWriteMessage1(*session, message)) {
    clearNoiseSession(*session);
    errorPrintf("failed to start private handshake\n");
    return false;
  }
  return sendNoiseHandshakePacket(peerID, message);
}

static bool encryptAndSendNoisePayload(const uint8_t *peerID, const std::vector<uint8_t> &plainPayload) {
  NoiseSession *session = findNoiseSession(peerID);
  if (session == nullptr || !session->established) return false;

  std::vector<uint8_t> encrypted;
  if (!noiseCipherEncrypt(session->sendCipher,
                          plainPayload.empty() ? nullptr : plainPayload.data(), plainPayload.size(),
                          nullptr, 0, true, encrypted)) {
    return false;
  }
  return sendNoiseEncryptedPacket(peerID, encrypted);
}

static bool queueNoisePayload(const uint8_t *peerID, uint8_t type, const std::vector<uint8_t> &payload,
                              const String &messageID, const String &preview) {
  for (auto &pending : pendingNoisePayloads) {
    if (!pending.used) {
      pending.used = true;
      memcpy(pending.peerID, peerID, SENDER_ID_SIZE);
      pending.type = type;
      pending.messageID = messageID;
      pending.preview = preview;
      pending.payload = payload;
      return true;
    }
  }
  return false;
}

static void markPendingUnused(PendingNoisePayload &pending) {
  pending.used = false;
  pending.type = 0;
  pending.messageID = "";
  pending.preview = "";
  pending.payload.clear();
  memset(pending.peerID, 0, sizeof(pending.peerID));
}

static void flushPendingNoisePayloads(const uint8_t *peerID) {
  for (auto &pending : pendingNoisePayloads) {
    if (!pending.used || !sameID(pending.peerID, peerID)) continue;
    std::vector<uint8_t> payload = pending.payload;
    String messageID = pending.messageID;
    uint8_t type = pending.type;
    markPendingUnused(pending);
    if (!encryptAndSendNoisePayload(peerID, payload)) {
      errorPrintf("failed to send pending private payload to %s\n", peerDisplayName(peerID).c_str());
      continue;
    }
    if (type == NOISE_PAYLOAD_PRIVATE_MESSAGE) {
      debugPrintf("private message %s sent to %s\n", messageID.c_str(), peerDisplayName(peerID).c_str());
    }
  }
}

static std::vector<uint8_t> buildTypedNoisePayload(uint8_t type, const String &text) {
  std::vector<uint8_t> payload;
  payload.reserve(1 + text.length());
  payload.push_back(type);
  payload.insert(payload.end(), (const uint8_t *)text.c_str(), (const uint8_t *)text.c_str() + text.length());
  return payload;
}

static bool buildPrivateMessagePayload(const String &messageID, const String &content, std::vector<uint8_t> &payload) {
  if (messageID.length() > 255 || content.length() > MAX_PRIVATE_TEXT_BYTES) return false;
  payload.clear();
  payload.reserve(1 + 2 + messageID.length() + 2 + content.length());
  payload.push_back(NOISE_PAYLOAD_PRIVATE_MESSAGE);
  payload.push_back(0x00);
  payload.push_back((uint8_t)messageID.length());
  payload.insert(payload.end(), (const uint8_t *)messageID.c_str(), (const uint8_t *)messageID.c_str() + messageID.length());
  payload.push_back(0x01);
  payload.push_back((uint8_t)content.length());
  payload.insert(payload.end(), (const uint8_t *)content.c_str(), (const uint8_t *)content.c_str() + content.length());
  return true;
}

static bool decodePrivateMessagePayload(const uint8_t *data, size_t len, String &messageID, String &content) {
  size_t offset = 0;
  bool haveID = false;
  bool haveContent = false;
  while (offset + 2 <= len) {
    uint8_t type = data[offset++];
    uint8_t length = data[offset++];
    if (offset + length > len) return false;
    if (type == 0x00) {
      messageID = String((const char *)(data + offset), length);
      haveID = true;
    } else if (type == 0x01) {
      content = String((const char *)(data + offset), length);
      haveContent = true;
    } else {
      return false;
    }
    offset += length;
  }
  return offset == len && haveID && haveContent;
}

static String makePrivateMessageID() {
  sentMessageCounter++;
  char timeBuf[24];
  snprintf(timeBuf, sizeof(timeBuf), "%llu", (unsigned long long)nowMs());
  String id = hexOf(myPeerID, SENDER_ID_SIZE);
  id += "-";
  id += timeBuf;
  id += "-";
  id += String(sentMessageCounter);
  return id;
}

static void trackPrivateMessage(const uint8_t *peerID, const String &messageID) {
  TrackedPrivateMessage *slot = nullptr;
  for (auto &tracked : trackedPrivateMessages) {
    if (tracked.used && tracked.messageID == messageID && sameID(tracked.peerID, peerID)) {
      slot = &tracked;
      break;
    }
    if (slot == nullptr && !tracked.used) slot = &tracked;
  }
  if (slot == nullptr) slot = &trackedPrivateMessages[0];
  slot->used = true;
  memcpy(slot->peerID, peerID, SENDER_ID_SIZE);
  slot->messageID = messageID;
  slot->sentAt = nowMs();
  slot->delivered = false;
  slot->read = false;
}

static TrackedPrivateMessage *findTrackedPrivateMessage(const uint8_t *peerID, const String &messageID) {
  for (auto &tracked : trackedPrivateMessages) {
    if (tracked.used && tracked.messageID == messageID && sameID(tracked.peerID, peerID)) return &tracked;
  }
  return nullptr;
}

static bool sendTypedNoisePayloadToPeer(const uint8_t *peerID, uint8_t type, const String &text,
                                        bool queueIfNeeded = true) {
  std::vector<uint8_t> payload = buildTypedNoisePayload(type, text);
  if (encryptAndSendNoisePayload(peerID, payload)) return true;
  if (!queueIfNeeded) return false;
  if (!queueNoisePayload(peerID, type, payload, text, "")) {
    errorPrintf("private queue full\n");
    return false;
  }
  return startNoiseHandshake(peerID);
}

static bool sendPrivateMessageToPeer(PeerInfo *peer, const String &content) {
  if (peer == nullptr) return false;
  if (content.length() == 0) {
    errorPrintf("private message cannot be empty\n");
    return false;
  }
  if (content.length() > MAX_PRIVATE_TEXT_BYTES) {
    errorPrintf("private message too long (%u > %u bytes)\n",
                (unsigned)content.length(), (unsigned)MAX_PRIVATE_TEXT_BYTES);
    return false;
  }

  String messageID = makePrivateMessageID();
  std::vector<uint8_t> payload;
  if (!buildPrivateMessagePayload(messageID, content, payload)) {
    errorPrintf("failed to encode private message\n");
    return false;
  }

  trackPrivateMessage(peer->peerID, messageID);
  privateChatPrintf(nowMs(), "you", peer->nick.c_str(), content.c_str());

  if (encryptAndSendNoisePayload(peer->peerID, payload)) {
    debugPrintf("private message %s sent to %s\n", messageID.c_str(), peer->nick.c_str());
    return true;
  }

  if (!queueNoisePayload(peer->peerID, NOISE_PAYLOAD_PRIVATE_MESSAGE, payload, messageID, content)) {
    errorPrintf("private queue full\n");
    return false;
  }
  systemPrintf("handshaking with %s for private chat\n", peer->nick.c_str());
  return startNoiseHandshake(peer->peerID);
}

static void markNoiseSessionEstablished(NoiseSession &session) {
  String name = peerDisplayName(session.peerID);
  debugPrintf("private session established with %s\n", name.c_str());
  flushPendingNoisePayloads(session.peerID);
}

static void handleNoiseHandshake(const Packet &packet) {
  if (sameID(packet.senderID, myPeerID)) return;
  if (!packet.hasRecipient || !sameID(packet.recipientID, myPeerID)) return;

  if (packet.payload.size() == 32) {
    NoiseSession *session = allocateNoiseSession(packet.senderID, false);
    if (session == nullptr || !noiseReadMessage1(*session, packet.payload)) {
      if (session != nullptr) clearNoiseSession(*session);
      debugPrintf("bad noise message 1 from %s\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
      return;
    }

    std::vector<uint8_t> response;
    if (!noiseWriteMessage2(*session, response)) {
      clearNoiseSession(*session);
      debugPrintf("failed noise response to %s\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
      return;
    }
    sendNoiseHandshakePacket(packet.senderID, response);
    return;
  }

  NoiseSession *session = findNoiseSession(packet.senderID);
  if (session == nullptr) {
    debugPrintf("noise handshake continuation from unknown peer %s\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
    return;
  }

  bool ok = false;
  bool attempted = false;
  std::vector<uint8_t> response;
  if (session->initiator && session->pattern == 1 && packet.payload.size() >= 96) {
    attempted = true;
    ok = noiseReadMessage2(*session, packet.payload) &&
         peerIDMatchesNoiseKey(packet.senderID, session->remoteStaticPublic) &&
         noiseWriteMessage3(*session, response) &&
         noiseSplit(*session);
    if (ok) {
      sendNoiseHandshakePacket(packet.senderID, response);
      markNoiseSessionEstablished(*session);
      return;
    }
  } else if (!session->initiator && session->pattern == 2 &&
             (packet.payload.size() == 48 || packet.payload.size() == 64)) {
    attempted = true;
    ok = noiseReadMessage3(*session, packet.payload) &&
         peerIDMatchesNoiseKey(packet.senderID, session->remoteStaticPublic) &&
         noiseSplit(*session);
    if (ok) {
      markNoiseSessionEstablished(*session);
      return;
    }
  }

  if (!attempted) {
    debugPrintf("unexpected noise handshake from %s (%u bytes, pattern=%u, role=%s)\n",
                hexOf(packet.senderID, SENDER_ID_SIZE).c_str(),
                (unsigned)packet.payload.size(),
                (unsigned)session->pattern,
                session->initiator ? "initiator" : "responder");
    return;
  }

  debugPrintf("noise handshake failed from %s (%u bytes)\n",
              hexOf(packet.senderID, SENDER_ID_SIZE).c_str(),
              (unsigned)packet.payload.size());
  clearNoiseSession(*session);
}

static void handleNoiseEncrypted(const Packet &packet) {
  if (sameID(packet.senderID, myPeerID)) return;
  if (!packet.hasRecipient || !sameID(packet.recipientID, myPeerID)) return;

  NoiseSession *session = findNoiseSession(packet.senderID);
  if (session == nullptr || !session->established) {
    debugPrintf("encrypted private packet before session from %s; starting handshake\n",
                hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
    startNoiseHandshake(packet.senderID);
    return;
  }

  std::vector<uint8_t> decrypted;
  if (!noiseCipherDecrypt(session->receiveCipher,
                          packet.payload.empty() ? nullptr : packet.payload.data(), packet.payload.size(),
                          nullptr, 0, true, decrypted)) {
    debugPrintf("private decrypt failed from %s; resetting session\n",
                hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
    clearNoiseSession(*session);
    startNoiseHandshake(packet.senderID, true);
    return;
  }
  if (decrypted.empty()) return;

  uint8_t payloadType = decrypted[0];
  const uint8_t *payloadData = decrypted.size() > 1 ? decrypted.data() + 1 : nullptr;
  size_t payloadLen = decrypted.size() > 1 ? decrypted.size() - 1 : 0;
  String name = peerDisplayName(packet.senderID);

  if (payloadType == NOISE_PAYLOAD_PRIVATE_MESSAGE) {
    String messageID;
    String content;
    if (!decodePrivateMessagePayload(payloadData, payloadLen, messageID, content)) {
      debugPrintf("bad private TLV from %s\n", name.c_str());
      return;
    }
    privateChatPrintf(packet.timestamp, name.c_str(), "you", content.c_str());
    sendTypedNoisePayloadToPeer(packet.senderID, NOISE_PAYLOAD_DELIVERED, messageID, false);
    if (privateMessageCallback != nullptr) {
      BitChatPeer snapshot = makePeerSnapshot(packet.senderID);
      privateMessageCallback(snapshot, content, messageID);
    }
  } else if (payloadType == NOISE_PAYLOAD_DELIVERED) {
    String messageID = stringFromBytes(payloadData, payloadLen);
    TrackedPrivateMessage *tracked = findTrackedPrivateMessage(packet.senderID, messageID);
    if (tracked != nullptr) tracked->delivered = true;
    systemPrintf("delivered to %s: %s\n", name.c_str(), messageID.c_str());
  } else if (payloadType == NOISE_PAYLOAD_READ_RECEIPT) {
    String messageID = stringFromBytes(payloadData, payloadLen);
    TrackedPrivateMessage *tracked = findTrackedPrivateMessage(packet.senderID, messageID);
    if (tracked != nullptr) {
      tracked->delivered = true;
      tracked->read = true;
    }
    systemPrintf("read by %s: %s\n", name.c_str(), messageID.c_str());
  } else if (payloadType == NOISE_PAYLOAD_VERIFY_CHALLENGE || payloadType == NOISE_PAYLOAD_VERIFY_RESPONSE) {
    debugPrintf("verification payload 0x%02x from %s ignored in serial port\n", payloadType, name.c_str());
  } else if (payloadType == NOISE_PAYLOAD_FILE_TRANSFER) {
    debugPrintf("encrypted file payload from %s ignored; file transfer is not implemented on ESP32 yet\n", name.c_str());
  } else {
    debugPrintf("unknown private payload 0x%02x from %s\n", payloadType, name.c_str());
  }
}

static void upsertPeer(const uint8_t *peerID, const DecodedAnnouncement &ann) {
  PeerInfo *updatedPeer = nullptr;
  PeerInfo *existing = findPeer(peerID);
  if (existing == nullptr) {
    if (peers.size() >= 32) {
      peers.erase(peers.begin());
    }
    PeerInfo peer;
    memcpy(peer.peerID, peerID, SENDER_ID_SIZE);
    memcpy(peer.noiseKey, ann.noiseKey, PUBLIC_KEY_SIZE);
    memcpy(peer.signingKey, ann.signingKey, PUBLIC_KEY_SIZE);
    peer.nick = ann.nick;
    peer.lastSeenMs = nowMs();
    peers.push_back(peer);
    updatedPeer = &peers.back();
    systemPrintf("%s joined (%s)\n", ann.nick.c_str(), hexOf(peerID, SENDER_ID_SIZE).c_str());
  } else {
    String oldNick = existing->nick;
    memcpy(existing->noiseKey, ann.noiseKey, PUBLIC_KEY_SIZE);
    memcpy(existing->signingKey, ann.signingKey, PUBLIC_KEY_SIZE);
    existing->nick = ann.nick;
    existing->lastSeenMs = nowMs();
    updatedPeer = existing;
    if (oldNick != ann.nick) {
      systemPrintf("%s is now %s\n", oldNick.c_str(), ann.nick.c_str());
    }
  }

  if (peerCallback != nullptr && updatedPeer != nullptr) {
    BitChatPeer snapshot = makePeerSnapshot(*updatedPeer);
    peerCallback(snapshot);
  }
}

static void handleAnnounce(const Packet &packet) {
  DecodedAnnouncement ann;
  if (!decodeAnnouncement(packet.payload, ann)) {
    debugPrintf("bad announce payload\n");
    return;
  }

  uint8_t digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, ann.noiseKey, PUBLIC_KEY_SIZE);
  if (memcmp(digest, packet.senderID, SENDER_ID_SIZE) != 0) {
    debugPrintf("reject announce: peer id mismatch from %s\n", ann.nick.c_str());
    return;
  }

  if (!verifyPacketSignature(packet, ann.signingKey)) {
    debugPrintf("reject announce: bad signature from %s\n", ann.nick.c_str());
    return;
  }

  if (sameID(packet.senderID, myPeerID)) return;
  upsertPeer(packet.senderID, ann);
}

static void handlePublicMessage(const Packet &packet) {
  if (sameID(packet.senderID, myPeerID)) return;

  PeerInfo *peer = findPeer(packet.senderID);
  if (peer == nullptr) {
    debugPrintf("drop message from unknown peer %s\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
    return;
  }
  if (!verifyPacketSignature(packet, peer->signingKey)) {
    debugPrintf("drop message from %s: bad signature\n", peer->nick.c_str());
    return;
  }

  String text((const char *)packet.payload.data(), packet.payload.size());
  chatPrintf(packet.timestamp, peer->nick.c_str(), text.c_str());
  if (publicMessageCallback != nullptr) {
    BitChatPeer snapshot = makePeerSnapshot(*peer);
    publicMessageCallback(snapshot, text);
  }
}

static bool decodeRequestSync(const std::vector<uint8_t> &payload, RequestSyncInfo &out) {
  bool sawP = false;
  bool sawM = false;
  bool sawData = false;
  uint8_t p = 0;
  uint32_t m = 0;
  bool sawTypes = false;
  uint64_t types = 0;

  size_t offset = 0;
  while (offset + 3 <= payload.size()) {
    uint8_t type = payload[offset++];
    uint16_t length = readU16BE(&payload[offset]);
    offset += 2;
    if (offset + length > payload.size()) return false;

    const uint8_t *value = &payload[offset];
    if (type == 0x01 && length == 1) {
      p = value[0];
      sawP = true;
    } else if (type == 0x02 && length == 4) {
      m = readU32BE(value);
      sawM = true;
    } else if (type == 0x03) {
      if (length > 1024) return false;
      sawData = true;
    } else if (type == 0x04 && length >= 1 && length <= 8) {
      sawTypes = true;
      types = 0;
      for (uint16_t i = 0; i < length; ++i) {
        types |= ((uint64_t)value[i]) << (8 * i);
      }
    }

    offset += length;
  }

  if (!sawP || p < 1 || !sawM || m == 0 || !sawData) return false;
  out.wantsAnnounce = !sawTypes || ((types & 0x01ULL) != 0);
  return true;
}

static void handleRequestSync(const Packet &packet, uint8_t stream) {
  if (sameID(packet.senderID, myPeerID)) return;
  if (packet.hasRecipient && !sameID(packet.recipientID, myPeerID)) return;

  RequestSyncInfo sync;
  if (!decodeRequestSync(packet.payload, sync)) {
    debugPrintf("bad request sync from %s\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
    return;
  }

  if (!sync.wantsAnnounce) return;

  PeerInfo *peer = findPeer(packet.senderID);
  if (peer != nullptr && !verifyPacketSignature(packet, peer->signingKey)) {
    debugPrintf("drop request sync from %s: bad signature\n", peer->nick.c_str());
    return;
  }

  bool directRequest = packet.hasRecipient && sameID(packet.recipientID, myPeerID);
  sendAnnouncePacket(true, 0, directRequest, (int8_t)stream, false);

  uint32_t now = millis();
  if (debugMode && now - lastSyncLogAt >= SYNC_LOG_INTERVAL_MS) {
    lastSyncLogAt = now;
    debugPrintf("request sync from %s; sent announce response%s\n",
                hexOf(packet.senderID, SENDER_ID_SIZE).c_str(),
                directRequest ? " (RSR)" : "");
  }
}

static void processPacket(const Packet &packet, uint8_t stream) {
  if (packet.isCompressed) {
    debugPrintf("drop compressed packet type 0x%02x; compression not implemented on ESP32 yet\n", packet.type);
    return;
  }

  switch (packet.type) {
    case MSG_ANNOUNCE:
      handleAnnounce(packet);
      break;
    case MSG_MESSAGE:
      handlePublicMessage(packet);
      break;
    case MSG_LEAVE:
      systemPrintf("%s left\n", hexOf(packet.senderID, SENDER_ID_SIZE).c_str());
      break;
    case MSG_NOISE_HANDSHAKE:
      handleNoiseHandshake(packet);
      break;
    case MSG_NOISE_ENCRYPTED:
      handleNoiseEncrypted(packet);
      break;
    case MSG_FRAGMENT:
      debugPrintf("fragment packet ignored; fragmentation is not implemented yet\n");
      break;
    case MSG_REQUEST_SYNC:
      handleRequestSync(packet, stream);
      break;
    case MSG_FILE_TRANSFER:
      debugPrintf("packet type 0x%02x ignored\n", packet.type);
      break;
    default:
      debugPrintf("unknown packet type 0x%02x\n", packet.type);
      break;
  }
}

static bool expectedFrameLength(const std::vector<uint8_t> &buffer, size_t &frameLength) {
  if (buffer.size() < 22) return false;
  uint8_t version = buffer[0];
  if (version != 1 && version != 2) {
    frameLength = 1;
    return true;
  }

  size_t headerSize = version == 2 ? 16 : 14;
  if (buffer.size() < headerSize + SENDER_ID_SIZE) return false;

  uint8_t flags = buffer[11];
  uint32_t payloadLength = version == 2 ? readU32BE(&buffer[12]) : readU16BE(&buffer[12]);
  if (payloadLength > 1024 * 1024) {
    frameLength = 1;
    return true;
  }

  frameLength = headerSize + SENDER_ID_SIZE + payloadLength;
  if (flags & FLAG_HAS_RECIPIENT) frameLength += SENDER_ID_SIZE;
  if (version >= 2 && (flags & FLAG_HAS_ROUTE)) {
    size_t routeCountOffset = headerSize + SENDER_ID_SIZE + ((flags & FLAG_HAS_RECIPIENT) ? SENDER_ID_SIZE : 0);
    if (buffer.size() <= routeCountOffset) return false;
    frameLength += 1 + (size_t)buffer[routeCountOffset] * SENDER_ID_SIZE;
  }
  if (flags & FLAG_HAS_SIGNATURE) frameLength += SIGNATURE_SIZE;
  return true;
}

static void appendIncoming(std::vector<uint8_t> &buffer, const uint8_t *data, size_t len, uint8_t stream) {
  buffer.insert(buffer.end(), data, data + len);
  while (true) {
    size_t frameLength = 0;
    if (!expectedFrameLength(buffer, frameLength)) return;
    if (frameLength == 1) {
      buffer.erase(buffer.begin());
      continue;
    }
    if (buffer.size() < frameLength) return;

    std::vector<uint8_t> frame(buffer.begin(), buffer.begin() + frameLength);
    buffer.erase(buffer.begin(), buffer.begin() + frameLength);

    Packet packet;
    if (decodePacket(frame, packet)) {
      processPacket(packet, stream);
    } else {
      debugPrintf("failed to decode incoming frame\n");
    }
  }
}

class LocalServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    serverPeerConnected = true;
    serverConnectedNotice = true;
    requestAnnounceFromCallback();
  }

  void onDisconnect(BLEServer *server) override {
    serverPeerConnected = false;
    serverDisconnectedNotice = true;
    restartAdvertisingRequested = true;
  }
};

class LocalCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() == 0) return;
    queueRxFromCallback(0, (const uint8_t *)value.c_str(), value.length());
  }
};

static void remoteNotifyCallback(BLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify) {
  queueRxFromCallback(1, data, length);
}

class RemoteClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *client) override {
    remoteConnected = true;
  }

  void onDisconnect(BLEClient *client) override {
    remoteConnected = false;
    remoteCharacteristic = nullptr;
    remoteDisconnectedNotice = true;
  }
};

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (remoteConnected || connectInProgress || pendingDevice != nullptr) return;
    if (!advertisedDevice.isConnectable()) return;
    if (!advertisedDevice.isAdvertisingService(BLEUUID(BITCHAT_SERVICE_UUID))) return;

    pendingDevice = new BLEAdvertisedDevice(advertisedDevice);
    pendingDeviceRssi = advertisedDevice.getRSSI();
    stopScanRequested = true;
  }
};

static void connectToPendingDevice() {
  if (pendingDevice == nullptr || connectInProgress || remoteConnected) return;
  connectInProgress = true;

  BLEAdvertisedDevice *device = pendingDevice;
  pendingDevice = nullptr;

  systemPrintf("connecting to BitChat peripheral...\n");
  if (remoteClient != nullptr) {
    delete remoteClient;
    remoteClient = nullptr;
  }

  remoteClient = BLEDevice::createClient();
  remoteClient->setClientCallbacks(new RemoteClientCallbacks());

  if (!remoteClient->connect(device)) {
    systemPrintf("connect failed\n");
    delete device;
    connectInProgress = false;
    return;
  }

  BLERemoteService *service = remoteClient->getService(BITCHAT_SERVICE_UUID);
  if (service == nullptr) {
    debugPrintf("service not found\n");
    remoteClient->disconnect();
    delete device;
    connectInProgress = false;
    return;
  }

  remoteCharacteristic = service->getCharacteristic(BITCHAT_CHARACTERISTIC_UUID);
  if (remoteCharacteristic == nullptr) {
    debugPrintf("characteristic not found\n");
    remoteClient->disconnect();
    delete device;
    connectInProgress = false;
    return;
  }

  if (remoteCharacteristic->canNotify()) {
    remoteCharacteristic->registerForNotify(remoteNotifyCallback);
  }

  remoteConnected = true;
  connectInProgress = false;
  delete device;

  systemPrintf("connected to remote BitChat peripheral\n");
  sendAnnounce(true);
}

static void handleDeferredBleEvents() {
  if (stopScanRequested) {
    stopScanRequested = false;
    if (bleScan != nullptr && bleScan->isScanning()) {
      bleScan->stop();
    }
    if (pendingDevice != nullptr) {
      debugPrintf("found BitChat peripheral RSSI=%d\n", pendingDeviceRssi);
    }
  }

  if (restartAdvertisingRequested) {
    restartAdvertisingRequested = false;
    BLEDevice::startAdvertising();
  }

  if (serverConnectedNotice) {
    serverConnectedNotice = false;
    systemPrintf("phone connected to ESP32 peripheral\n");
  }

  if (serverDisconnectedNotice) {
    serverDisconnectedNotice = false;
    systemPrintf("phone disconnected from ESP32 peripheral\n");
  }

  if (remoteDisconnectedNotice) {
    remoteDisconnectedNotice = false;
    systemPrintf("disconnected from remote BitChat peripheral\n");
  }

  if (announceRequested) {
    announceRequested = false;
    sendAnnounce(true);
  }
}

static void startBle() {
  BLEDevice::init("");
  BLEDevice::setMTU(517);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new LocalServerCallbacks());

  BLEService *service = server->createService(BITCHAT_SERVICE_UUID);
  localCharacteristic = service->createCharacteristic(
      BITCHAT_CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR |
          BLECharacteristic::PROPERTY_NOTIFY);
  localCharacteristic->setCallbacks(new LocalCharacteristicCallbacks());
  localCharacteristic->addDescriptor(new BLE2902());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BITCHAT_SERVICE_UUID);
  advertising->setScanResponse(false);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
  bleScan->setActiveScan(false);
  bleScan->setInterval(160);
  bleScan->setWindow(80);
}

static bool parseBoolArg(String arg, bool current, bool &out) {
  arg.trim();
  arg.toLowerCase();
  if (arg.length() == 0 || arg == "toggle") {
    out = !current;
    return true;
  }
  if (arg == "on" || arg == "1" || arg == "true" || arg == "yes") {
    out = true;
    return true;
  }
  if (arg == "off" || arg == "0" || arg == "false" || arg == "no") {
    out = false;
    return true;
  }
  return false;
}

static bool isDigits(const String &value) {
  if (value.length() == 0) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

static PeerInfo *findPeerBySelector(String selector) {
  selector.trim();
  if (selector.length() == 0) return nullptr;

  if (isDigits(selector)) {
    int index = selector.toInt();
    if (index >= 1 && (size_t)index <= peers.size()) return &peers[index - 1];
  }

  String wanted = selector;
  wanted.toLowerCase();
  for (auto &peer : peers) {
    String id = hexOf(peer.peerID, SENDER_ID_SIZE);
    String nick = peer.nick;
    nick.toLowerCase();
    if (id.startsWith(wanted) || nick == wanted) return &peer;
  }
  return nullptr;
}

static String timezoneString() {
  int16_t minutes = timeZoneOffsetMinutes;
  char sign = minutes < 0 ? '-' : '+';
  if (minutes < 0) minutes = -minutes;
  return String(sign) + twoDigits(minutes / 60) + ":" + twoDigits(minutes % 60);
}

static bool parseTimezone(String arg, int16_t &minutesOut) {
  arg.trim();
  if (arg.length() == 0) return false;

  bool negative = arg.startsWith("-");
  bool hasSign = arg.startsWith("+") || arg.startsWith("-");
  bool hasColon = arg.indexOf(':') >= 0;
  if (arg.startsWith("+") || arg.startsWith("-")) arg = arg.substring(1);
  arg.replace(":", "");
  if (!isDigits(arg)) return false;

  int value = arg.toInt();
  int minutes = 0;
  if (!hasSign && !hasColon && arg.length() != 4) {
    minutes = value;
  } else if (arg.length() <= 2) {
    minutes = value * 60;
  } else if (arg.length() == 3 || arg.length() == 4) {
    minutes = (value / 100) * 60 + (value % 100);
  } else {
    return false;
  }
  if (minutes > 14 * 60) return false;
  minutesOut = negative ? -minutes : minutes;
  return true;
}

static void printBanner() {
  if (!serialOutputEnabled) return;
  Serial.println();
  Serial.println("bitchat");
  Serial.printf("@%s  %s  peers:%u  /help\n",
                nickname.c_str(),
                hexOf(myPeerID, SENDER_ID_SIZE).c_str(),
                (unsigned)peers.size());
  Serial.println("type a message and press enter");
  Serial.println();
}

static void printStartupGuide() {
  if (!serialOutputEnabled) return;
  Serial.println();
  Serial.println("bitchat ESP32 Arduino");
  Serial.println("BLE mesh chat compatible with current BitChat iOS and Android clients.");
  Serial.println();
  Serial.printf("identity: @%s  %s\n", nickname.c_str(), hexOf(myPeerID, SENDER_ID_SIZE).c_str());
  Serial.printf("service:  %s\n", BITCHAT_SERVICE_UUID);
  Serial.println();
  Serial.println("clock requirement");
  Serial.println("  BitChat packets carry Unix epoch millisecond timestamps.");
  Serial.println("  Phone clients reject stale or future packets; iOS currently accepts about +/-5 minutes.");
  Serial.println("  ESP32 boards usually have no battery-backed RTC, so set time after every reset.");
  Serial.printf("  current ESP32 time: %llu (%s %s, source: %s)\n",
                (unsigned long long)nowMs(),
                timeOf(nowMs()).c_str(),
                timezoneString().c_str(),
                timeLoadedFromPrefs ? "saved offset" : "firmware compile time");
  Serial.println();
  Serial.println("set time now");
  Serial.println("  /time EPOCH_MS       example: /time 1779274623804");
  Serial.println("  /time EPOCH_SECONDS  accepted and converted to milliseconds");
  Serial.println("  get epoch ms on a computer: date +%s000");
  Serial.println("  get epoch ms in a browser console: Date.now()");
  Serial.println();
  Serial.println("quick commands");
  Serial.println("  TEXT                 send signed public message");
  Serial.println("  /dm PEER TEXT        send encrypted private message");
  Serial.println("  /peers               list verified peers");
  Serial.println("  /sessions            list private Noise sessions");
  Serial.println("  /debug on|off        protocol diagnostics, default off");
  Serial.println("  /help                full command list");
  Serial.println();
}

static void printStatus() {
  if (!serialOutputEnabled) return;
  size_t sessionCount = 0;
  size_t establishedCount = 0;
  size_t pendingCount = 0;
  for (auto &session : noiseSessions) {
    if (session.used) {
      sessionCount++;
      if (session.established) establishedCount++;
    }
  }
  for (auto &pending : pendingNoisePayloads) {
    if (pending.used) pendingCount++;
  }

  Serial.println();
  Serial.println("status");
  Serial.printf("  nick:        %s\n", nickname.c_str());
  Serial.printf("  peer id:     %s\n", hexOf(myPeerID, SENDER_ID_SIZE).c_str());
  Serial.printf("  peers:       %u\n", (unsigned)peers.size());
  Serial.printf("  private:     %u sessions, %u ready, %u queued\n",
                (unsigned)sessionCount, (unsigned)establishedCount, (unsigned)pendingCount);
  Serial.printf("  server link: %s\n", serverPeerConnected ? "connected" : "waiting");
  Serial.printf("  remote link: %s\n", remoteConnected ? "connected" : (connectInProgress ? "connecting" : "idle"));
  Serial.printf("  heap:        %u\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("  time:        %llu %s %s\n",
                (unsigned long long)nowMs(),
                timeOf(nowMs()).c_str(),
                timezoneString().c_str());
  Serial.printf("  clock set:   %s\n", timeSetThisBoot ? "yes" : "no, use /time EPOCH_MS after reset");
  Serial.printf("  debug:       %s\n", debugMode ? "on" : "off");
  Serial.printf("  alive:       %s\n", aliveMode ? "on" : "off");
  Serial.printf("  quiet:       %s\n", quietMode ? "on" : "off");
  Serial.printf("  max public:  %u bytes\n", (unsigned)MAX_PUBLIC_TEXT_BYTES);
  Serial.println();
}

static void printPeers() {
  if (!serialOutputEnabled) return;
  Serial.printf("[%s] peers: %u\n", timeOf(nowMs()).c_str(), (unsigned)peers.size());
  for (size_t i = 0; i < peers.size(); ++i) {
    const PeerInfo &peer = peers[i];
    uint64_t age = nowMs() > peer.lastSeenMs ? (nowMs() - peer.lastSeenMs) / 1000ULL : 0;
    Serial.printf("  %u. %-18s %s  seen %lus ago\n",
                  (unsigned)(i + 1),
                  peer.nick.c_str(),
                  hexOf(peer.peerID, SENDER_ID_SIZE).c_str(),
                  (unsigned long)age);
  }
}

static void printSessions() {
  if (!serialOutputEnabled) return;
  Serial.printf("[%s] private sessions\n", timeOf(nowMs()).c_str());
  bool any = false;
  for (size_t i = 0; i < MAX_NOISE_SESSIONS; ++i) {
    const NoiseSession &session = noiseSessions[i];
    if (!session.used) continue;
    any = true;
    size_t pending = 0;
    for (auto &item : pendingNoisePayloads) {
      if (item.used && sameID(item.peerID, session.peerID)) pending++;
    }
    Serial.printf("  %u. %-18s %s  %s  pattern=%u queued=%u\n",
                  (unsigned)(i + 1),
                  peerDisplayName(session.peerID).c_str(),
                  hexOf(session.peerID, SENDER_ID_SIZE).c_str(),
                  session.established ? "ready" : (session.initiator ? "handshaking out" : "handshaking in"),
                  (unsigned)session.pattern,
                  (unsigned)pending);
  }
  if (!any) Serial.println("  none");
}

static std::vector<uint8_t> buildRequestSyncPayload() {
  std::vector<uint8_t> payload;
  payload.push_back(0x01);
  appendU16BE(payload, 1);
  payload.push_back(1);

  payload.push_back(0x02);
  appendU16BE(payload, 4);
  appendU32BE(payload, 1);

  payload.push_back(0x03);
  appendU16BE(payload, 0);

  payload.push_back(0x04);
  appendU16BE(payload, 1);
  payload.push_back(0x03);  // announce + public message
  return payload;
}

static void sendRequestSync(PeerInfo *target) {
  Packet packet = makeBasePacket(MSG_REQUEST_SYNC, buildRequestSyncPayload());
  packet.ttl = 0;
  if (target != nullptr) {
    packet.hasRecipient = true;
    memcpy(packet.recipientID, target->peerID, SENDER_ID_SIZE);
  }
  if (!signPacket(packet)) {
    errorPrintf("request sync signing failed\n");
    return;
  }
  std::vector<uint8_t> frame = encodePacket(packet, false);
  sendFrame(frame);
  if (target == nullptr) {
    systemPrintf("sync requested from nearby peers\n");
  } else {
    systemPrintf("sync requested from %s\n", target->nick.c_str());
  }
}

static void printHelp() {
  if (!serialOutputEnabled) return;
  Serial.println();
  Serial.println("bitchat serial commands");
  Serial.println("chat");
  Serial.println("  TEXT                  send public message");
  Serial.println("  /say TEXT             send public message");
  Serial.println("  /me ACTION            send public action text");
  Serial.println("  /dm PEER TEXT         send private encrypted message");
  Serial.println("  /read PEER MSG_ID     send private read receipt");
  Serial.println("peers");
  Serial.println("  /peers or /who        list verified peers");
  Serial.println("  /sessions             list private Noise sessions");
  Serial.println("  /sync [all|PEER]      request announce/message sync");
  Serial.println("identity");
  Serial.println("  /nick NAME            set nickname and announce");
  Serial.println("  /id                   show peer and public keys");
  Serial.println("  /time [EPOCH_MS]      show or set clock in epoch ms");
  Serial.println("  /tz [+HHMM|MINUTES]   set serial display timezone");
  Serial.println("view");
  Serial.println("  /status               show connection and memory state");
  Serial.println("  /about                show startup description and clock requirement");
  Serial.println("  /view                 redraw header");
  Serial.println("  /clear                clear terminal");
  Serial.println("  /debug [on|off]       show protocol diagnostics");
  Serial.println("  /alive [on|off]       periodic status line");
  Serial.println("  /quiet [on|off]       hide join/link system lines");
  Serial.println("ble");
  Serial.println("  /announce             send signed announce now");
  Serial.println("  /scan                 start BLE scan now");
  Serial.println("  /disconnect           disconnect outbound BLE client");
  Serial.println("  /reboot               restart ESP32");
  Serial.println();
}

static void printAlive() {
  if (!aliveMode) return;
  uint32_t now = millis();
  if (now - lastAliveAt < 5000) return;
  lastAliveAt = now;
  systemPrintf("alive peers=%u server=%s remote=%s heap=%u time=%llu\n",
               (unsigned)peers.size(),
               serverPeerConnected ? "yes" : "no",
               remoteConnected ? "yes" : "no",
               (unsigned)ESP.getFreeHeap(),
               (unsigned long long)nowMs());
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (!line.startsWith("/")) {
    sendPublicMessage(line);
    return;
  }

  int firstSpace = line.indexOf(' ');
  String command = firstSpace < 0 ? line.substring(1) : line.substring(1, firstSpace);
  String args = firstSpace < 0 ? "" : line.substring(firstSpace + 1);
  command.toLowerCase();
  args.trim();

  if (command == "help" || command == "h" || command == "?") {
    printHelp();
  } else if (command == "say" || command == "msg" || command == "s") {
    sendPublicMessage(args);
  } else if (command == "me") {
    if (args.length() == 0) {
      errorPrintf("usage: /me ACTION\n");
    } else {
      String action = "* ";
      action += nickname;
      action += " ";
      action += args;
      sendPublicMessage(action);
    }
  } else if (command == "dm" || command == "pm" || command == "private") {
    int split = args.indexOf(' ');
    if (split <= 0 || split >= (int)args.length() - 1) {
      errorPrintf("usage: /dm PEER TEXT\n");
      return;
    }
    String selector = args.substring(0, split);
    String text = args.substring(split + 1);
    text.trim();
    PeerInfo *peer = findPeerBySelector(selector);
    if (peer == nullptr) {
      errorPrintf("peer not found: %s\n", selector.c_str());
      return;
    }
    sendPrivateMessageToPeer(peer, text);
  } else if (command == "read" || command == "rr") {
    int split = args.indexOf(' ');
    if (split <= 0 || split >= (int)args.length() - 1) {
      errorPrintf("usage: /read PEER MSG_ID\n");
      return;
    }
    String selector = args.substring(0, split);
    String messageID = args.substring(split + 1);
    messageID.trim();
    PeerInfo *peer = findPeerBySelector(selector);
    if (peer == nullptr) {
      errorPrintf("peer not found: %s\n", selector.c_str());
      return;
    }
    if (sendTypedNoisePayloadToPeer(peer->peerID, NOISE_PAYLOAD_READ_RECEIPT, messageID)) {
      systemPrintf("read receipt sent to %s: %s\n", peer->nick.c_str(), messageID.c_str());
    }
  } else if (command == "nick") {
    if (args.length() == 0) {
      errorPrintf("usage: /nick NAME\n");
      return;
    }
    setNicknameInternal(args, true);
    systemPrintf("nickname set to %s\n", nickname.c_str());
    sendAnnounce(true);
  } else if (command == "time") {
    if (args.length() == 0) {
      systemPrintf("time %llu %s %s\n",
                   (unsigned long long)nowMs(),
                   timeOf(nowMs()).c_str(),
                   timezoneString().c_str());
      return;
    }
    uint64_t t = strtoull(args.c_str(), nullptr, 10);
    if (t > 1000000000000ULL) {
      setEpochMs(t);
      systemPrintf("time set to %llu %s\n", (unsigned long long)nowMs(), timeOf(nowMs()).c_str());
      sendAnnounce(true);
    } else if (t > 1000000000ULL) {
      setEpochMs(t * 1000ULL);
      systemPrintf("time set to %llu %s\n", (unsigned long long)nowMs(), timeOf(nowMs()).c_str());
      sendAnnounce(true);
    } else {
      errorPrintf("time must be epoch milliseconds or epoch seconds\n");
    }
  } else if (command == "tz") {
    if (args.length() == 0) {
      systemPrintf("timezone %s\n", timezoneString().c_str());
      return;
    }
    int16_t offset = 0;
    if (!parseTimezone(args, offset)) {
      errorPrintf("usage: /tz +0530 or /tz 330\n");
      return;
    }
    timeZoneOffsetMinutes = offset;
    prefs.putShort("tzMin", timeZoneOffsetMinutes);
    systemPrintf("timezone set to %s\n", timezoneString().c_str());
  } else if (command == "announce") {
    sendAnnounce(true);
    systemPrintf("announce sent\n");
  } else if (command == "peers" || command == "who") {
    printPeers();
  } else if (command == "sessions") {
    printSessions();
  } else if (command == "sync") {
    String syncTarget = args;
    syncTarget.toLowerCase();
    if (args.length() == 0 || syncTarget == "all") {
      sendRequestSync(nullptr);
    } else {
      PeerInfo *peer = findPeerBySelector(args);
      if (peer == nullptr) {
        errorPrintf("peer not found: %s\n", args.c_str());
      } else {
        sendRequestSync(peer);
      }
    }
  } else if (command == "id") {
    Serial.printf("peer id: %s\n", hexOf(myPeerID, SENDER_ID_SIZE).c_str());
    Serial.printf("noise public key: %s\n", hexOf(noisePublicKey, PUBLIC_KEY_SIZE).c_str());
    Serial.printf("signing public key: %s\n", hexOf(signingPublicKey, PUBLIC_KEY_SIZE).c_str());
  } else if (command == "status") {
    printStatus();
  } else if (command == "about") {
    printStartupGuide();
  } else if (command == "view") {
    printBanner();
  } else if (command == "clear" || command == "cls") {
    Serial.print("\033[2J\033[H");
    printBanner();
  } else if (command == "debug" || command == "raw") {
    bool next = false;
    if (!parseBoolArg(args, debugMode, next)) {
      errorPrintf("usage: /debug [on|off]\n");
      return;
    }
    debugMode = next;
    prefs.putBool("debug", debugMode);
    systemPrintf("debug %s\n", debugMode ? "on" : "off");
  } else if (command == "alive") {
    bool next = false;
    if (!parseBoolArg(args, aliveMode, next)) {
      errorPrintf("usage: /alive [on|off]\n");
      return;
    }
    aliveMode = next;
    prefs.putBool("alive", aliveMode);
    systemPrintf("alive %s\n", aliveMode ? "on" : "off");
  } else if (command == "quiet") {
    bool next = false;
    if (!parseBoolArg(args, quietMode, next)) {
      errorPrintf("usage: /quiet [on|off]\n");
      return;
    }
    quietMode = next;
    prefs.putBool("quiet", quietMode);
    Serial.printf("[%s] * quiet %s\n", timeOf(nowMs()).c_str(), quietMode ? "on" : "off");
  } else if (command == "scan") {
    if (bleScan != nullptr && !bleScan->isScanning()) {
      systemPrintf("scanning for BitChat peers...\n");
      bleScan->start(5, false);
      bleScan->clearResults();
      lastScanAt = millis();
    } else {
      systemPrintf("scan already active\n");
    }
  } else if (command == "disconnect") {
    if (remoteClient != nullptr && remoteConnected) {
      remoteClient->disconnect();
      systemPrintf("remote BLE client disconnect requested\n");
    } else {
      systemPrintf("no outbound BLE client is connected\n");
    }
  } else if (command == "reboot") {
    systemPrintf("restarting\n");
    delay(100);
    ESP.restart();
  } else {
    errorPrintf("unknown command: /%s. try /help\n", command.c_str());
  }
}

void BitChatESP32::begin(uint32_t baud) {
  BitChatConfig config;
  config.baud = baud;
  begin(config);
}

void BitChatESP32::begin(const char *name, uint32_t baud) {
  BitChatConfig config;
  config.nickname = name;
  config.baud = baud;
  begin(config);
}

void BitChatESP32::begin(const String &name, uint32_t baud) {
  BitChatConfig config;
  config.nickname = name.c_str();
  config.baud = baud;
  begin(config);
}

void BitChatESP32::begin(const BitChatConfig &config) {
  serialOutputEnabled = config.serialOutput;
  serialCommandsEnabled = config.serialCommands;

  if (config.serialOutput || config.serialCommands) {
    Serial.begin(config.baud);
    Serial.setTimeout(20);
    if (config.waitForSerial) waitForSerialMonitor();
    delay(200);
    bootLog("serial ready");
  }

  bootLog("initializing libsodium");
  if (sodium_init() < 0) {
    if (serialOutputEnabled) Serial.println("libsodium init failed");
    while (true) delay(1000);
  }

  bootLog("loading identity");
  loadOrCreateIdentity();
  if (config.nickname != nullptr && strlen(config.nickname) > 0) {
    setNicknameInternal(String(config.nickname), config.persistNickname);
  }

  bootLog("creating BLE receive queue");
  rxQueue = xQueueCreate(12, sizeof(RxChunk *));
  if (rxQueue == nullptr) {
    if (serialOutputEnabled) Serial.println("BLE receive queue allocation failed");
    while (true) delay(1000);
  }
  bootLog("starting BLE");
  startBle();
  bootLog("BLE started");

  if (config.printStartupGuide) printStartupGuide();
  if (debugMode) {
    debugPrintf("service UUID: %s\n", BITCHAT_SERVICE_UUID);
    debugPrintf("time now: %llu\n", (unsigned long long)nowMs());
  }

  if (config.autoAnnounce) sendAnnounce(true);
  lastAliveAt = millis();
}

void BitChatESP32::loop() {
  handleDeferredBleEvents();
  drainRxQueue();

  if (serialCommandsEnabled && Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleSerialLine(line);
  }

  connectToPendingDevice();

  if (!remoteConnected && !connectInProgress && pendingDevice == nullptr && bleScan != nullptr) {
    uint32_t now = millis();
    if (!bleScan->isScanning() && now - lastScanAt > 8000) {
      lastScanAt = now;
      bleScan->start(5, false);
      bleScan->clearResults();
    }
  }

  sendAnnounce(false);
  printAlive();
  delay(20);
}

void BitChatESP32::printAbout() {
  printStartupGuide();
}

bool BitChatESP32::sendPublicMessage(const String &message) {
  return ::sendPublicMessage(message);
}

bool BitChatESP32::sendPrivateMessage(const String &peerSelector, const String &message) {
  PeerInfo *peer = findPeerBySelector(peerSelector);
  if (peer == nullptr) {
    errorPrintf("peer not found: %s\n", peerSelector.c_str());
    return false;
  }
  return sendPrivateMessageToPeer(peer, message);
}

bool BitChatESP32::sendPrivateMessage(const BitChatPeer &peer, const String &message) {
  return sendPrivateMessage(peer.id, message);
}

bool BitChatESP32::replyPrivate(const BitChatPeer &peer, const String &message) {
  return sendPrivateMessage(peer, message);
}

bool BitChatESP32::requestSync(const String &peerSelector) {
  String selector = peerSelector;
  selector.trim();
  if (selector.length() == 0 || selector == "all") {
    sendRequestSync(nullptr);
    return true;
  }

  PeerInfo *peer = findPeerBySelector(selector);
  if (peer == nullptr) {
    errorPrintf("peer not found: %s\n", selector.c_str());
    return false;
  }
  sendRequestSync(peer);
  return true;
}

void BitChatESP32::announce() {
  sendAnnounce(true);
}

void BitChatESP32::setNickname(const String &name) {
  if (!setNicknameInternal(name, true)) return;
  systemPrintf("nickname set to %s\n", ::nickname.c_str());
  sendAnnounce(true);
}

void BitChatESP32::setEpochMs(uint64_t epochMs) {
  ::setEpochMs(epochMs);
  systemPrintf("time set to %llu %s\n", (unsigned long long)::nowMs(), timeOf(::nowMs()).c_str());
  sendAnnounce(true);
}

void BitChatESP32::setTime(uint64_t epochMs) {
  setEpochMs(epochMs);
}

uint64_t BitChatESP32::now() const {
  return ::nowMs();
}

bool BitChatESP32::isClockSet() const {
  return timeSetThisBoot;
}

bool BitChatESP32::hasTime() const {
  return isClockSet();
}

String BitChatESP32::peerId() const {
  return hexOf(myPeerID, SENDER_ID_SIZE);
}

String BitChatESP32::nickname() const {
  return ::nickname;
}

size_t BitChatESP32::peerCount() const {
  return peers.size();
}

bool BitChatESP32::getPeer(size_t index, BitChatPeer &peer) const {
  if (index >= peers.size()) return false;
  peer = makePeerSnapshot(peers[index]);
  return true;
}

void BitChatESP32::enableSerialOutput(bool enabled) {
  serialOutputEnabled = enabled;
}

void BitChatESP32::enableSerialCommands(bool enabled) {
  serialCommandsEnabled = enabled;
}

void BitChatESP32::setDebug(bool enabled, bool persist) {
  debugMode = enabled;
  if (persist) prefs.putBool("debug", debugMode);
}

void BitChatESP32::setAlive(bool enabled, bool persist) {
  aliveMode = enabled;
  if (persist) prefs.putBool("alive", aliveMode);
}

void BitChatESP32::setQuiet(bool enabled, bool persist) {
  quietMode = enabled;
  if (persist) prefs.putBool("quiet", quietMode);
}

void BitChatESP32::setTimezoneMinutes(int16_t minutes, bool persist) {
  timeZoneOffsetMinutes = minutes;
  if (persist) prefs.putShort("tzMin", timeZoneOffsetMinutes);
}

void BitChatESP32::onPublicMessage(BitChatPublicMessageCallback callback) {
  publicMessageCallback = callback;
}

void BitChatESP32::onPrivateMessage(BitChatPrivateMessageCallback callback) {
  privateMessageCallback = callback;
}

void BitChatESP32::onPeer(BitChatPeerCallback callback) {
  peerCallback = callback;
}
