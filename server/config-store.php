<?php
declare(strict_types=1);

const RADIO_DATA_DIR = __DIR__ . '/data';
const RADIO_CONFIG_FILE = RADIO_DATA_DIR . '/radio-config.json';
const RADIO_ADMIN_FILE = RADIO_DATA_DIR . '/admin.json';

function radioTextLength(string $value): int
{
    return function_exists('mb_strlen') ? mb_strlen($value, 'UTF-8') : strlen($value);
}

function defaultRadioConfig(): array
{
    return [
        'version' => 1,
        'updated_at' => gmdate('c'),
        'stations' => [
            [
                'id' => 'cairo',
                'name' => 'Quran Radio Cairo',
                'enabled' => true,
                'sources' => [
                    'http://stream.radiojar.com/8s5u5tpdtwzuv',
                ],
            ],
            [
                'id' => 'roqiah',
                'name' => 'Ruqyah Shariah',
                'enabled' => true,
                'sources' => [
                    'https://qurango.net/radio/roqiah',
                ],
            ],
        ],
    ];
}

function normalizeRadioConfig(array $input): array
{
    $rawStations = $input['stations'] ?? [];
    if (!is_array($rawStations) || count($rawStations) < 1 || count($rawStations) > 16) {
        throw new InvalidArgumentException('Add between 1 and 16 stations.');
    }

    $stations = [];
    $seen = [];
    foreach ($rawStations as $rawStation) {
        if (!is_array($rawStation)) {
            throw new InvalidArgumentException('Invalid station data.');
        }
        $id = strtolower(trim((string) ($rawStation['id'] ?? '')));
        $name = trim((string) ($rawStation['name'] ?? ''));
        if (!preg_match('/^[a-z0-9][a-z0-9_-]{0,31}$/', $id)) {
            throw new InvalidArgumentException('Station IDs accept lowercase letters, digits, hyphens and underscores only.');
        }
        if (isset($seen[$id])) {
            throw new InvalidArgumentException('Duplicate station ID: ' . $id);
        }
        if ($name === '' || radioTextLength($name) > 60) {
            throw new InvalidArgumentException('Station names must contain 1 to 60 characters.');
        }
        $seen[$id] = true;

        $rawSources = $rawStation['sources'] ?? [];
        if (!is_array($rawSources)) {
            throw new InvalidArgumentException('Invalid station sources: ' . $name);
        }
        $sources = [];
        foreach ($rawSources as $rawSource) {
            $source = trim((string) $rawSource);
            if ($source === '') {
                continue;
            }
            $parts = parse_url($source);
            $scheme = strtolower((string) ($parts['scheme'] ?? ''));
            if (($scheme !== 'http' && $scheme !== 'https') || empty($parts['host'])) {
                throw new InvalidArgumentException('Invalid source URL for station: ' . $name);
            }
            if (strlen($source) > 1500) {
                throw new InvalidArgumentException('Source URL is too long for station: ' . $name);
            }
            if (!in_array($source, $sources, true)) {
                $sources[] = $source;
            }
        }
        if (count($sources) < 1 || count($sources) > 12) {
            throw new InvalidArgumentException('Each station requires 1 to 12 source URLs.');
        }

        $stations[] = [
            'id' => $id,
            'name' => $name,
            'enabled' => (bool) ($rawStation['enabled'] ?? true),
            'sources' => $sources,
        ];
    }

    $enabledCount = count(array_filter($stations, static fn(array $station): bool => $station['enabled']));
    if ($enabledCount < 1) {
        throw new InvalidArgumentException('Enable at least one station.');
    }

    return [
        'version' => max(1, (int) ($input['version'] ?? 1)),
        'updated_at' => (string) ($input['updated_at'] ?? gmdate('c')),
        'stations' => $stations,
    ];
}

function loadRadioConfig(): array
{
    if (!is_file(RADIO_CONFIG_FILE)) {
        return defaultRadioConfig();
    }
    $raw = @file_get_contents(RADIO_CONFIG_FILE);
    $decoded = is_string($raw) ? json_decode($raw, true) : null;
    if (!is_array($decoded)) {
        return defaultRadioConfig();
    }
    try {
        return normalizeRadioConfig($decoded);
    } catch (Throwable $error) {
        return defaultRadioConfig();
    }
}

function saveRadioConfig(array $input): array
{
    $current = loadRadioConfig();
    $config = normalizeRadioConfig($input);
    $config['version'] = max((int) $current['version'] + 1, time());
    $config['updated_at'] = gmdate('c');

    if (!is_dir(RADIO_DATA_DIR) && !@mkdir(RADIO_DATA_DIR, 0750, true) && !is_dir(RADIO_DATA_DIR)) {
        throw new RuntimeException('Could not create the data directory.');
    }
    $json = json_encode($config, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
    if (!is_string($json)) {
        throw new RuntimeException('Could not encode the configuration.');
    }
    $temporary = RADIO_CONFIG_FILE . '.tmp-' . bin2hex(random_bytes(5));
    if (@file_put_contents($temporary, $json . "\n", LOCK_EX) === false || !@rename($temporary, RADIO_CONFIG_FILE)) {
        @unlink($temporary);
        throw new RuntimeException('Could not save the configuration.');
    }
    @chmod(RADIO_CONFIG_FILE, 0640);
    return $config;
}

function enabledStations(array $config): array
{
    return array_values(array_filter(
        $config['stations'],
        static fn(array $station): bool => (bool) $station['enabled']
    ));
}
