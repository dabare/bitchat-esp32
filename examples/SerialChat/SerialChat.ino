// SPDX-License-Identifier: BSD-3-Clause
/*
  BitChat ESP32 SerialChat example

  This example runs the full serial-controlled BitChat ESP32 peer.

  Startup flow:
  1. Open Serial Monitor at 115200 baud.
  2. The board prints the BitChat ESP32 description and clock requirement.
  3. Set epoch time after every reset:

       /time EPOCH_MS

     Examples:

       /time 1779274623804
       /time 1779274623

     Phone clients reject stale/future timestamps. Current iOS accepts regular
     BLE packets only within about +/-5 minutes of the phone clock.

  4. Optionally set your nickname:

       /nick esp32-lab

  5. Discover peers:

       /sync
       /peers

  6. Chat:

       hello public mesh
       /dm 1 encrypted private hello

  Full command list:

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
*/

#include <BitChatESP32.h>

BitChatESP32 bitchat;

void setup() {
  bitchat.begin(115200);
}

void loop() {
  bitchat.loop();
}
