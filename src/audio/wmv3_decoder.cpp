/**
 * @file        audio/wmv3_decoder.cpp
 * @brief       WMV3 frames decoded with FFmpeg's VC-1 decoder
 *
 * @license     BSD 3-Clause License
 */

#include <rex/audio/wmv3_decoder.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/logging.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
}

namespace rex::audio {

struct Wmv3Decoder::State {
  AVCodecContext* context = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  AVFrame* scratch = nullptr;
  bool have_picture = false;
  std::vector<uint8_t> input;

  ~State() {
    av_frame_free(&scratch);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
  }
};

Wmv3Decoder::Wmv3Decoder() = default;
Wmv3Decoder::~Wmv3Decoder() = default;

bool Wmv3Decoder::is_open() const {
  return state_ && state_->context;
}

bool Wmv3Decoder::Open(const uint8_t* sequence_header, size_t header_size, int width,
                       int height) {
  state_.reset();
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!codec) {
    REXLOG_ERROR("wmv3: FFmpeg was built without its WMV3 decoder");
    return false;
  }
  auto state = std::make_unique<State>();
  state->context = avcodec_alloc_context3(codec);
  state->packet = av_packet_alloc();
  state->frame = av_frame_alloc();
  state->scratch = av_frame_alloc();
  if (!state->context || !state->packet || !state->frame || !state->scratch) {
    return false;
  }
  state->context->width = width;
  state->context->height = height;
  state->context->extradata =
      static_cast<uint8_t*>(av_mallocz(header_size + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!state->context->extradata) {
    return false;
  }
  std::memcpy(state->context->extradata, sequence_header, header_size);
  state->context->extradata_size = int(header_size);
  if (int err = avcodec_open2(state->context, codec, nullptr); err < 0) {
    REXLOG_ERROR("wmv3: avcodec_open2 failed ({})", err);
    return false;
  }
  state_ = std::move(state);
  return true;
}

bool Wmv3Decoder::Decode(const uint8_t* data, size_t size) {
  if (!is_open()) {
    return false;
  }
  State& s = *state_;
  // FFmpeg reads past the end of a packet by up to the padding size.
  s.input.assign(data, data + size);
  s.input.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  s.packet->data = s.input.data();
  s.packet->size = int(size);
  if (avcodec_send_packet(s.context, s.packet) < 0) {
    return false;
  }
  // A receive that finds nothing still clears the frame it is given, so each
  // picture goes through a frame of its own and is kept only once complete.
  bool got = false;
  while (avcodec_receive_frame(s.context, s.scratch) == 0) {
    av_frame_unref(s.frame);
    av_frame_move_ref(s.frame, s.scratch);
    got = true;
  }
  if (got) {
    s.have_picture = true;
  }
  return got;
}

bool Wmv3Decoder::CopyPlanes(uint8_t* y, uint8_t* u, uint8_t* v, int width, int height) const {
  if (!is_open() || !state_->have_picture) {
    return false;
  }
  const AVFrame* f = state_->frame;
  const int w = std::min(width, f->width);
  const int h = std::min(height, f->height);
  for (int row = 0; row < h; ++row) {
    std::memcpy(y + size_t(row) * width, f->data[0] + size_t(row) * f->linesize[0], w);
  }
  for (int row = 0; row < h / 2; ++row) {
    std::memcpy(u + size_t(row) * (width / 2), f->data[1] + size_t(row) * f->linesize[1], w / 2);
    std::memcpy(v + size_t(row) * (width / 2), f->data[2] + size_t(row) * f->linesize[2], w / 2);
  }
  return true;
}

void Wmv3Decoder::Flush() {
  if (is_open()) {
    avcodec_flush_buffers(state_->context);
  }
}

}  // namespace rex::audio
