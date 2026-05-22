# BitChat Protocol Notes For ESP32 Arduino

This workspace keeps upstream iOS/macOS and Android reference checkouts under `upstream_references/`, which is ignored by Git. There was no existing Arduino/ESP32 implementation, so this repository root is now an Arduino library that ports the protocol pieces needed for public and private mesh chat. The serial-controlled reference sketch is `examples/SerialChat/SerialChat.ino`.

## Transport

- BLE service UUID:
  - mainnet/release: `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C`
  - iOS debug/testnet: `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5A`
- Characteristic UUID: `A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D`
- Characteristic properties: read, write, write without response, notify.
- Current iOS and Android both advertise and scan; the ESP32 library does the same.

## Packet Format

The current packet format lives in:

- iOS: `upstream_references/bitchat-ios/localPackages/BitFoundation/Sources/BitFoundation/BinaryProtocol.swift`
- Android: `upstream_references/bitchat-android/app/src/main/java/com/bitchat/android/protocol/BinaryProtocol.kt`

v1 packets use:

```text
version      1 byte
type         1 byte
ttl          1 byte
timestamp    8 bytes, big-endian milliseconds
flags        1 byte
payloadLen   2 bytes, big-endian
senderID     8 bytes
recipientID  8 bytes if flag 0x01 is set
payload      payloadLen bytes
signature    64 bytes if flag 0x02 is set
```

Implemented message types:

- `0x01` announce
- `0x02` public message
- `0x03` leave receive logging
- `0x10` Noise handshake
- `0x11` Noise encrypted private payload
- `0x21` request sync

Recognized but not implemented yet:

- `0x20` fragment
- `0x22` file transfer

## Identity And Signing

Current iOS/Android builds reject unsigned or unverified peers. A compatible ESP32 cannot just send a nickname; it must:

1. Generate a 32-byte Curve25519/X25519 public key.
2. Derive the 8-byte peer ID from `SHA256(noisePublicKey).prefix(8)`.
3. Generate an Ed25519 signing key pair.
4. Send an announce TLV payload:
   - `0x01`: nickname
   - `0x02`: 32-byte noise public key
   - `0x03`: 32-byte Ed25519 signing public key
5. Sign the canonical packet bytes with Ed25519.

The Arduino library uses ESP32 Arduino core 3.x libsodium for these operations and stores keys in `Preferences`.

Important signing detail: upstream signs `packet.toBinaryDataForSigning()`, which means the packet is copied with `ttl = 0`, no signature, and encoded with the default padding behavior. The library matches that behavior.

## Current ESP32 Scope

The ESP32 Arduino library currently supports:

- Mainnet BLE service by default.
- Switchable iOS debug/testnet UUID in `src/BitChatESP32Config.h` with `#define BITCHAT_USE_TESTNET 1`.
- Persistent ESP32 identity keys.
- Signed announces.
- Verified peer table from signed announces.
- Signed outbound public messages from Serial.
- Verified inbound public messages printed as a phone-style serial timeline.
- Noise XX private sessions using `Noise_XX_25519_ChaChaPoly_SHA256`.
- Private message send/receive with upstream `NoisePayloadType.privateMessage` and `PrivateMessagePacket` TLV encoding.
- Delivery ACK receive/send and read receipt send/receive for private messages.
- Basic dual BLE role: local GATT server plus client scanner/connector.
- BLE callbacks defer packet parsing, crypto, Serial logging, advertising restart, and scan stop into `BitChatESP32::loop()` to avoid overflowing the small NimBLE host task stack on RISC-V ESP32 boards.
- `REQUEST_SYNC` payload validation and announce response. The library responds to sync requests with a fresh signed announce on the same BLE link; directed sync responses are marked with the mutable RSR flag.
- Serial command surface for chat, identity, peer listing, sync, BLE controls, view controls, and diagnostics.
- Programmatic callbacks, `begin("nickname")`, `BitChatConfig`, and send/reply helpers for Arduino automation examples.

Serial commands:

```text
TEXT                  send public message
/say TEXT             send public message
/me ACTION            send public action text
/dm PEER TEXT         send private encrypted message
/read PEER MSG_ID     send private read receipt
/peers or /who        list verified peers
/sessions             list private Noise sessions
/sync [all|PEER]      request announce/message sync
/nick NAME            set nickname and announce
/id                   show peer and public keys
/time [EPOCH_MS|SEC]  show or set clock in epoch ms or seconds
/tz [+HHMM|MINUTES]   set serial display timezone
/status               show connection and memory state
/about                show startup description and clock requirement
/view                 redraw header
/clear                clear terminal
/debug [on|off]       enable or disable protocol diagnostics; default is off
/alive [on|off]       periodic status line
/quiet [on|off]       hide join/link system lines
/announce             send signed announce now
/scan                 start BLE scan now
/disconnect           disconnect outbound BLE client
/reboot               restart ESP32
```

Any non-command Serial line is sent as a signed public mesh message. Protocol diagnostics, including boot breadcrumbs, are hidden by default; enable them with `/debug on` and disable them with `/debug off`.

With `/debug on`, the library prints early boot breadcrumbs before BLE startup. It can also print an `alive` status line every five seconds when `/alive on` is enabled. Use Serial Monitor at `115200`. On ESP32-C3/S3 boards using native USB, enable `USB CDC On Boot` in the Arduino board menu or open the UART0 serial port instead.

## Known Gaps

- Private text messages, delivery ACKs, and read receipts are implemented. Encrypted file transfer and verification challenge/response payloads are recognized but not acted on by the serial UI.
- Noise transport replay-window enforcement and rekey policy are minimal in this ESP32 port; sessions can be reset by sending another private message if a peer loses state.
- Compression is not implemented. Short announces and short public messages are not compressed upstream, so this is acceptable for the first public-chat port.
- Large packet fragmentation is not implemented. The library limits outbound public text to 300 bytes to stay inside negotiated 517-byte BLE MTU after adding a 64-byte signature.
- Full GCS sync candidate comparison is not implemented yet. `REQUEST_SYNC` is handled enough for peer discovery by returning the ESP32 announcement, but the library does not yet retain and replay older public messages.
- ESP32 has no trusted wall clock by default. iOS currently accepts regular BLE packet timestamps only within about +/-5 minutes of the phone clock, so set current epoch milliseconds with `/time EPOCH_MS` after every reset.
- The classic ESP32 BLE stack plus libsodium is large; the verified build uses about 96% of a generic ESP32 app partition.
- If `nimble_host` reports stack overflow, keep all heavy work out of callbacks. The current library queues inbound chunks with a FreeRTOS queue and handles them from `BitChatESP32::loop()`.

## Build Check

Verified with:

```sh
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile --fqbn esp32:esp32:esp32 --libraries . examples/SerialChat
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli compile --fqbn esp32:esp32:esp32c3 --libraries . examples/SerialChat
```

Result:

```text
esp32:   Sketch uses 1265567 bytes (96%); globals use 46760 bytes (14%).
esp32c3: Sketch uses 858529 bytes (65%); globals use 23376 bytes (7%).
```
