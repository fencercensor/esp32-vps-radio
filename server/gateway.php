<?php
declare(strict_types=1);

require_once __DIR__ . '/config-store.php';

// Long-lived, unbuffered response: one request represents one radio listener.
ignore_user_abort(false);
@set_time_limit(0);
@ini_set('max_execution_time', '0');
@ini_set('output_buffering', '0');
@ini_set('zlib.output_compression', '0');
while (ob_get_level() > 0) {
    @ob_end_clean();
}
ob_implicit_flush(true);

const STARTUP_TIMEOUT_SECONDS = 45.0;
const READ_SIZE = 16384;
// Keep about 13 seconds before playback begins and allow roughly 70 seconds
// to accumulate behind it. The extra latency is intentional: it isolates the
// listener from irregular HLS segment arrival.
const LEGACY_PREBUFFER_BYTES = 98304;
// The wired ESP already keeps 6.6 seconds locally. Keeping its server-side
// startup below that window lets a dropped HTTP session reconnect before the
// local queue empties, while legacy clients retain the deeper startup buffer.
const WIRED_PREBUFFER_BYTES = 24576;
const MAX_BUFFER_BYTES = 524288;
const ADPCM_BLOCK_BYTES = 1024;
// One 1,024-byte mono IMA-ADPCM block contains 2,041 samples at 14.7 kHz.
const INITIAL_BURST_BLOCKS = 1;
const ADPCM_BLOCK_DURATION_NS = 138843537;

function sendJson(int $status, array $payload): never
{
    http_response_code($status);
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    exit;
}

/** @param array<int, resource> $pipes */
function stopTranscoder($process, array &$pipes): void
{
    foreach ($pipes as $pipe) {
        if (is_resource($pipe)) {
            @fclose($pipe);
        }
    }
    $pipes = [];
    if (!is_resource($process)) {
        return;
    }
    $status = @proc_get_status($process);
    if (is_array($status) && ($status['running'] ?? false)) {
        @proc_terminate($process);
        usleep(100000);
        $status = @proc_get_status($process);
        if (is_array($status) && ($status['running'] ?? false)) {
            @proc_terminate($process, 9);
        }
    }
    @proc_close($process);
}

$stationId = isset($_GET['station']) ? strtolower((string) $_GET['station']) : '';
$wiredClient = isset($_GET['client']) && (string) $_GET['client'] === 'wired';
$outputChannels = 1;
$radioConfig = loadRadioConfig();
$station = null;
foreach (enabledStations($radioConfig) as $candidate) {
    if (hash_equals((string) $candidate['id'], $stationId)) {
        $station = $candidate;
        break;
    }
}
if (!is_array($station)) {
    sendJson(404, ['error' => 'unknown_station']);
}
$configVersion = (int) $radioConfig['version'];
$prebufferBytes = $wiredClient ? WIRED_PREBUFFER_BYTES : LEGACY_PREBUFFER_BYTES;

$ffmpeg = __DIR__ . '/bin/ffmpeg';
if (!is_executable($ffmpeg) || !function_exists('proc_open')) {
    sendJson(500, ['error' => 'gateway_not_ready']);
}

$process = null;
$pipes = [];
$firstChunk = '';
$chosenIndex = -1;
register_shutdown_function(static function () use (&$process, &$pipes): void {
    stopTranscoder($process, $pipes);
});

foreach ($station['sources'] as $index => $upstream) {
    $command = [
        $ffmpeg,
        '-nostdin',
        '-hide_banner',
        '-loglevel', 'error',
        '-rw_timeout', '15000000',
        '-reconnect', '1',
        '-reconnect_streamed', '1',
        '-reconnect_delay_max', '5',
        '-i', $upstream,
        '-vn',
        '-map_metadata', '-1',
        // Raise the average programme level by a conservative 3 dB while the
        // look-ahead limiter keeps peaks below full scale before ADPCM encoding.
        '-ac', (string) $outputChannels,
        '-ar', '14700',
        '-c:a', 'adpcm_ima_wav',
        '-block_size', '1024',
        '-threads', '1',
        '-f', 'wav',
        'pipe:1',
    ];
    $descriptors = [
        1 => ['pipe', 'w'],
        2 => ['file', '/dev/null', 'a'],
    ];
    $pipes = [];
    $process = @proc_open($command, $descriptors, $pipes, __DIR__, null, [
        'bypass_shell' => true,
    ]);
    if (!is_resource($process) || !isset($pipes[1])) {
        stopTranscoder($process, $pipes);
        $process = null;
        continue;
    }

    stream_set_blocking($pipes[1], false);
    $deadline = microtime(true) + STARTUP_TIMEOUT_SECONDS;
    while (microtime(true) < $deadline) {
        $chunk = @fread($pipes[1], READ_SIZE);
        if (is_string($chunk) && $chunk !== '') {
            $firstChunk .= $chunk;
            if (strlen($firstChunk) >= $prebufferBytes + 128) {
                break;
            }
        }
        $status = @proc_get_status($process);
        if (!is_array($status) || !($status['running'] ?? false)) {
            break;
        }
        usleep(50000);
    }

    if (strlen($firstChunk) >= $prebufferBytes + 128 &&
        substr($firstChunk, 0, 4) === 'RIFF' &&
        strpos($firstChunk, 'data') !== false) {
        $chosenIndex = (int) $index;
        break;
    }

    $firstChunk = '';
    stopTranscoder($process, $pipes);
    $process = null;
}

