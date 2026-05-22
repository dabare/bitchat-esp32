# SerialChat Example

`SerialChat` runs the full BitChat ESP32 library with a Serial Monitor interface. The example sketch is intentionally small:

```cpp
#include <BitChatESP32.h>

BitChatESP32 bitchat;

void setup() {
  bitchat.begin(115200);
}

void loop() {
  bitchat.loop();
}
```

## Flash

1. Install the `bitchat_esp32` library.
2. Open `File > Examples > BitChat ESP32 > SerialChat`.
3. Select your ESP32 board and port.
4. For ESP32-C3/S3 native USB boards, enable `USB CDC On Boot`.
5. Upload.
6. Open Serial Monitor at `115200`.

## First Run Flow

Set the ESP32 clock after every reset:

```text
/time EPOCH_MS
```

Phone clients reject stale or future BitChat packet timestamps. Use epoch milliseconds from `Date.now()` in a browser console, or from a computer shell:

```sh
date +%s000
```

Typical session:

```text
/time 1779274623804
/nick esp32-lab
/sync
/peers
hello public mesh
/dm 1 encrypted private hello
/sessions
```

## Commands

```text
TEXT                  send signed public message
/say TEXT             send signed public message
/me ACTION            send public action text
/dm PEER TEXT         send encrypted private message
/read PEER MSG_ID     send private read receipt
/peers or /who        list verified peers
/sessions             list private Noise sessions
/sync [all|PEER]      request announce/message sync
/nick NAME            set nickname and announce
/id                   show peer and public keys
/time [EPOCH_MS|SEC]  show or set clock
/tz [+HHMM|MINUTES]   set serial display timezone
/about                show startup description
/status               show connection and memory state
/view                 redraw compact header
/clear                clear terminal
/debug [on|off]       protocol diagnostics; default off
/alive [on|off]       periodic status line
/quiet [on|off]       hide join/link system lines
/announce             send signed announce now
/scan                 start BLE scan now
/disconnect           disconnect outbound BLE client
/reboot               restart ESP32
```

Peer selectors accepted by `/dm`, `/read`, and `/sync PEER`:

- Peer list index, for example `/dm 1 hello`.
- Exact nickname, case-insensitive.
- Peer ID prefix, for example `/dm d1e91f hello`.

## Debug Flow

Debug logs are disabled by default.

```text
/debug on
/status
/alive on
/debug off
```

Use `/alive on` only while troubleshooting because it prints a periodic status line.
