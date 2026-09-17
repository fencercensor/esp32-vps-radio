<?php
declare(strict_types=1);

require_once __DIR__ . '/config-store.php';

$config = loadRadioConfig();
$stations = enabledStations($config);

header('Content-Type: text/plain; charset=utf-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('X-Content-Type-Options: nosniff');
echo 'VERSION=' . (int) $config['version'] . "\n";
foreach ($stations as $station) {
    echo 'STATION=' . rawurlencode((string) $station['id']) . '|' .
        rawurlencode((string) $station['name']) . "\n";
}