if (!is_resource($process) || $chosenIndex < 0 || !isset($pipes[1])) {
    sendJson(502, ['error' => 'all_station_sources_failed']);
}

http_response_code(200);
header('Content-Type: audio/x-wav');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('Pragma: no-cache');
header('Connection: close');
header('Content-Encoding: identity');
header('X-Accel-Buffering: no');
header('X-Audio-Format: ima-adpcm;rate=14700;channels=' . $outputChannels . ';block=1024;paced=' .
    ($wiredClient ? '0;client=wired;esp-buffer=6.66s' : '1;client=legacy;esp-buffer=12') .
    ';initial=1;prebuffer=' . ($wiredClient ? '3.3s' : '13s'));
header('X-Source-Index: ' . $chosenIndex);
header('X-Radio-Config-Version: ' . $configVersion);

$dataMarker = strpos($firstChunk, 'data');
if ($dataMarker === false) {
    stopTranscoder($process, $pipes);
    sendJson(502, ['error' => 'invalid_transcoder_header']);
}
$headerLength = $dataMarker + 8;
$initialLength = $headerLength + INITIAL_BURST_BLOCKS * ADPCM_BLOCK_BYTES;
if (strlen($firstChunk) < $initialLength) {
    stopTranscoder($process, $pipes);
    sendJson(502, ['error' => 'short_transcoder_buffer']);
}

// Legacy Bluetooth clients keep exact pacing. Wired clients have no radio
// coexistence constraint, so TCP backpressure is allowed to fill their larger
// local queue and rapidly restore it after a short Wi-Fi fade.
echo substr($firstChunk, 0, $initialLength);
flush();
$streamBuffer = substr($firstChunk, $initialLength);
$nextSendAt = hrtime(true) + ADPCM_BLOCK_DURATION_NS;

try {
    while (!connection_aborted()) {
        while (strlen($streamBuffer) < MAX_BUFFER_BYTES) {
            $room = MAX_BUFFER_BYTES - strlen($streamBuffer);
            $chunk = @fread($pipes[1], min(READ_SIZE, $room));
            if (!is_string($chunk) || $chunk === '') {
                break;
            }
            $streamBuffer .= $chunk;
        }

        if ($wiredClient && strlen($streamBuffer) >= ADPCM_BLOCK_BYTES) {
            $availableBlocks = intdiv(strlen($streamBuffer), ADPCM_BLOCK_BYTES);
            $sendBlocks = min(16, $availableBlocks);
            $sendBytes = $sendBlocks * ADPCM_BLOCK_BYTES;
            echo substr($streamBuffer, 0, $sendBytes);
            $streamBuffer = substr($streamBuffer, $sendBytes);
            flush();
            continue;
        }

        if (strlen($streamBuffer) >= ADPCM_BLOCK_BYTES) {
            $now = hrtime(true);
            if ($now < $nextSendAt) {
                usleep((int) min(20000, max(1000, ($nextSendAt - $now) / 1000)));
                continue;
            }
            // Never catch up with a network burst after a source delay.
            if ($now > $nextSendAt + ADPCM_BLOCK_DURATION_NS) {
                $nextSendAt = $now;
            }
            echo substr($streamBuffer, 0, ADPCM_BLOCK_BYTES);
            $streamBuffer = substr($streamBuffer, ADPCM_BLOCK_BYTES);
            flush();
            $nextSendAt += ADPCM_BLOCK_DURATION_NS;
            continue;
        }

        $status = @proc_get_status($process);
        if ((!is_array($status) || !($status['running'] ?? false)) &&
            feof($pipes[1])) {
            break;
        }
        usleep(5000);
    }
} finally {
    stopTranscoder($process, $pipes);
}
