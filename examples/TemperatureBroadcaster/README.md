# TemperatureBroadcaster Example

This example periodically sends a signed public BitChat message with a temperature sample.

## Behavior

After the clock is set, the ESP32 announces as `@esp32-temp` and broadcasts:

```text
temperature sample: 24.3 C
```

The default sample is synthetic so the example works without extra hardware. To use an analog temperature sensor, set `TEMPERATURE_PIN` in the sketch to an ADC-capable GPIO. The example maps ADC `0..4095` to `0..50 C`.

## Flash And Run

1. Install the `BitChat ESP32` library.
2. Open `File > Examples > BitChat ESP32 > TemperatureBroadcaster`.
3. Select your ESP32 board and port.
4. Upload.
5. Open Serial Monitor at `115200`.
6. Set the clock:

   ```text
   /time EPOCH_MS
   ```

The example waits for `/time` before broadcasting because phone clients reject stale or future BitChat timestamps.

The example uses the simple project API:

```cpp
bitchat.begin("esp32-temp");
bitchat.sendPublicMessage("temperature sample: 24.3 C");
```

The regular serial commands still work, so you can use `/nick`, `/peers`, `/sync`, `/status`, and `/debug on` while testing.
