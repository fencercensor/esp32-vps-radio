# Server setup

Use Linux with Apache or LiteSpeed, PHP 8.1 or newer, `proc_open`, and executable FFmpeg. Each listener keeps a PHP request and an FFmpeg process active; hosting must allow long requests and sufficient CPU capacity.

## Install

1. Configure a domain and a trusted HTTPS certificate for the origin.
2. Upload `server/` contents into an `esp-radio` directory in the site's document root, including `.htaccess` files and `.user.ini`.
3. Obtain a trusted FFmpeg build suitable for the host. Place an executable copy or suitable link at `bin/ffmpeg`. No executable is published here.
4. Let PHP write to `data/` while denying web access to that directory.
5. Optionally copy `data/radio-config.example.json` to the private `data/radio-config.json`.
6. Complete admin password setup over HTTPS before allowing untrusted management access.

Example permissions, depending on the host's PHP ownership model:

```sh
chmod 755 bin/ffmpeg
chmod 750 data
chmod 640 data/.htaccess
chmod 644 *.php .htaccess .user.ini bin/.htaccess
```

The application refuses HTTP without embedding a deployment hostname. Configure any desired HTTPS redirects at the hosting layer with your trusted canonical domain. With a TLS-terminating proxy, configure the server's HTTPS status correctly; do not trust arbitrary client-supplied forwarding headers.

## Stations and administration

Open `https://radio.example.com/esp-radio/admin.php`, replacing the example domain. First-run setup stores a password hash in `data/admin.json`; subsequent visits show sign-in.

The catalog supports up to 16 stations. Each has an ID, a name, an enabled flag and 1 to 12 ordered source URLs. The gateway tries the next URL if a source fails to start. Save increments the version and writes the catalog atomically.

Devices refresh their catalogs before a new stream connection. Saving does not deliberately close an existing stream. Changes appear on a later connection or station change; the current firmware does not continuously poll an uninterrupted stream.

## Audio transport

FFmpeg outputs mono IMA-ADPCM at 14700 Hz with 1024-byte blocks. The same mono programme feeds both AUX channels. No gain boost, `volume` filter or `alimiter` is applied.

`client=wired&transport=rz1` enables lossless transport when `gzcompress` is available. Groups of up to eight original blocks are compressed with zlib, or sent raw if compression is not smaller. Audio content and sample rate are unchanged. Savings vary by programme; larger buffers alone do not reduce bandwidth.

Requests without RZ1 and servers without `gzcompress` retain raw transport. The firmware checks `X-Radio-Transport` before decoding. RZ1's WAV header is followed by framed data, so its response is not a standalone playable WAV file. Use the raw endpoint for ordinary WAV inspection; see [TRANSPORT.md](TRANSPORT.md).

## Deployment checks

Run `php -l` on every PHP file, then check:

- `/esp-radio/health.php`: status 200 and `"ok":true` when prerequisites are ready. `transports` includes `rz1` if compression is available.
- `/esp-radio/device-config.php`: lightweight public station catalog.
- `/esp-radio/gateway.php?station=cairo&client=wired`: raw audio.
- `/esp-radio/gateway.php?station=cairo&client=wired&transport=rz1`: negotiated lossless audio.
- `/esp-radio/data/admin.json`, `/esp-radio/data/radio-config.json`, `/esp-radio/bin/ffmpeg`: access must be denied.
- HTTP management: reject it, or redirect at the host to trusted HTTPS before handling credentials.

Health confirms prerequisites, not reachability of every station. Exercise real playback, reconnects and AUX listening.

## Updates and privacy

Back up outside the web root. Preserve admin hashes, the latest runtime catalog, FFmpeg and private firmware settings. Publish only examples; exclude deployment addresses, signed URLs, backups and logs. See [SECURITY.md](../SECURITY.md).
