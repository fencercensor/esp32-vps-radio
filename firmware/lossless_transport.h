#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef ARDUINO_ARCH_ESP32
#include <miniz.h>
#else
#include <zlib.h>
#endif

namespace RadioLossless {
constexpr size_t kBlockBytes = 1024;
constexpr size_t kMaxBlocks = 8;
constexpr size_t kMaxBytes = kBlockBytes * kMaxBlocks;
constexpr size_t kHeaderBytes = 8;
struct Frame {
  uint8_t encoding;
  uint8_t blocks;
  uint16_t rawBytes;
  uint16_t wireBytes;
};
inline bool parseHeader(const uint8_t *header, Frame &frame) {
  if (header == nullptr || header[0] != 'R' || header[1] != 'Z') return false;
  frame.encoding = header[2];
  frame.blocks = header[3];
  frame.rawBytes = header[4] | (uint16_t(header[5]) << 8);
  frame.wireBytes = header[6] | (uint16_t(header[7]) << 8);
  return frame.encoding <= 1 && frame.blocks >= 1 && frame.blocks <= kMaxBlocks &&
      frame.rawBytes == frame.blocks * kBlockBytes && frame.wireBytes > 0 &&
      frame.wireBytes <= frame.rawBytes &&
      (frame.encoding != 0 || frame.wireBytes == frame.rawBytes);
}
inline bool decode(const Frame &frame, const uint8_t *wire, uint8_t *audio) {
  if (wire == nullptr || audio == nullptr || frame.rawBytes > kMaxBytes ||
      frame.rawBytes != frame.blocks * kBlockBytes || frame.blocks == 0 ||
      frame.wireBytes == 0 || frame.wireBytes > frame.rawBytes) return false;
  if (frame.encoding == 0) {
    if (frame.wireBytes != frame.rawBytes) return false;
    memcpy(audio, wire, frame.rawBytes);
    return true;
  }
  if (frame.encoding != 1) return false;
#ifdef ARDUINO_ARCH_ESP32
  // The ROM inflater has a large state: retain it outside the network task's
  // stack and bound the output to this frame, avoiding a 32 KB dictionary.
  static tinfl_decompressor *inflater = nullptr;
  if (inflater == nullptr)
    inflater = static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor)));
  if (inflater == nullptr) return false;
  tinfl_init(inflater);
  size_t inputBytes = frame.wireBytes;
  size_t outputBytes = frame.rawBytes;
  const tinfl_status status = tinfl_decompress(inflater, wire, &inputBytes,
      audio, audio, &outputBytes,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE && inputBytes == frame.wireBytes &&
      outputBytes == frame.rawBytes;
#else
  uLongf outputBytes = frame.rawBytes;
  return uncompress(audio, &outputBytes, wire, frame.wireBytes) == Z_OK &&
      outputBytes == frame.rawBytes;
#endif
}
}  // namespace RadioLossless
