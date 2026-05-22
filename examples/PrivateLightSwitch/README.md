# PrivateLightSwitch Example

This example controls a GPIO light or relay from encrypted BitChat private messages.

## Hardware

- ESP32 board.
- Built-in LED, or a relay/LED connected to `LIGHT_PIN`.

The sketch uses `LED_BUILTIN` when available, otherwise GPIO `2`. Change these constants in the sketch for your board:

```cpp
static const uint8_t LIGHT_PIN = LED_BUILTIN;
static const uint8_t LIGHT_ON_LEVEL = HIGH;
```

Use `LIGHT_ON_LEVEL = LOW` for active-low relay modules.

## Flash And Run

1. Install the `bitchat_esp32` library.
2. Open `File > Examples > BitChat ESP32 > PrivateLightSwitch`.
3. Select your ESP32 board and port.
4. Upload.
5. Open Serial Monitor at `115200`.
6. Set the clock:

   ```text
   /time EPOCH_MS
   ```

7. In BitChat on a phone, find `@esp32-light` and send it a private message.

## Private Commands

```text
on
off
toggle
status
```

The ESP32 replies privately with the new light state.

The example uses the simple project API:

```cpp
bitchat.onPrivateMessage(handlePrivateMessage);
bitchat.begin("esp32-light");
bitchat.replyPrivate(peer, "light is on");
```

The regular serial commands still work, so you can use `/peers`, `/sync`, `/status`, and `/debug on` while testing.
