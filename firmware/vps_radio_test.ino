#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <atomic>
#include <driver/i2s.h>
#include <lwip/sockets.h>

#include "ima_adpcm.h"
#include "radio_config.h"

namespace {

constexpr char kSetupApName[] = "ESP-Radio-Setup";
constexpr char kSetupApPassword[] = RADIO_SETUP_AP_PASSWORD;
constexpr char kGatewayHost[] = RADIO_GATEWAY_HOST;
constexpr char kGatewayHttpHost[] = RADIO_GATEWAY_HTTP_HOST;
constexpr uint16_t kGatewayPort = RADIO_GATEWAY_PORT;
constexpr char kDeviceConfigPath[] = "/esp-radio/device-config.php";
constexpr uint8_t kPreviousPin = 32;
constexpr uint8_t kPlayPausePin = 33;
constexpr uint8_t kNextPin = 27;
constexpr uint8_t kVolumePin = 34;

struct Station {
  char id[33];
  char name[129];
};

struct WifiCredential {
  String ssid;
  String password;
};

constexpr uint8_t kMaxStations = 16;
constexpr uint8_t kMaxWifiNetworks = 5;
Station stations[kMaxStations];
uint8_t stationCount = 0;
uint32_t radioConfigVersion = 0;
WifiCredential wifiNetworks[kMaxWifiNetworks];
uint8_t wifiNetworkCount = 0;
uint8_t wifiAttemptIndex = 0;

constexpr uint32_t kInputSampleRate = 14700;
constexpr uint8_t kOutputOversample = 8;
constexpr uint32_t kOutputSampleRate = kInputSampleRate * kOutputOversample;
constexpr size_t kMaxAdpcmBlock = 1024;
constexpr size_t kMaxDecodedSamples = 1 + 2 * (kMaxAdpcmBlock - 4);
// Keep compressed ADPCM blocks instead of expanded PCM. Forty-eight mono
// blocks hold about 6.66 seconds while using less than 50 KB of DRAM.
constexpr size_t kEncodedRingBlocks = 48;
constexpr size_t kEncodedStartBlocks = 36;
constexpr size_t kEncodedLowWaterBlocks = 24;
constexpr size_t kEncodedHighWaterBlocks = 44;
constexpr uint16_t kFadeFrames = 1024;
constexpr i2s_port_t kAudioI2sPort = I2S_NUM_0;
constexpr size_t kDacChunkFrames = 256;

struct EncodedBlock {
  uint32_t generation;
  uint16_t size;
  uint8_t data[kMaxAdpcmBlock];
};

struct WavFormat {
  uint16_t formatTag = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t blockAlign = 0;
  uint16_t bitsPerSample = 0;
  uint16_t samplesPerBlock = 0;
};

// The network task is the sole producer; the wired DAC task is the sole
// consumer and decodes one block only when it is needed for playback.
EncodedBlock encodedRing[kEncodedRingBlocks];
int16_t audioDecoded[kMaxDecodedSamples];
std::atomic<uint32_t> encodedWriteCount{0};
std::atomic<uint32_t> encodedReadCount{0};

std::atomic<bool> playbackPrimed{false};
std::atomic<bool> wiredAudioReady{false};
std::atomic<uint32_t> audioWrites{0};
std::atomic<uint64_t> outputFrames{0};
std::atomic<uint32_t> underruns{0};
std::atomic<uint32_t> decodedBlocks{0};
std::atomic<uint32_t> droppedInputBlocks{0};
std::atomic<uint32_t> backpressureWaits{0};
std::atomic<uint64_t> networkBytes{0};
std::atomic<uint32_t> networkReconnects{0};
std::atomic<uint32_t> streamGeneration{1};
std::atomic<uint8_t> selectedStation{0};
std::atomic<uint8_t> volumePercent{100};
std::atomic<bool> radioPaused{false};
// -1 repeats one sample occasionally, 0 consumes exactly one input sample per
// output frame, +1 drops one occasionally. This keeps the independent VPS and
// DAC clocks from slowly filling or draining the PCM queue.
std::atomic<int8_t> clockTrimMode{0};

bool wifiAttempted = false;
bool wifiAttemptActive = false;
bool networkTaskStarted = false;
bool audioOutputTaskStarted = false;
std::atomic<bool> wifiPortalActive{false};
std::atomic<bool> restartScheduled{false};
uint32_t lastWifiRetryAt = 0;
uint32_t wifiAttemptStartedAt = 0;
std::atomic<uint32_t> restartAt{0};
DNSServer dnsServer;
WebServer setupServer(80);

void restartStreamBuffer();

struct Button {
  explicit Button(uint8_t buttonPin) : pin(buttonPin) {}
  uint8_t pin;
  bool lastReading = HIGH;
  bool stableReading = HIGH;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;

  void begin() {
    pinMode(pin, INPUT_PULLUP);
    lastReading = stableReading = digitalRead(pin);
    changedAt = millis();
  }

  bool released(uint32_t *heldMs = nullptr) {
    const bool reading = digitalRead(pin);
    if (reading != lastReading) {
      lastReading = reading;
      changedAt = millis();
    }
    if (millis() - changedAt >= 35 && stableReading != lastReading) {
      const bool wasPressed = stableReading == LOW;
      stableReading = lastReading;
      if (stableReading == LOW) {
        pressedAt = millis();
        return false;
      }
      if (wasPressed && stableReading == HIGH) {
        if (heldMs != nullptr) *heldMs = millis() - pressedAt;
        return true;
      }
    }
    return false;
  }

