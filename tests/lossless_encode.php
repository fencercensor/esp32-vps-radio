<?php
declare(strict_types=1);

require_once dirname(__DIR__) . '/server/lossless.php';
if ($argc !== 3) {
    throw new InvalidArgumentException('Usage: lossless_encode.php INPUT OUTPUT');
}
$input = file_get_contents($argv[1]);
if (!is_string($input) || $input === '' || strlen($input) % 1024 !== 0) {
    throw new InvalidArgumentException('Input must contain complete ADPCM blocks.');
}
$wire = '';
for ($offset = 0; $offset < strlen($input); $offset += 8192) {
    $wire .= radioLosslessFrame(substr($input, $offset, 8192));
}
if (file_put_contents($argv[2], $wire) !== strlen($wire)) {
    throw new RuntimeException('Could not write the encoded fixture.');
}
