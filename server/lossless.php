<?php
declare(strict_types=1);

// RZ1 is an opt-in transport. Every frame independently restores the exact
// original IMA-ADPCM bytes; the encoder, sample rate and audio never change.
// Header: "RZ", encoding (0=raw, 1=zlib), block count, raw length, wire length.
// Lengths are little-endian uint16. Maximum eight 1024-byte blocks per frame.
function radioLosslessFrame(string $audio): string
{
    $length = strlen($audio);
    if ($length < 1024 || $length > 8192 || $length % 1024 !== 0) {
        throw new InvalidArgumentException('Invalid radio frame size.');
    }
    $compressed = gzcompress($audio, 6);
    $useCompressed = is_string($compressed) && strlen($compressed) < $length;
    $payload = $useCompressed ? $compressed : $audio;
    return pack('a2CCvv', 'RZ', $useCompressed ? 1 : 0,
        intdiv($length, 1024), $length, strlen($payload)) . $payload;
}
