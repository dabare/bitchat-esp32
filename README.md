# BitChat ESP32 Arduino Library

This repository is a complete Arduino library for running a BitChat peer on ESP32. It lets an ESP32 join nearby BitChat iOS and Android clients over BLE mesh, send signed public messages, send and receive encrypted private messages, and expose the protocol through either Serial Monitor or a small Arduino callback API.

The repository root is the Arduino library root:

```text
library.properties
keywords.txt
LICENSE
NOTICE.md
src/
  BitChatESP32.h
  BitChatESP32.cpp
  BitChatESP32Config.h
examples/
  SerialChat/
    SerialChat.ino
    README.md
  PrivateLightSwitch/
    PrivateLightSwitch.ino
    README.md
  TemperatureBroadcaster/
    TemperatureBroadcaster.ino
    README.md
```

## What Works

- BLE service and characteristic compatibility with BitChat iOS and Android.
- Persistent ESP32 peer identity in `Preferences`.
- Curve25519/X25519 identity key and Ed25519 signing key generation.
- Peer ID derivation from `SHA256(noisePublicKey).prefix(8)`.
- Signed announce packets.
- Verified peer table from signed announces.
- Signed public messages from Serial.
- Inbound public messages displayed in a phone-style timeline.
- `REQUEST_SYNC` receive handling and signed announce response.
- Noise XX private sessions using `Noise_XX_25519_ChaChaPoly_SHA256`.
- Encrypted private text messages using the upstream `NoisePayloadType.privateMessage` and `PrivateMessagePacket` TLV format.
- Delivery ACK send/receive for private messages.
- Read receipt send/receive for private messages.
- Programmatic callbacks for public messages, private messages, and peer discovery.
- Programmatic public/private send helpers for automation sketches.
- Debug logs disabled by default, with `/debug on` when needed.
- BLE callbacks only queue received bytes; packet parsing and crypto run from `loop()` to avoid `nimble_host` stack overflow.

## Current Limits

- Public text is limited to 300 bytes because outbound fragmentation is not implemented.
- Private text is limited to 255 bytes because the upstream private-message TLV length field is one byte.
- Compression is not implemented.
- Fragment receive/reassembly is not implemented.
- Encrypted file transfer is recognized but not handled by the Serial UI.
- Verification challenge/response payloads are recognized but not handled by the Serial UI.
- Noise replay-window enforcement and rekey policy are minimal compared with the phone apps.
- The generic classic ESP32 build is close to the default app partition size limit.

## Hardware And Software

Recommended boards:

- ESP32-C3, ESP32-S3, or another ESP32 board supported by ESP32 Arduino core 3.x.
- Generic classic ESP32 works, but the build is large.

Required software:

- Arduino IDE or `arduino-cli`.
- ESP32 Arduino core 3.x.
- Serial Monitor at `115200`.

For ESP32-C3/S3 native USB boards, enable `USB CDC On Boot` in the Arduino board menu or use the UART0 serial port.

## Quick Use In Your Sketch

For a normal Serial-enabled device, the minimal project shape is:

```cpp
#include <BitChatESP32.h>

BitChatESP32 bitchat;

void onPrivateMessage(const BitChatPeer &peer, const String &message, const String &messageId) {
  (void)messageId;
  if (message == "status") {
    bitchat.replyPrivate(peer, "device is online");
  }
}

void setup() {
  bitchat.onPrivateMessage(onPrivateMessage);
  bitchat.begin("esp32-device");
}

void loop() {
  bitchat.loop();
}
```

`bitchat.loop()` handles BLE scanning, GATT client/server work, packet verification, Noise private sessions, delivery ACKs, read receipts, serial commands, and callbacks. Keep your callback short; set flags for longer hardware work.

## Install The Library In Arduino IDE

### Arduino IDE ZIP Install

Use this when you want to install this library without moving files manually.

1. Download `BitChat_ESP32-VERSION.zip` from this repository's release page.
2. In Arduino IDE, choose `Sketch > Include Library > Add .ZIP Library...`.
3. Select the downloaded `BitChat_ESP32-VERSION.zip` file.
4. Restart Arduino IDE if the examples do not appear immediately.
5. Open `File > Examples > BitChat ESP32 > SerialChat`.

Use the `BitChat_ESP32-VERSION.zip` release asset, not GitHub's generated `Source code` ZIP. The release ZIP contains this layout:

   ```text
   BitChat_ESP32/library.properties
   BitChat_ESP32/src/BitChatESP32.h
   BitChat_ESP32/examples/SerialChat/SerialChat.ino
   BitChat_ESP32/LICENSE
   BitChat_ESP32/README.md
   ```

Arduino IDE must see `library.properties`, `src/`, and `examples/` inside the installed library folder.

### Arduino IDE Manual Install

Copy this repository folder into your Arduino libraries folder:

```text
~/Documents/Arduino/libraries/bitchat-esp32
```

The installed layout should look like:

