# Security and privacy

## Keep private

- `firmware/radio_config.h`: deployment IP, certificate hostname and setup password.
- `server/data/admin.json`: admin password hash.
- `server/data/radio-config.json`: live station catalog and any private or signed URLs.
- `server/bin/ffmpeg`: separately obtained executable.
- Build products, flash dumps, serial logs, backups and screenshots of private sessions.

Wi-Fi passwords are stored in NVS. A full flash dump can contain credentials even when source files do not. Publish neither flash dumps nor compiled firmware. Examples use a documentation-only IP and domain. The public root CA certificate is a trust anchor, not a secret or deployment certificate.

## Transport and management

The device connects to the configured numeric origin over HTTPS and validates its certificate chain and hostname. The domain is used for SNI and the HTTP Host header. There is no insecure mode or plaintext fallback; NTP sets the clock before certificate checks.

RZ1 restores exact ADPCM bytes with bounded frame sizes and zlib checksum validation. Invalid frames end the connection. Raw fallback means uncompressed audio inside verified TLS, not unencrypted HTTP.

Admin access requires HTTPS, secure cookies, strict session mode, password hashing and CSRF checks. Web access rules deny HTTP and direct downloads of runtime data or FFmpeg. Configure redirects at the host with a trusted canonical domain instead of an arbitrary request Host header.

Complete first-run admin setup before exposing it to untrusted visitors. An administrator can submit URLs that FFmpeg fetches from the server network; grant this access only to trusted operators. Keep PHP, FFmpeg and the operating system updated.

The local Wi-Fi portal requires setup mode. Use a unique setup password and close setup mode when finished. Catalog APIs are public; they contain station information, not passwords or Wi-Fi credentials.

## Public-release checks

Run `bash scripts/security-check.sh`, inspect staged paths and review the staged diff before a push. The check rejects private runtime files, common secret patterns, personal absolute paths, non-example IP addresses and Arabic text in public source/documentation. Manually review free-form URLs and visual assets too.

Stage reviewed source and examples only. Do not copy an entire live deployment into Git. Use a GitHub noreply commit email if keeping your contact email private. Ignore rules do not remove secrets already present in history.

If a credential is exposed, rotate it and remove it from history. Report security issues privately instead of posting passwords, keys or sensitive exploit details in public issues.

The wiring PDF has a generic English title and no author, creator, dates or XMP metadata. Keep replacement asset metadata free of private identifiers.
