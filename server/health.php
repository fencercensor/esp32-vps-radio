<?php
declare(strict_types=1);

require_once __DIR__ . '/config-store.php';

$ffmpeg = __DIR__ . '/bin/ffmpeg';
$ready = is_executable($ffmpeg) && function_exists('proc_open');
$stations = array_map(
    static fn(array $station): string => (string) $station['id'],
    enabledStations(loadRadioConfig())
);

http_response_code($ready ? 200 : 503);
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
echo json_encode([
    'ok' => $ready,
    'service' => 'esp-radio-gateway',
    'format' => 'ima-adpcm',
    'sample_rate' => 14700,
    'channels' => 1,
    'block_size' => 1024,
    'stations' => $stations,
], JSON_UNESCAPED_SLASHES);
