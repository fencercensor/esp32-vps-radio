#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ImaAdpcm {

struct State {
  int predictor;
  int stepIndex;
};

inline int16_t decodeNibble(uint8_t nibble, State &state) {
  static const int kIndexTable[16] = {
      -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
  static const int kStepTable[89] = {
      7,     8,     9,     10,    11,    12,    13,    14,    16,
      17,    19,    21,    23,    25,    28,    31,    34,    37,
      41,    45,    50,    55,    60,    66,    73,    80,    88,
      97,    107,   118,   130,   143,   157,   173,   190,   209,
      230,   253,   279,   307,   337,   371,   408,   449,   494,
      544,   598,   658,   724,   796,   876,   963,   1060,  1166,
      1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,
      3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
      7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289,
      16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

  const int step = kStepTable[state.stepIndex];
  // WAV IMA ADPCM uses one combined multiply before the right shift.  Keeping
  // the rounding here identical to FFmpeg matters at the smallest step sizes.
  const int difference = ((2 * (nibble & 7) + 1) * step) >> 3;
  state.predictor += (nibble & 8) ? -difference : difference;
  if (state.predictor > 32767) state.predictor = 32767;
  if (state.predictor < -32768) state.predictor = -32768;

  state.stepIndex += kIndexTable[nibble & 0x0f];
  if (state.stepIndex < 0) state.stepIndex = 0;
  if (state.stepIndex > 88) state.stepIndex = 88;
  return static_cast<int16_t>(state.predictor);
}

inline size_t decodeMonoBlock(const uint8_t *block, size_t blockSize,
                              int16_t *output, size_t outputCapacity) {
  if (block == nullptr || output == nullptr || blockSize < 4 ||
      outputCapacity == 0) {
    return 0;
  }

  State state{
      static_cast<int16_t>(static_cast<uint16_t>(block[0]) |
                           (static_cast<uint16_t>(block[1]) << 8)),
      block[2],
  };
  if (state.stepIndex > 88) return 0;

  size_t written = 0;
  output[written++] = static_cast<int16_t>(state.predictor);
  for (size_t index = 4; index < blockSize && written < outputCapacity;
       ++index) {
    output[written++] = decodeNibble(block[index] & 0x0f, state);
    if (written < outputCapacity) {
      output[written++] = decodeNibble(block[index] >> 4, state);
    }
  }
  return written;
}

}  // namespace ImaAdpcm