```text
~/Documents/Arduino/libraries/bitchat-esp32/library.properties
~/Documents/Arduino/libraries/bitchat-esp32/src/BitChatESP32.h
~/Documents/Arduino/libraries/bitchat-esp32/examples/SerialChat/SerialChat.ino
```

Avoid this incorrect nested layout:

```text
~/Documents/Arduino/libraries/bitchat-esp32/bitchat-esp32/library.properties
```

Restart Arduino IDE, then open:

```text
File > Examples > BitChat ESP32 > SerialChat
```

### Local Development Install

When compiling from this workspace, pass the library folder with `--libraries`:

```sh
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' compile --fqbn esp32:esp32:esp32c3 --libraries . examples/SerialChat
```

For a generic classic ESP32:

```sh
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' compile --fqbn esp32:esp32:esp32 --libraries . examples/SerialChat
```

The quotes are needed because the Arduino IDE path contains spaces.

## Flash The SerialChat Example

Using Arduino IDE:

1. Open `File > Examples > BitChat ESP32 > SerialChat`.
2. Choose your ESP32 board from `Tools > Board`.
3. Choose your serial port from `Tools > Port`.
4. For ESP32-C3/S3 native USB boards, enable `Tools > USB CDC On Boot > Enabled`.
5. Click `Upload`.
6. Open Serial Monitor at `115200`.

Using `arduino-cli` from this workspace:

```sh
'/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' upload --fqbn esp32:esp32:esp32c3 --port /dev/cu.usbmodemXXXX --libraries . examples/SerialChat
```

Replace `/dev/cu.usbmodemXXXX` with your board port.

## Included Examples

Open examples from `File > Examples > BitChat ESP32`.

```text
SerialChat              full Serial Monitor chat UI and command flow
PrivateLightSwitch      switch a GPIO light or relay from private messages
TemperatureBroadcaster  periodically broadcast a temperature sample
```

Each example folder includes its own README.

## BitChat Network Selection

The library uses the release/mainnet BitChat BLE UUID by default. Android and release iOS builds use mainnet.

To test against a debug/testnet iOS build, edit `src/BitChatESP32Config.h` before compiling:

```cpp
#define BITCHAT_USE_TESTNET 1
```

Keep it at `0` for Android and release iOS:

```cpp
#define BITCHAT_USE_TESTNET 0
```

## Important Clock Requirement

BitChat packets include Unix epoch millisecond timestamps. Phone clients reject stale or future packets for replay protection. The current iOS validator accepts regular BLE packets only within about `+/-5 minutes` of the phone clock.

Most ESP32 boards do not have a battery-backed real-time clock. After every reset or power cycle, set the current epoch time from Serial before expecting reliable phone interoperability.

At startup the example prints:

```text
set time now
  /time EPOCH_MS
  /time EPOCH_SECONDS
```

Examples:

```text
/time 1779274623804
/time 1779274623
```

Get epoch milliseconds on a computer:

```sh
date +%s000
```

Get epoch milliseconds in a browser console:

```js
Date.now()
```

When `/time` is set, the ESP32 immediately sends a fresh signed announce.

## Serial Startup Flow

1. Open Serial Monitor at `115200`.
2. Read the startup description and clock requirement.
3. Set the clock:

   ```text
   /time EPOCH_MS
   ```

4. Optionally set your nickname:

   ```text
   /nick esp32-lab
   ```

5. Wait for peers or request sync:

   ```text
   /sync
   /peers
   ```

6. Send public or private messages.

The full serial walkthrough is also documented in `examples/SerialChat/README.md` and in the comment at the top of `examples/SerialChat/SerialChat.ino`.

## Serial Commands

Public chat:

```text
TEXT                  send signed public message
/say TEXT             send signed public message
/me ACTION            send public action text
```

Private chat:

```text
/dm PEER TEXT         send encrypted private message
/read PEER MSG_ID     send private read receipt
/sessions             list private Noise sessions
```

Peers and sync:

```text
/peers
/who
/sync
/sync all
/sync PEER
```

Identity and clock:

```text
/nick NAME
/id
/time
/time EPOCH_MS
/time EPOCH_SECONDS
/tz +0530
/tz 330
```

View and diagnostics:

```text
/about
/status
/view
/clear
/debug on
/debug off
/alive on
/alive off
/quiet on
/quiet off
```

BLE controls:

```text
/announce
/scan
/disconnect
/reboot
```

Peer selectors accepted by `/dm`, `/read`, and `/sync PEER`:

- Peer list index, for example `/dm 1 hello`.
- Exact nickname, case-insensitive.
- Peer ID prefix, for example `/dm d1e91f hello`.

## Private Message Behavior

When you send:

```text
/dm 1 hello
```

the library:

1. Finds the verified peer.
2. Starts a Noise XX handshake if no private session exists.
3. Queues the private message while handshaking.
4. Encrypts `[NoisePayloadType.privateMessage][PrivateMessagePacket TLV]`.
5. Sends a `MSG_NOISE_ENCRYPTED` packet to the selected peer.
6. Prints your outgoing line immediately.