  bool isPressed() const { return stableReading == LOW; }
};

Button previousButton{kPreviousPin};
Button playPauseButton{kPlayPausePin};
Button nextButton{kNextPin};
float filteredPotReading = 0.0f;
int lastVolumePercent = -1;

void loadDefaultStations() {
  stationCount = 2;
  strlcpy(stations[0].id, "cairo", sizeof(stations[0].id));
  strlcpy(stations[0].name, "Quran Radio Cairo", sizeof(stations[0].name));
  strlcpy(stations[1].id, "roqiah", sizeof(stations[1].id));
  strlcpy(stations[1].name, "Ruqyah Shariah", sizeof(stations[1].name));
}

String urlDecode(const String &input) {
  String output;
  output.reserve(input.length());
  for (size_t index = 0; index < input.length(); ++index) {
    const char value = input[index];
    if (value == '%' && index + 2 < input.length()) {
      const char encoded[3] = {input[index + 1], input[index + 2], 0};
      char *end = nullptr;
      const long decoded = strtol(encoded, &end, 16);
      if (end == encoded + 2) {
        output += static_cast<char>(decoded);
        index += 2;
        continue;
      }
    }
    output += value == '+' ? ' ' : value;
  }
  return output;
}

void loadCachedRadioConfig() {
  loadDefaultStations();
  Preferences settings;
  if (!settings.begin("esp32-radio", true)) return;
  // A brand-new board starts at version 0 so its first gateway connection
  // pulls the shared server configuration before normal playback.
  radioConfigVersion = settings.getUInt("cfg_version", 0);
  const uint8_t cachedCount = min(settings.getUChar("station_count", 0),
                                  static_cast<uint8_t>(kMaxStations));
  if (cachedCount > 0) {
    stationCount = 0;
    for (uint8_t index = 0; index < cachedCount; ++index) {
      const String id = settings.getString(("sid" + String(index)).c_str(), "");
      const String name = settings.getString(("sname" + String(index)).c_str(), "");
      if (id.isEmpty() || name.isEmpty()) continue;
      strlcpy(stations[stationCount].id, id.c_str(), sizeof(stations[stationCount].id));
      strlcpy(stations[stationCount].name, name.c_str(), sizeof(stations[stationCount].name));
      ++stationCount;
    }
    if (stationCount == 0) loadDefaultStations();
  }
  settings.end();
}

void saveCachedRadioConfig(uint32_t version, const Station *newStations,
                           uint8_t newCount) {
  Preferences settings;
  if (!settings.begin("esp32-radio", false)) return;
  const uint8_t oldCount = settings.getUChar("station_count", 0);
  settings.putUInt("cfg_version", version);
  settings.putUChar("station_count", newCount);
  for (uint8_t index = 0; index < newCount; ++index) {
    settings.putString(("sid" + String(index)).c_str(), newStations[index].id);
    settings.putString(("sname" + String(index)).c_str(), newStations[index].name);
  }
  for (uint8_t index = newCount; index < oldCount; ++index) {
    settings.remove(("sid" + String(index)).c_str());
    settings.remove(("sname" + String(index)).c_str());
  }
  settings.end();
}

void loadWifiNetworks() {
  wifiNetworkCount = 0;
  Preferences settings;
  if (!settings.begin("esp32-radio", true)) return;
  const uint8_t savedCount = min(settings.getUChar("wifi_count", 0),
                                 static_cast<uint8_t>(kMaxWifiNetworks));
  for (uint8_t index = 0; index < savedCount; ++index) {
    const String ssid = settings.getString(("wssid" + String(index)).c_str(), "");
    if (ssid.isEmpty()) continue;
    wifiNetworks[wifiNetworkCount].ssid = ssid;
    wifiNetworks[wifiNetworkCount].password =
        settings.getString(("wpass" + String(index)).c_str(), "");
    ++wifiNetworkCount;
  }
  if (wifiNetworkCount == 0) {
    const String legacySsid = settings.getString("wifi_ssid", "");
    if (!legacySsid.isEmpty()) {
      wifiNetworks[0].ssid = legacySsid;
      wifiNetworks[0].password = settings.getString("wifi_pass", "");
      wifiNetworkCount = 1;
    }
  }
  settings.end();
}

void saveWifiNetworks() {
  Preferences settings;
  if (!settings.begin("esp32-radio", false)) return;
  const uint8_t oldCount = settings.getUChar("wifi_count", 0);
  settings.putUChar("wifi_count", wifiNetworkCount);
  for (uint8_t index = 0; index < wifiNetworkCount; ++index) {
    settings.putString(("wssid" + String(index)).c_str(), wifiNetworks[index].ssid);
    settings.putString(("wpass" + String(index)).c_str(), wifiNetworks[index].password);
  }
  for (uint8_t index = wifiNetworkCount; index < oldCount; ++index) {
    settings.remove(("wssid" + String(index)).c_str());
    settings.remove(("wpass" + String(index)).c_str());
  }
  settings.end();
}

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

bool startWiredAudio() {
  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX |
                                        I2S_MODE_DAC_BUILT_IN);
  config.sample_rate = kOutputSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 8;
  config.dma_buf_len = kDacChunkFrames;
  config.use_apll = true;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  if (i2s_driver_install(kAudioI2sPort, &config, 0, nullptr) != ESP_OK) {
    Serial.println("[AUDIO] I2S driver install failed");
    return false;
  }
  if (i2s_set_pin(kAudioI2sPort, nullptr) != ESP_OK ||
      // Drive the two independent AUX filter branches from the two built-in
      // DAC outputs: GPIO25 to Left and GPIO26 to Right.
      i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK ||
      i2s_set_clk(kAudioI2sPort, kOutputSampleRate,
                  I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) {
    Serial.println("[AUDIO] internal DAC setup failed");
    i2s_driver_uninstall(kAudioI2sPort);
    return false;
  }

  // The built-in DAC consumes the high 8 bits as unsigned audio. Writing the
  // midpoint first keeps the wired audio output quiet while the buffer fills.
  uint16_t silence[kDacChunkFrames * 2];
  for (size_t index = 0; index < kDacChunkFrames * 2; ++index)
    silence[index] = 0x8000;
  size_t written = 0;
  i2s_write(kAudioI2sPort, silence, sizeof(silence), &written, portMAX_DELAY);
  wiredAudioReady = written == sizeof(silence);
  Serial.printf("[AUDIO] wired DAC GPIO25+GPIO26 ready=%d input=%u output=%u oversample=%u\n",
                wiredAudioReady.load(), kInputSampleRate, kOutputSampleRate,
                kOutputOversample);
  return wiredAudioReady.load();
}

void wiredAudioTask(void *) {
  uint16_t dmaFrames[kDacChunkFrames * 2];
  uint32_t activeGeneration = streamGeneration.load();
  size_t decodedCount = 0;
  size_t decodedIndex = 0;
  uint16_t trimCounter = 0;
  uint16_t fadeInPosition = kFadeFrames;
  uint8_t spanPosition = 0;
  uint8_t spanLength = kOutputOversample;
  int16_t fromSample = 0;
  int16_t toSample = 0;
  int16_t lastOutput = 0;
  int32_t quantizationError = 0;
  bool haveSpan = false;

  auto takeNextSample = [&](int16_t &sample) -> bool {
    while (true) {
      if (decodedIndex < decodedCount) {
        sample = audioDecoded[decodedIndex++];
        return true;
      }

      const uint32_t read =
          encodedReadCount.load(std::memory_order_relaxed);
      const uint32_t write =
          encodedWriteCount.load(std::memory_order_acquire);
      if (read == write) return false;

      EncodedBlock &block = encodedRing[read % kEncodedRingBlocks];
      if (block.generation != activeGeneration) {
        encodedReadCount.store(read + 1, std::memory_order_release);
        continue;
      }

      decodedCount = ImaAdpcm::decodeMonoBlock(
          block.data, block.size, audioDecoded, kMaxDecodedSamples);
      decodedIndex = 0;
      encodedReadCount.store(read + 1, std::memory_order_release);
      if (decodedCount == 0) {
        ++droppedInputBlocks;
        continue;
      }
      ++decodedBlocks;
    }
  };

  while (true) {
    for (size_t frame = 0; frame < kDacChunkFrames; ++frame) {
      const uint32_t wantedGeneration = streamGeneration.load();
      if (activeGeneration != wantedGeneration) {
        activeGeneration = wantedGeneration;
        playbackPrimed = false;
        clockTrimMode = 0;
        trimCounter = 0;
        spanPosition = 0;
        spanLength = kOutputOversample;
        haveSpan = false;
        decodedCount = 0;
        decodedIndex = 0;
        quantizationError = 0;
      }

      const uint32_t queuedBlocks =
          encodedWriteCount.load(std::memory_order_acquire) -
          encodedReadCount.load(std::memory_order_relaxed);
      bool primed = playbackPrimed.load();
      if (!radioPaused.load() && !primed &&
          queuedBlocks >= kEncodedStartBlocks &&
          takeNextSample(fromSample) && takeNextSample(toSample)) {
        spanPosition = 0;
        spanLength = kOutputOversample;
        haveSpan = true;
        playbackPrimed = true;
        primed = true;
        fadeInPosition = 0;
      }

      int16_t output = 0;
      if (!radioPaused.load() && primed && haveSpan) {
        const int32_t difference =
            static_cast<int32_t>(toSample) - fromSample;
        output = static_cast<int16_t>(
            static_cast<int32_t>(fromSample) +
            difference * spanPosition / spanLength);
        if (fadeInPosition < kFadeFrames) {
          output = static_cast<int32_t>(output) * fadeInPosition / kFadeFrames;
          ++fadeInPosition;
        }

        if (++spanPosition >= spanLength) {
          fromSample = toSample;
          if (takeNextSample(toSample)) {
            const uint32_t latestQueuedBlocks =
                encodedWriteCount.load(std::memory_order_acquire) -
                encodedReadCount.load(std::memory_order_relaxed);
            if (latestQueuedBlocks >= kEncodedHighWaterBlocks)
              clockTrimMode = 1;
            else if (latestQueuedBlocks <= kEncodedLowWaterBlocks)
              clockTrimMode = -1;
            else
              clockTrimMode = 0;

            spanLength = kOutputOversample;
            if (++trimCounter >= 256) {
              trimCounter = 0;
              if (clockTrimMode.load() > 0)
                spanLength = kOutputOversample - 1;
              else if (clockTrimMode.load() < 0)
                spanLength = kOutputOversample + 1;
            }
            spanPosition = 0;
          } else {
            playbackPrimed = false;
            haveSpan = false;
            ++underruns;
          }
        }
      } else {
        // Short ramp to the DAC midpoint avoids a hard edge when pausing or
        // changing stations.
        if (lastOutput > 256)
          output = lastOutput - 256;
        else if (lastOutput < -256)
          output = lastOutput + 256;
      }

      lastOutput = output;
      const int16_t scaled = static_cast<int32_t>(output) *
                             volumePercent.load() / 100;
      uint16_t dacValue = 0x8000;
      if (scaled == 0 && lastOutput == 0) {
        quantizationError = 0;
      } else {
        // First-order error feedback moves the 8-bit DAC's quantization noise
        // above the useful 7.35 kHz audio band. The existing 1k/10nF analog
        // filter then attenuates it instead of leaving it across speech.
        int32_t shaped = static_cast<int32_t>(scaled) + quantizationError;
        if (shaped > 32767) shaped = 32767;
        if (shaped < -32768) shaped = -32768;
        int32_t quantized = (shaped + 32768 + 128) >> 8;
        if (quantized > 255) quantized = 255;
        if (quantized < 0) quantized = 0;
        const int32_t reconstructed = (quantized << 8) - 32768;
        quantizationError = shaped - reconstructed;
        dacValue = static_cast<uint16_t>(quantized << 8);
      }
      dmaFrames[frame * 2] = dacValue;
      dmaFrames[frame * 2 + 1] = dacValue;
    }

    size_t written = 0;
    if (i2s_write(kAudioI2sPort, dmaFrames, sizeof(dmaFrames), &written,
                  portMAX_DELAY) == ESP_OK && written == sizeof(dmaFrames)) {
      ++audioWrites;
      outputFrames.fetch_add(kDacChunkFrames);
    }
  }
}

bool readExact(WiFiClient &client, uint8_t *destination, size_t length,
               uint32_t timeoutMs = 15000) {
  size_t received = 0;
  uint32_t lastProgressAt = millis();
  while (received < length) {
    const int available = client.available();
    if (available > 0) {
      const size_t wanted = min(length - received, static_cast<size_t>(available));
      const int count = client.read(destination + received, wanted);
      if (count > 0) {
        received += count;
        lastProgressAt = millis();
        continue;
      }
    }
    if (!client.connected() || millis() - lastProgressAt >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

bool skipExact(WiFiClient &client, uint32_t length) {
  uint8_t scratch[64];
  while (length != 0) {
    const size_t part = min(static_cast<uint32_t>(sizeof(scratch)), length);
    if (!readExact(client, scratch, part)) return false;
    length -= part;
  }
  return true;
}

bool readLine(WiFiClient &client, char *line, size_t capacity) {
  if (capacity == 0) return false;
  size_t length = 0;
  uint32_t lastProgressAt = millis();
  while (millis() - lastProgressAt < 30000) {
    if (client.available() == 0) {
      if (!client.connected()) return false;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const int value = client.read();
    if (value < 0) continue;
    lastProgressAt = millis();
    if (value == '\n') {
      line[length] = '\0';
      return true;
    }
    if (value != '\r' && length + 1 < capacity) line[length++] = value;
  }
  return false;
}

bool readHttpHeaders(WiFiClient &client,
                     uint32_t *announcedConfigVersion = nullptr) {
  char line[192];
  if (!readLine(client, line, sizeof(line)) || strstr(line, " 200 ") == nullptr)
    return false;
  while (readLine(client, line, sizeof(line))) {
    if (line[0] == '\0') return true;
    constexpr char kConfigVersionHeader[] = "X-Radio-Config-Version:";
    if (announcedConfigVersion != nullptr &&
        strncasecmp(line, kConfigVersionHeader,
                    strlen(kConfigVersionHeader)) == 0) {
      *announcedConfigVersion =
          strtoul(line + strlen(kConfigVersionHeader), nullptr, 10);
    }
  }
  return false;
}

bool fetchRemoteRadioConfig() {
  WiFiClient client;
  client.setNoDelay(true);
  if (!client.connect(kGatewayHost, kGatewayPort)) {
    Serial.println("[CONFIG] server connection failed");
    return false;
  }
  int receiveBufferBytes = 2048;
  client.setSocketOption(SO_RCVBUF, reinterpret_cast<char *>(&receiveBufferBytes),
                         sizeof(receiveBufferBytes));
  client.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                kDeviceConfigPath, kGatewayHttpHost);
  if (!readHttpHeaders(client)) {
    client.stop();
    Serial.println("[CONFIG] invalid server response");
    return false;
  }

  uint32_t newVersion = 0;
  Station *newStations = static_cast<Station *>(
      calloc(kMaxStations, sizeof(Station)));
  if (newStations == nullptr) {
    client.stop();
    Serial.println("[CONFIG] not enough memory for update");
    return false;
  }
  uint8_t newCount = 0;
  char line[512];
  while (readLine(client, line, sizeof(line))) {
    if (strncmp(line, "VERSION=", 8) == 0) {
      newVersion = strtoul(line + 8, nullptr, 10);
    } else if (strncmp(line, "STATION=", 8) == 0 && newCount < kMaxStations) {
      const String record(line + 8);
      const int separator = record.indexOf('|');
      if (separator <= 0) continue;
      const String id = urlDecode(record.substring(0, separator));
      const String name = urlDecode(record.substring(separator + 1));
      if (id.isEmpty() || name.isEmpty()) continue;
      strlcpy(newStations[newCount].id, id.c_str(), sizeof(newStations[newCount].id));
      strlcpy(newStations[newCount].name, name.c_str(), sizeof(newStations[newCount].name));
      ++newCount;
    }
  }
  client.stop();
  if (newVersion == 0 || newCount == 0) {
    free(newStations);
    Serial.println("[CONFIG] empty server configuration ignored");
    return false;
  }
  if (newVersion == radioConfigVersion) {
    free(newStations);
    return true;
  }

  const String previousStationId = stations[selectedStation.load()].id;
  saveCachedRadioConfig(newVersion, newStations, newCount);
  radioConfigVersion = newVersion;
  stationCount = newCount;
  memcpy(stations, newStations, sizeof(Station) * newCount);
  free(newStations);
  uint8_t newSelection = 0;
  for (uint8_t index = 0; index < stationCount; ++index) {
    if (previousStationId == stations[index].id) {
      newSelection = index;
      break;
    }
  }
  selectedStation = newSelection;
  Serial.printf("[CONFIG] applied version=%u stations=%u wired-output=1\n",
                radioConfigVersion, stationCount);
  return true;
}

bool readWavHeader(WiFiClient &client, WavFormat &format) {
  uint8_t riff[12];
  if (!readExact(client, riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0)
    return false;

  bool foundFormat = false;
  while (true) {
    uint8_t chunkHeader[8];
    if (!readExact(client, chunkHeader, sizeof(chunkHeader))) return false;
    const uint32_t chunkSize = readLe32(chunkHeader + 4);

    if (memcmp(chunkHeader, "data", 4) == 0) return foundFormat;
    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16 || chunkSize > 128) return false;
      uint8_t body[128];
      if (!readExact(client, body, chunkSize)) return false;
      format.formatTag = readLe16(body);
      format.channels = readLe16(body + 2);
      format.sampleRate = readLe32(body + 4);
      format.blockAlign = readLe16(body + 12);
      format.bitsPerSample = readLe16(body + 14);
      if (chunkSize >= 20) format.samplesPerBlock = readLe16(body + 18);
      foundFormat = true;
    } else if (!skipExact(client, chunkSize)) {
      return false;
    }
    if ((chunkSize & 1) != 0 && !skipExact(client, 1)) return false;
  }
}

bool validateWavFormat(const WavFormat &format) {
  const uint16_t expectedSamples = 1 + 2 * (format.blockAlign - 4);
  return format.formatTag == 0x11 && format.channels == 1 &&
         format.sampleRate == kInputSampleRate && format.bitsPerSample == 4 &&
         format.blockAlign >= 4 && format.blockAlign <= kMaxAdpcmBlock &&
         format.samplesPerBlock == expectedSamples;
}

bool pushEncodedBlock(const EncodedBlock &block, uint32_t generation) {
  if (block.size < 4 || block.size > kMaxAdpcmBlock ||
      block.generation != generation)
    return false;
  bool recordedWait = false;
  while (WiFi.isConnected() && !radioPaused.load() &&
         streamGeneration.load() == generation) {
    const uint32_t write =
        encodedWriteCount.load(std::memory_order_relaxed);
    const uint32_t read =
        encodedReadCount.load(std::memory_order_acquire);
    if (write - read < kEncodedRingBlocks) {
      EncodedBlock &destination = encodedRing[write % kEncodedRingBlocks];
      destination.generation = block.generation;
      destination.size = block.size;
      memcpy(destination.data, block.data, block.size);
      if (radioPaused.load() || streamGeneration.load() != generation)
        return false;
      encodedWriteCount.store(write + 1, std::memory_order_release);
      return true;
    }
    if (!recordedWait) {
      ++backpressureWaits;
      recordedWait = true;
    }
    // Never discard a complete 139 ms block. TCP backpressure leaves any
    // surplus queued at the VPS until the DAC consumes another local block.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return false;
}

void networkTask(void *) {
  while (true) {
    if (!WiFi.isConnected() || radioPaused.load() || wifiPortalActive) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    const uint32_t activeGeneration = streamGeneration.load();
    const uint8_t stationIndex = selectedStation.load();
    char stationId[sizeof(stations[0].id)];
    strlcpy(stationId, stations[stationIndex].id, sizeof(stationId));
    WiFiClient client;
    client.setNoDelay(true);
    Serial.printf("[NET] connecting station=%s host=%s\n", stationId,
                  kGatewayHttpHost);
    if (!client.connect(kGatewayHost, kGatewayPort)) {
      ++networkReconnects;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    int receiveBufferBytes = 8192;
    client.setSocketOption(SO_RCVBUF, reinterpret_cast<char *>(&receiveBufferBytes),
                           sizeof(receiveBufferBytes));

    client.printf("GET /esp-radio/gateway.php?station=%s&client=wired HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                  stationId, kGatewayHttpHost);
    uint32_t announcedConfigVersion = 0;
    if (!readHttpHeaders(client, &announcedConfigVersion)) {
      client.stop();
      ++networkReconnects;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (announcedConfigVersion != 0 &&
        announcedConfigVersion != radioConfigVersion) {
      Serial.printf("[CONFIG] server version=%u cached=%u; updating\n",
                    announcedConfigVersion, radioConfigVersion);
      // Keep this audio socket open while the small catalogue request runs.
      // Saving or adding a station must not drain the local audio queue or
      // restart the ESP. The current station continues until the listener
      // changes it; the new catalogue is used for the next selection.
      fetchRemoteRadioConfig();
    }

    WavFormat format;
    if (!readWavHeader(client, format) ||
        !validateWavFormat(format)) {
      Serial.printf("[NET] invalid stream fmt=%u ch=%u rate=%u block=%u bits=%u samples=%u\n",
                    format.formatTag, format.channels, format.sampleRate,
                    format.blockAlign, format.bitsPerSample,
                    format.samplesPerBlock);
      client.stop();
      ++networkReconnects;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    Serial.printf("[NET] stream ready channels=%u block=%u samples=%u\n",
                  format.channels, format.blockAlign, format.samplesPerBlock);
    while ((client.connected() || client.available() > 0) &&
           streamGeneration.load() == activeGeneration &&
           !radioPaused.load()) {
      EncodedBlock block;
      block.generation = activeGeneration;
      block.size = format.blockAlign;
      if (!readExact(client, block.data, block.size)) break;
      if (!pushEncodedBlock(block, activeGeneration)) break;
      networkBytes.fetch_add(block.size);
      if (!WiFi.isConnected() || radioPaused.load() ||
          streamGeneration.load() != activeGeneration)
        break;
    }
    client.stop();
    if (streamGeneration.load() == activeGeneration && !radioPaused.load()) {
      ++networkReconnects;
      Serial.println("[NET] stream ended; reconnecting");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void beginNextWifiAttempt() {
  wifiAttempted = true;
  if (wifiNetworkCount == 0 || wifiPortalActive) return;
  if (wifiAttemptIndex >= wifiNetworkCount) wifiAttemptIndex = 0;
  WiFi.mode(WIFI_STA);
  // Bluetooth is completely disabled in the wired build, so modem sleep can
  // be disabled to minimize Wi-Fi receive latency without any coexistence cost.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  const WifiCredential &network = wifiNetworks[wifiAttemptIndex];
  WiFi.begin(network.ssid.c_str(), network.password.c_str());
  wifiAttemptActive = true;
  wifiAttemptStartedAt = millis();
  lastWifiRetryAt = millis();
  Serial.printf("[WIFI] trying %u/%u %s\n", wifiAttemptIndex + 1,
                wifiNetworkCount, network.ssid.c_str());
  wifiAttemptIndex = (wifiAttemptIndex + 1) % wifiNetworkCount;
}

String htmlEscape(const String &value) {
  String result;
  result.reserve(value.length() + 16);
  for (size_t index = 0; index < value.length(); ++index) {
    switch (value[index]) {
      case '&': result += F("&amp;"); break;
      case '<': result += F("&lt;"); break;
      case '>': result += F("&gt;"); break;
      case '"': result += F("&quot;"); break;
      case '\'': result += F("&#39;"); break;
      default: result += value[index];
    }
  }
  return result;
}

String wifiSetupPage(const String &notice = "") {
  String options;
  const int found = WiFi.scanNetworks(false, true);
  for (int index = 0; index < found; ++index) {
    const String ssid = WiFi.SSID(index);
    if (!ssid.isEmpty() && options.indexOf("value=\"" + htmlEscape(ssid) + "\"") < 0)
      options += "<option value=\"" + htmlEscape(ssid) + "\">";
  }
  String rows;
  for (uint8_t index = 0; index < kMaxWifiNetworks; ++index) {
    const String ssid = index < wifiNetworkCount ? wifiNetworks[index].ssid : "";
    rows += "<section><strong>الشبكة " + String(index + 1) +
            "</strong><input name=\"ssid" + String(index) +
            "\" list=\"scanned\" value=\"" + htmlEscape(ssid) +
            "\" placeholder=\"اختر أو اكتب اسم الشبكة\"><input name=\"pass" +
            String(index) +
            "\" type=\"password\" placeholder=\"كلمة المرور — اتركها فارغة للاحتفاظ بالحالية\"></section>";
  }
  return "<!doctype html><html lang=\"ar\" dir=\"rtl\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>إعداد Wi-Fi للراديو</title><style>body{margin:0;background:#07101c;color:#eef6ff;font:16px Tahoma,sans-serif}main{width:min(680px,calc(100% - 24px));margin:24px auto}h1{color:#38bdf8}section{padding:14px;margin:10px 0;background:#101c2c;border:1px solid #2a3c52;border-radius:14px}input{display:block;width:100%;box-sizing:border-box;margin-top:9px;padding:12px;border:1px solid #38506b;border-radius:10px;background:#080f19;color:white}button{width:100%;padding:13px;border:0;border-radius:11px;background:#38bdf8;color:#03121c;font-weight:bold;font-size:16px}.note{padding:12px;border-radius:10px;background:#123528;color:#bbf7d0}</style></head><body><main><h1>إعداد شبكات راديو ESP</h1><p>احفظ حتى خمس شبكات. سيجربها الجهاز بالترتيب عند انقطاع الشبكة الأساسية.</p>" +
         (notice.isEmpty() ? "" : "<p class=\"note\">" + htmlEscape(notice) + "</p>") +
         "<form method=\"post\" action=\"/save\"><datalist id=\"scanned\">" + options +
         "</datalist>" + rows + "<button>حفظ وإعادة تشغيل الراديو</button></form></main></body></html>";
}

void startWifiPortal() {
  if (wifiPortalActive) return;
  wifiPortalActive = true;
  radioPaused = true;
  restartStreamBuffer();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kSetupApName, kSetupApPassword);
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupServer.on("/", HTTP_GET, []() {
    setupServer.send(200, "text/html; charset=utf-8", wifiSetupPage());
  });
  setupServer.on("/save", HTTP_POST, []() {
    WifiCredential updated[kMaxWifiNetworks];
    uint8_t count = 0;
    for (uint8_t index = 0; index < kMaxWifiNetworks; ++index) {
      String ssid = setupServer.arg("ssid" + String(index));
      String password = setupServer.arg("pass" + String(index));
      ssid.trim();
      if (ssid.isEmpty()) continue;
      if (password.isEmpty()) {
        for (uint8_t old = 0; old < wifiNetworkCount; ++old) {
          if (wifiNetworks[old].ssid == ssid) {
            password = wifiNetworks[old].password;
            break;
          }
        }
      }
      updated[count].ssid = ssid;
      updated[count].password = password;
      ++count;
    }
    if (count == 0) {
      setupServer.send(400, "text/html; charset=utf-8",
                       wifiSetupPage("أضف شبكة واحدة على الأقل."));
      return;
    }
    wifiNetworkCount = count;
    for (uint8_t index = 0; index < count; ++index) wifiNetworks[index] = updated[index];
    saveWifiNetworks();
    setupServer.send(200, "text/html; charset=utf-8",
                     wifiSetupPage("تم الحفظ. سيعاد تشغيل الجهاز خلال ثانيتين."));
    restartAt = millis() + 2000;
    restartScheduled = true;
  });
  setupServer.onNotFound([]() {
    setupServer.sendHeader("Location", "http://192.168.4.1/", true);
    setupServer.send(302, "text/plain", "");
  });
  setupServer.begin();
  Serial.println("[SETUP] Wi-Fi portal: ESP-Radio-Setup / 192.168.4.1");
}

void restartStreamBuffer() {
  playbackPrimed = false;
  ++streamGeneration;
  encodedReadCount.store(encodedWriteCount.load(std::memory_order_acquire),
                         std::memory_order_release);
}

void saveSelectedStation() {
  Preferences settings;
  if (settings.begin("esp32-radio", false)) {
    settings.putString("station_id", stations[selectedStation.load()].id);
    settings.end();
  }
}

void selectAnotherStation(int change) {
  int station = static_cast<int>(selectedStation.load()) + change;
  if (station < 0) station = stationCount - 1;
  if (station >= stationCount) station = 0;
  selectedStation = static_cast<uint8_t>(station);
  radioPaused = false;
  saveSelectedStation();
  restartStreamBuffer();
  Serial.printf("[BUTTON] station=%u %s\n", selectedStation.load(),
                stations[selectedStation.load()].name);
}

void togglePause() {
  radioPaused = !radioPaused.load();
  restartStreamBuffer();
  Serial.printf("[BUTTON] radio=%s\n", radioPaused.load() ? "paused" : "playing");
}

void updateVolume(bool force = false) {
  static uint32_t lastReadAt = 0;
  if (!force && millis() - lastReadAt < 40) return;
  lastReadAt = millis();

  const int reading = analogRead(kVolumePin);
  if (filteredPotReading == 0.0f)
    filteredPotReading = reading;
  else
    filteredPotReading = filteredPotReading * 0.85f + reading * 0.15f;

  int percent = constrain(
      static_cast<int>(filteredPotReading * 100.0f / 4095.0f + 0.5f), 0,
      100);
  if (percent <= 2) percent = 0;
  if (percent >= 98) percent = 100;
  if (!force && abs(percent - lastVolumePercent) <= 2) return;
  lastVolumePercent = percent;
  volumePercent = percent;
  Serial.printf("[VOLUME] %d%%\n", percent);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Managed VPS wired AUX radio 2026-09-13.33 ===");
  Serial.printf("Gateway: http://%s:%u host=%s\n", kGatewayHost,
                kGatewayPort, kGatewayHttpHost);

  previousButton.begin();
  playPauseButton.begin();
  nextButton.begin();
  analogReadResolution(12);
  pinMode(kVolumePin, INPUT);

  loadCachedRadioConfig();
  loadWifiNetworks();
  Preferences settings;
  if (settings.begin("esp32-radio", true)) {
    const String savedStationId = settings.getString("station_id", "");
    uint8_t savedIndex = settings.getUChar("station", 0) % stationCount;
    for (uint8_t index = 0; index < stationCount; ++index) {
      if (savedStationId == stations[index].id) {
        savedIndex = index;
        break;
      }
    }
    selectedStation = savedIndex;
    settings.end();
  }
  filteredPotReading = analogRead(kVolumePin);
  updateVolume(true);
  Serial.printf("[RADIO] station=%u %s\n", selectedStation.load(),
                stations[selectedStation.load()].name);
  Serial.printf("[CONFIG] cached version=%u stations=%u wired-output=1 wifi=%u\n",
                radioConfigVersion, stationCount, wifiNetworkCount);
  Serial.println("[BUTTONS] previous/next=station; play=play/pause; hold previous+next=Wi-Fi setup");

  if (!startWiredAudio()) {
    Serial.println("[SYSTEM] wired audio initialization failed; restarting");
    delay(1000);
    ESP.restart();
  }
  if (xTaskCreatePinnedToCore(wiredAudioTask, "wired-audio", 4096, nullptr, 3,
                              nullptr, 0) != pdPASS) {
    Serial.println("[SYSTEM] wired audio task allocation failed; restarting");
    delay(1000);
    ESP.restart();
  }
  audioOutputTaskStarted = true;

  if (wifiNetworkCount == 0) {
    startWifiPortal();
  } else {
    beginNextWifiAttempt();
  }
}

void loop() {
  static uint32_t lastReportAt = 0;
  static uint32_t wifiConnectedAt = 0;
  static uint32_t wifiComboStartedAt = 0;
  updateVolume();
  uint32_t previousHeld = 0;
  uint32_t playHeld = 0;
  uint32_t nextHeld = 0;
  const bool previousReleased = previousButton.released(&previousHeld);
  const bool playReleased = playPauseButton.released(&playHeld);
  const bool nextReleased = nextButton.released(&nextHeld);

  if (!wifiPortalActive && previousButton.isPressed() && nextButton.isPressed()) {
    if (wifiComboStartedAt == 0) wifiComboStartedAt = millis();
    if (millis() - wifiComboStartedAt >= 4000) startWifiPortal();
  } else {
    wifiComboStartedAt = 0;
  }
  if (!wifiPortalActive) {
    if (previousReleased) selectAnotherStation(-1);
    if (nextReleased) selectAnotherStation(+1);
    if (playReleased) togglePause();
  } else {
    dnsServer.processNextRequest();
    setupServer.handleClient();
  }
  if (restartScheduled.load() &&
      static_cast<int32_t>(millis() - restartAt.load()) >= 0) {
    Serial.println("[SYSTEM] controlled restart");
    delay(50);
    ESP.restart();
  }

  if (WiFi.isConnected()) {
    wifiAttemptActive = false;
  } else if (!wifiPortalActive && wifiAttempted && wifiNetworkCount > 0) {
    if (wifiAttemptActive && millis() - wifiAttemptStartedAt >= 45000) {
      WiFi.disconnect(false, false);
      wifiAttemptActive = false;
      beginNextWifiAttempt();
    } else if (!wifiAttemptActive && millis() - lastWifiRetryAt >= 5000) {
      beginNextWifiAttempt();
    }
  }
  if (!WiFi.isConnected()) {
    wifiConnectedAt = 0;
  } else if (wifiConnectedAt == 0) {
    wifiConnectedAt = millis();
  }
  if (WiFi.isConnected() && wifiConnectedAt != 0 &&
      millis() - wifiConnectedAt >= 1500 && !networkTaskStarted) {
    networkTaskStarted = true;
    // Keep Wi-Fi/TCP/ADPCM work on the Arduino application core while the
    // small blocking DAC writer owns core 0.
    if (xTaskCreatePinnedToCore(networkTask, "radio-network", 6144, nullptr, 1,
                                nullptr, 1) != pdPASS) {
      networkTaskStarted = false;
      Serial.println("[NET] task allocation failed");
    }
  }
  if (millis() - lastReportAt >= 5000) {
    lastReportAt = millis();
    const uint32_t queuedBlocks =
        encodedWriteCount.load(std::memory_order_acquire) -
        encodedReadCount.load(std::memory_order_relaxed);
    const uint32_t bufferedMs =
        queuedBlocks * (kMaxDecodedSamples * 1000UL / kInputSampleRate);
    Serial.printf(
        "[STATUS] station=%u paused=%d volume=%u wired=%d wifi=%d rssi=%d primed=%d buffer_blocks=%u buffer_ms=%u "
        "trim=%d cfg=%u stations=%u writes=%u frames=%llu blocks=%u dropped=%u waits=%u bytes=%llu underruns=%u reconnects=%u heap=%u\n",
        selectedStation.load(), radioPaused.load(), volumePercent.load(),
        wiredAudioReady.load() && audioOutputTaskStarted, WiFi.status(),
        WiFi.isConnected() ? WiFi.RSSI() : 0, playbackPrimed.load(),
        static_cast<unsigned>(queuedBlocks),
        static_cast<unsigned>(bufferedMs),
        static_cast<int>(clockTrimMode.load()), radioConfigVersion, stationCount,
        audioWrites.load(), outputFrames.load(), decodedBlocks.load(),
        droppedInputBlocks.load(), backpressureWaits.load(), networkBytes.load(), underruns.load(), networkReconnects.load(),
        ESP.getFreeHeap());
  }
  delay(20);
}
