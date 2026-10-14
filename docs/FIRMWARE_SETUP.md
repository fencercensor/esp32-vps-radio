# Firmware setup

Use a classic ESP32-WROOM-32 board with its internal DAC. This firmware does not support ESP32-S3, C3 or C6 and does not use Bluetooth.

## Private configuration

Copy `firmware/radio_config.example.h` to `firmware/radio_config.h` and change:

| Setting | Purpose |
| --- | --- |
| `RADIO_GATEWAY_HOST` | Numeric IPv4 address of your HTTPS origin. |
| `RADIO_GATEWAY_HTTP_HOST` | Domain matching the certificate and virtual host. |
| `RADIO_GATEWAY_PORT` | HTTPS port, normally 443. |
| `RADIO_SETUP_AP_PASSWORD` | Unique local setup password of 8 to 63 characters. |
| `RADIO_GATEWAY_CA` | PEM root CA verifying the server certificate chain. |

The example includes public ISRG Root X1. Replace it if your provider uses a different root. Normal certificate renewal under the same root does not require pinning a new leaf certificate. Do not use `setInsecure()` or disable hostname validation.

The device connects to the numeric IP but uses the configured domain for TLS SNI, certificate hostname checks and the HTTP Host header. Wi-Fi must allow NTP: connections fail closed until the clock is set for certificate validity checks.

Keep `radio_config.h` private. A compiled application can contain deployment settings; a full flash dump can also contain saved Wi-Fi credentials. Do not publish either.

## Build and upload

The current firmware was validated with **Arduino ESP32 Core 3.3.11**, `ESP32 Dev Module`, 4 MB flash and the **Default** partition scheme.

An Arduino sketch directory must match its main `.ino` filename. Copy the firmware files into a local directory named `vps_radio_test`, with your private configuration and both decoder headers beside `vps_radio_test.ino`.

```sh
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=default,FlashSize=4M vps_radio_test
arduino-cli upload --port YOUR_SERIAL_PORT --fqbn esp32:esp32:esp32:PartitionScheme=default,FlashSize=4M vps_radio_test
```

GPIO34 uses the legacy ADC1 driver to match the internal I2S DAC's legacy driver. Do not substitute Arduino 3.x `analogRead()` while keeping this DAC path: linking both ADC drivers causes a startup conflict.

Inspect the actual partition table before an application-only update. Match its layout and active application slot; do not overwrite NVS or erase the full flash during a routine update. Verify the uploaded image, then listen to AUX output.

### Apple Silicon tools

For `ctags: bad CPU type in executable`, use a native helper from the [official Arduino ctags source](https://github.com/arduino/ctags). In the `5.8-arduino11` source on recent macOS SDKs, rename its internal `__unused__` macro to `CTAGS_UNUSED` to avoid an SDK name collision.

Pass the helper directory as `tools.ctags.path` and `runtime.tools.ctags.path` through `--build-property`, without replacing global tools. Use a system temporary build directory if your OS restricts document-folder writes.

## Wi-Fi and controls

On first boot, connect to `ESP-Radio-Setup` with your configured setup password and open `http://192.168.4.1`. Save up to five 2.4 GHz networks in priority order. Credentials are stored in NVS.

| Control | Action |
| --- | --- |
| GPIO32 button | Previous station. |
| GPIO27 button | Next station. |
| GPIO33 button | Pause or resume. |
| Previous + Next for four seconds | Open local Wi-Fi setup. |
| GPIO34 potentiometer | Volume; ten seconds at zero stops live downloads. |

Raising volume reconnects to live audio and refills the queue. Pausing closes the stream. Neither action lowers audio quality.

## Runtime checks

Serial uses 115200 baud. A connected stream reports `tls=verified` and `transport=rz1`, or `transport=raw` if compression is unavailable.

- `wired=1`, `wifi=3`, `primed=1`: DAC, Wi-Fi and playback are ready.
- `buffer_blocks`: up to 48 encoded blocks, about 6.6 seconds.
- `bytes`, `raw_bytes`: received transport bytes and restored ADPCM bytes, excluding TCP/TLS overhead.
- `rz_frames`: decoded lossless frames; `standby=1`: zero-volume download suspension.
- Compare `underruns`, `dropped` and `reconnects` over time. Startup retries are included, so a nonzero total alone does not establish a new fault.

Keep runtime logs private. Advancing DAC counters are useful evidence; listening is the final audio check. Source failures and outages longer than the buffer can still interrupt playback.