Inbound private messages print like:

```text
[12:34:56] alice -> you: hello
```

The ESP32 sends a delivery ACK after displaying a private message. It also prints delivery and read receipt events from peers.

## Minimal Sketch

The `SerialChat` example is intentionally small because the full protocol is in the library:

```cpp
#include <BitChatESP32.h>

BitChatESP32 bitchat;

void setup() {
  bitchat.begin("esp32-device");
}

void loop() {
  bitchat.loop();
}
```

## Programmatic API

Automation sketches can keep the Serial command interface and also use callbacks:

```cpp
#include <BitChatESP32.h>

BitChatESP32 bitchat;

void onPrivateMessage(const BitChatPeer &peer, const String &message, const String &messageId) {
  (void)messageId;
  if (message == "status") {
    bitchat.replyPrivate(peer, "device is online");
  }
}

void setup() {
  bitchat.onPrivateMessage(onPrivateMessage);
  bitchat.begin("esp32-device");
}

void loop() {
  bitchat.loop();
}
```

Useful methods:

```text
sendPublicMessage(TEXT)
sendPrivateMessage(PEER_SELECTOR, TEXT)
sendPrivateMessage(peer, TEXT)
replyPrivate(peer, TEXT)
requestSync()
announce()
setNickname(NAME)
setEpochMs(EPOCH_MS)
setTime(EPOCH_MS)
isClockSet()
hasTime()
peerCount()
getPeer(INDEX, peer)
enableSerialOutput(BOOL)
enableSerialCommands(BOOL)
setDebug(BOOL)
setAlive(BOOL)
setQuiet(BOOL)
setTimezoneMinutes(MINUTES)
onPublicMessage(callback)
onPrivateMessage(callback)
onPeer(callback)
```

`getPeer()` uses a zero-based index. Serial commands still use one-based peer numbers, for example `/dm 1 hello`.

Callbacks run from `BitChatESP32::loop()`, so keep them short. Long device work should set a flag and let your sketch handle it after `bitchat.loop()` returns.

## Customization

Use `BitChatConfig` when a project needs less Serial output, a custom boot flow, or a headless UI:

```cpp
BitChatConfig config;
config.nickname = "esp32-sensor";
config.baud = 115200;
config.serialOutput = false;       // no library printing to Serial
config.serialCommands = false;     // library will not consume Serial input
config.waitForSerial = false;
config.printStartupGuide = false;
config.autoAnnounce = true;

bitchat.begin(config);
```

You can also change runtime behavior with methods:

```cpp
bitchat.enableSerialCommands(false);
bitchat.enableSerialOutput(false);
bitchat.setDebug(true);
bitchat.setQuiet(true);
bitchat.setTimezoneMinutes(330);
```

Use `BitChatConfig` before `begin()`. Use setter methods after `begin()`.

## Debugging

Protocol diagnostics are off by default:

```text
/debug on
/debug off
```

Use `/alive on` for a periodic connection, peer, heap, and time line.

Use `/status` to check:

- peer ID
- peer count
- private session count
- BLE server/client link state
- free heap
- current ESP32 time
- whether `/time` was set this boot

If phone clients do not show the ESP32:

1. Set the clock with `/time EPOCH_MS`.
2. Confirm mainnet/testnet UUID selection.
3. Run `/announce`.
4. Run `/sync`.
5. Turn on `/debug on`.
6. Confirm the phone is using nearby BLE mesh, not only Nostr/geohash transport.

## Stack Overflow Avoidance

Earlier versions could overflow `nimble_host` when doing Serial logging, packet parsing, or crypto inside BLE callbacks. The library avoids that pattern:

- BLE write/notify callbacks copy data into a FreeRTOS queue.
- `BitChatESP32::loop()` drains the queue.
- Packet decoding, signature verification, Noise handshakes, encryption, and Serial output happen in the main Arduino task.

If `nimble_host` stack overflow returns, audit any new callback code first.

## Verified Build Sizes

Current verified example build results:

```text
SerialChat esp32c3:              858529 bytes (65%); globals 23376 bytes (7%).
SerialChat esp32:               1265567 bytes (96%); globals 46760 bytes (14%).
PrivateLightSwitch esp32c3:      884699 bytes (67%); globals 24476 bytes (7%).
PrivateLightSwitch esp32:       1271175 bytes (96%); globals 46768 bytes (14%).
TemperatureBroadcaster esp32c3:  864505 bytes (65%); globals 23384 bytes (7%).
TemperatureBroadcaster esp32:   1269999 bytes (96%); globals 46776 bytes (14%).
```

## License

This library is licensed under the BSD 3-Clause License. Commercial use is allowed. Keep the license text with source or binary distributions, and give credit to the BitChat ESP32 Arduino port and upstream BitChat project where practical.

## Protocol Notes

More detailed protocol notes are in:

```text
docs/ESP32_PROTOCOL_ANALYSIS.md
```
