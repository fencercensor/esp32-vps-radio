#include "../firmware/lossless_transport.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  require(argc == 3, "Expected wire and original fixture paths");
  std::ifstream input(argv[1], std::ios::binary);
  std::ifstream original(argv[2], std::ios::binary);
  require(input.good() && original.good(), "Fixture open failed");
  std::vector<uint8_t> wire((std::istreambuf_iterator<char>(input)), {});
  std::vector<uint8_t> expected((std::istreambuf_iterator<char>(original)), {});
  std::vector<uint8_t> restored;
  uint8_t audio[RadioLossless::kMaxBytes];
  size_t cursor = 0;
  while (cursor < wire.size()) {
    require(wire.size() - cursor >= RadioLossless::kHeaderBytes, "Truncated header");
    RadioLossless::Frame frame;
    require(RadioLossless::parseHeader(wire.data() + cursor, frame), "Invalid frame");
    cursor += RadioLossless::kHeaderBytes;
    require(wire.size() - cursor >= frame.wireBytes, "Truncated payload");
    require(RadioLossless::decode(frame, wire.data() + cursor, audio), "Decode failed");
    restored.insert(restored.end(), audio, audio + frame.rawBytes);
    if (frame.encoding == 1) {
      std::vector<uint8_t> corrupt(wire.begin() + cursor, wire.begin() + cursor + frame.wireBytes);
      corrupt.back() ^= 1;
      require(!RadioLossless::decode(frame, corrupt.data(), audio), "Bad checksum accepted");
      auto truncated = frame;
      --truncated.wireBytes;
      require(!RadioLossless::decode(truncated, wire.data() + cursor, audio), "Incomplete zlib accepted");
    }
    cursor += frame.wireBytes;
  }
  require(restored == expected, "Audio bytes changed");
  uint8_t header[8] = {'R', 'Z', 1, 9, 0, 36, 1, 0};
  RadioLossless::Frame frame;
  require(!RadioLossless::parseHeader(header, frame), "Oversized group accepted");
  header[3] = 1; header[5] = 4; header[6] = 0;
  require(!RadioLossless::parseHeader(header, frame), "Zero payload accepted");
  header[6] = 1; header[2] = 7;
  require(!RadioLossless::parseHeader(header, frame), "Unknown encoding accepted");
  header[2] = 0;
  require(!RadioLossless::parseHeader(header, frame), "Wrong raw length accepted");
  header[2] = 1; header[0] = 'X';
  require(!RadioLossless::parseHeader(header, frame), "Wrong magic accepted");
  std::cout << "Exact round trip and malformed-frame rejection passed\n";
}
