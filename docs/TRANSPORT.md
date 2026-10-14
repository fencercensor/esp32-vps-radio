# RZ1 lossless transport

RZ1 reduces payload size without changing the ADPCM encoder or its decoded audio. Request `client=wired&transport=rz1` and check for `X-Radio-Transport: rz1`; otherwise read the original raw transport.

The body begins with the original RIFF/WAVE format header through the `data` marker. Following audio is framed. The WAV format must be mono IMA-ADPCM, 14700 Hz, 4 bits per sample, and a 1024-byte block alignment.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 bytes | ASCII `RZ`. |
| 2 | 1 byte | Encoding: 0 for raw bytes, 1 for zlib. |
| 3 | 1 byte | Complete ADPCM block count, 1 to 8. |
| 4 | 2 bytes | Restored byte count, little-endian. |
| 6 | 2 bytes | Payload byte count, little-endian. |
| 8 | Variable | Raw ADPCM or an independent complete zlib stream. |

Restored length equals block count times 1024 and is at most 8192. Payload length is positive and no larger than restored length. Raw frames require equal lengths. Compressed frames validate zlib checksums and must restore exactly the declared byte count. Unknown encodings, invalid sizes and incomplete frames are rejected.

The first block is sent promptly, followed by groups of up to eight blocks, about 1.1 seconds of audio. A final short group can contain fewer blocks when the transcoder ends. Each 8-byte frame header introduces small overhead for incompressible raw fallback; savings are not guaranteed for every programme.

The ESP32 uses its ROM miniz inflater with output limited to one group, then restores the original blocks into the audio queue. Inflate state and group buffers are kept off the task's stack. Catalog requests finish before the audio TLS connection opens to avoid simultaneous TLS-session memory pressure.

This is a transport protocol, not a new codec or HTTP gzip encoding. `bytes` includes frame headers; `raw_bytes` counts restored ADPCM. Neither includes TCP/TLS overhead or other internet traffic.
