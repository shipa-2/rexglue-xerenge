/**
 * @file        rex/audio/wmv3_decoder.h
 * @brief       WMV3 (VC-1 Main profile) video frames decoded on the host
 *
 * A title's movies are often WMV3 in ASF (the XMV player). Decoding them with
 * FFmpeg on the host costs a fraction of running the title's own recompiled
 * decoder; a title hooks its decoder's entry points and hands the compressed
 * frames here. FFmpeg stays behind this interface.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rex::audio {

class Wmv3Decoder {
 public:
  Wmv3Decoder();
  ~Wmv3Decoder();
  Wmv3Decoder(const Wmv3Decoder&) = delete;
  Wmv3Decoder& operator=(const Wmv3Decoder&) = delete;

  // The sequence header (STRUCT_C, 4 bytes for WMV3) and the picture size.
  bool Open(const uint8_t* sequence_header, size_t header_size, int width, int height);
  bool is_open() const;

  // One whole compressed frame. True when it produced a picture.
  bool Decode(const uint8_t* data, size_t size);

  // The last picture, as planar 4:2:0: luma rows of `width` bytes, chroma
  // rows of `width / 2`. False when there is none yet.
  bool CopyPlanes(uint8_t* y, uint8_t* u, uint8_t* v, int width, int height) const;

  // Drops reference pictures (a seek or a discontinuity).
  void Flush();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::audio
