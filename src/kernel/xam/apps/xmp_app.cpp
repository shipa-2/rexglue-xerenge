/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - XMP playback: decode and play the title's own playlist
 *              rather than acknowledging the calls and staying silent. After
 *              xenia-canary@764f230dd9883bfb63e3aea7642aac26f010e1bb, which
 *              did the same for Xenia; adapted here because this SDK's
 *              AudioDriver submits into guest memory at a fixed 6-channel
 *              48 kHz format rather than taking an arbitrary host pointer at
 *              whatever rate the driver was created with, and because this
 *              SDK's vendored FFmpeg previously built only libavcodec and
 *              libavutil (curated for WMA Pro), so libavformat and
 *              libswresample were added alongside this (see
 *              thirdparty/CMakeLists.txt) rather than hand-parsing ASF or
 *              resampling by hand.
 */

#include <algorithm>
#include <cstring>
#include <span>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(xmp_enable, true, "Audio",
                    "Decode and play the title's own XMP (background music) playlist");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

namespace {

// The SDL driver's fixed output shape: 6 channels (XAudio order fl fr fc lf bl
// br), 48 kHz, 256 sample-frames per SubmitFrame call. A song's own rate and
// channel count are resampled to stereo at this rate by swresample, then
// placed into the front-left/front-right slots of that layout by hand - no
// generic upmix matrix is involved, so a stereo song stays exactly a stereo
// song, just centred in front rather than spread across a 5.1 field it was
// never authored for.
constexpr uint32_t kOutputRate = 48000;
constexpr uint32_t kOutputChannelsInFrame = 6;
constexpr uint32_t kFrameChannelSamples = 256;
constexpr uint32_t kFrameSamples = kOutputChannelsInFrame * kFrameChannelSamples;
constexpr uint32_t kFrameBytes = kFrameSamples * sizeof(float);

struct VfsReadContext {
  rex::filesystem::File* file;
  size_t byte_offset;
};

int VfsRead(void* opaque, uint8_t* buf, int buf_size) {
  auto* ctx = static_cast<VfsReadContext*>(opaque);
  size_t bytes_read = 0;
  X_STATUS status = ctx->file->ReadSync(std::span<uint8_t>(buf, size_t(buf_size)),
                                        ctx->byte_offset, &bytes_read);
  if (XFAILED(status)) {
    return status == X_STATUS_END_OF_FILE ? AVERROR_EOF : AVERROR_UNKNOWN;
  }
  if (bytes_read == 0) {
    return AVERROR_EOF;
  }
  ctx->byte_offset += bytes_read;
  return int(bytes_read);
}

// Small RAII wrappers so the many early-exit points below (a bad file, an
// unsupported codec, a track change mid-decode) cannot leak an FFmpeg object.
struct FormatContextCloser {
  void operator()(AVFormatContext* c) const {
    if (c) avformat_close_input(&c);
  }
};
struct CodecContextCloser {
  void operator()(AVCodecContext* c) const {
    if (c) avcodec_free_context(&c);
  }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const {
    if (p) av_packet_free(&p);
  }
};
struct FrameDeleter {
  void operator()(AVFrame* f) const {
    if (f) av_frame_free(&f);
  }
};
struct SwrContextCloser {
  void operator()(SwrContext* s) const {
    if (s) swr_free(&s);
  }
};

}  // namespace

XmpApp::XmpApp(KernelState* kernel_state)
    : App(kernel_state, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1) {
  // The worker thread is started lazily, from XMPPlayTitlePlaylist, rather
  // than here. An XmpApp is constructed wherever the XAM app table is built,
  // which is not only the running emulator - the codegen tool links the same
  // kernel code and instantiates it too - and a thread that spends its whole
  // life blocked on a fence nothing will ever signal there is worse than
  // wasted: nothing in that context drives it towards exit, so the process
  // hangs waiting for a thread that was never going to finish.
}

void XmpApp::EnsureWorkerStarted() {
  if (worker_thread_ || !REXCVAR_GET(xmp_enable)) {
    return;
  }
  worker_running_ = true;
  worker_thread_ = rex::thread::Thread::Create({}, [this] { WorkerThreadMain(); });
  worker_thread_->set_name("XMP Music Player");
}

bool XmpApp::PlayFile(const std::string& utf8_path, Playlist* playlist, int song_index) {
  auto is_superseded = [&] {
    return active_playlist_ != playlist || active_song_index_ != song_index ||
           state_ == State::kIdle;
  };

  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action;
  X_STATUS open_status = kernel_state_->file_system()->OpenFile(
      nullptr, utf8_path, rex::filesystem::FileDisposition::kOpen,
      rex::filesystem::FileAccess::kGenericRead, false, true, &vfs_file, &action);
  if (XFAILED(open_status)) {
    REXKRNL_ERROR("XMP: opening {} failed with status {:08X}", utf8_path, open_status);
    return false;
  }

  constexpr int kIoBufferSize = 8192;
  uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
  VfsReadContext read_ctx{vfs_file, 0};
  AVIOContext* avio_ctx =
      avio_alloc_context(io_buffer, kIoBufferSize, 0, &read_ctx, &VfsRead, nullptr, nullptr);
  if (!avio_ctx) {
    av_free(io_buffer);
    vfs_file->Destroy();
    REXKRNL_ERROR("XMP: avio_alloc_context failed for {}", utf8_path);
    return false;
  }

  std::unique_ptr<AVFormatContext, FormatContextCloser> format_ctx(avformat_alloc_context());
  format_ctx->pb = avio_ctx;
  {
    AVFormatContext* raw = format_ctx.release();
    int ret = avformat_open_input(&raw, nullptr, nullptr, nullptr);
    format_ctx.reset(raw);
    if (ret != 0) {
      REXKRNL_ERROR("XMP: avformat_open_input failed for {}: {}", utf8_path, ret);
      av_freep(&avio_ctx->buffer);
      avio_context_free(&avio_ctx);
      vfs_file->Destroy();
      return false;
    }
  }
  // From here on the AVIOContext is owned by format_ctx's teardown, but its
  // buffer is not freed by avformat_close_input - only the context struct is.
  auto free_avio = [&] {
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);
  };

  if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
    REXKRNL_ERROR("XMP: no stream info in {}", utf8_path);
    format_ctx.reset();
    free_avio();
    vfs_file->Destroy();
    return false;
  }

  AVCodec* codec = nullptr;
  int stream_index =
      av_find_best_stream(format_ctx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream_index < 0 || !codec) {
    REXKRNL_ERROR("XMP: no audio stream in {}", utf8_path);
    format_ctx.reset();
    free_avio();
    vfs_file->Destroy();
    return false;
  }
  AVStream* audio_stream = format_ctx->streams[stream_index];

  std::unique_ptr<AVCodecContext, CodecContextCloser> codec_ctx(avcodec_alloc_context3(codec));
  avcodec_parameters_to_context(codec_ctx.get(), audio_stream->codecpar);
  if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
    REXKRNL_ERROR("XMP: could not open decoder for {}", utf8_path);
    format_ctx.reset();
    free_avio();
    vfs_file->Destroy();
    return false;
  }

  int64_t in_channel_layout = codec_ctx->channel_layout
                                  ? int64_t(codec_ctx->channel_layout)
                                  : av_get_default_channel_layout(codec_ctx->channels);
  std::unique_ptr<SwrContext, SwrContextCloser> swr(swr_alloc_set_opts(
      nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, int(kOutputRate), in_channel_layout,
      codec_ctx->sample_fmt, codec_ctx->sample_rate, 0, nullptr));
  if (!swr || swr_init(swr.get()) < 0) {
    REXKRNL_ERROR("XMP: could not set up resampling for {}", utf8_path);
    format_ctx.reset();
    free_avio();
    vfs_file->Destroy();
    return false;
  }

  auto semaphore = rex::thread::Semaphore::Create(64, 64);
  auto* audio_system =
      static_cast<rex::audio::AudioSystem*>(kernel_state_->emulator()->audio_system());
  rex::audio::AudioDriver* driver = nullptr;
  if (XFAILED(audio_system->CreateHostDriver(semaphore.get(), &driver)) || !driver) {
    REXKRNL_ERROR("XMP: could not create an audio driver for {}", utf8_path);
    format_ctx.reset();
    free_avio();
    vfs_file->Destroy();
    return false;
  }

  uint32_t frame_addr = memory_->SystemHeapAlloc(kFrameBytes);

  std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  std::vector<float> stereo;  // interleaved L,R at kOutputRate, resampled.
  uint8_t* resample_out[1] = {nullptr};

  bool decode_error = false;

  auto submit_chunk = [&]() -> bool {
    // Blocks for backpressure, and doubles as the pause gate: while paused,
    // nothing is queued, so SDL plays silence rather than racing ahead to
    // buffer the whole rest of the song.
    while (state_ != State::kPlaying && !is_superseded()) {
      resume_fence_.Wait();
    }
    if (is_superseded()) {
      return false;
    }
    rex::thread::Wait(semaphore.get(), true);
    if (is_superseded()) {
      return false;
    }
    float out[kFrameSamples] = {};
    for (uint32_t i = 0; i < kFrameChannelSamples; ++i) {
      out[i * kOutputChannelsInFrame + 0] = stereo[i * 2 + 0] * volume_;
      out[i * kOutputChannelsInFrame + 1] = stereo[i * 2 + 1] * volume_;
    }
    std::memcpy(memory_->TranslateVirtual<float*>(frame_addr), out, kFrameBytes);
    driver->SubmitFrame(frame_addr);
    stereo.erase(stereo.begin(), stereo.begin() + kFrameChannelSamples * 2);
    return true;
  };

  while (!is_superseded() && av_read_frame(format_ctx.get(), packet.get()) >= 0) {
    if (packet->stream_index == stream_index) {
      int send_ret = avcodec_send_packet(codec_ctx.get(), packet.get());
      if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
        REXKRNL_WARN("XMP: send_packet failed for {}: {}", utf8_path, send_ret);
      }
      while (true) {
        int recv_ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
        if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
          break;
        }
        if (recv_ret < 0) {
          REXKRNL_WARN("XMP: receive_frame failed for {}: {}", utf8_path, recv_ret);
          decode_error = true;
          break;
        }

        int64_t max_out_samples =
            av_rescale_rnd(swr_get_delay(swr.get(), codec_ctx->sample_rate) + frame->nb_samples,
                          kOutputRate, codec_ctx->sample_rate, AV_ROUND_UP);
        size_t prev_size = stereo.size();
        stereo.resize(prev_size + size_t(max_out_samples) * 2);
        resample_out[0] = reinterpret_cast<uint8_t*>(stereo.data() + prev_size);
        int converted =
            swr_convert(swr.get(), resample_out, int(max_out_samples),
                       const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        av_frame_unref(frame.get());
        if (converted < 0) {
          REXKRNL_WARN("XMP: resampling failed for {}: {}", utf8_path, converted);
          stereo.resize(prev_size);
          decode_error = true;
          break;
        }
        stereo.resize(prev_size + size_t(converted) * 2);

        while (stereo.size() >= kFrameChannelSamples * 2) {
          if (!submit_chunk()) {
            goto done_reading;
          }
        }
      }
    }
    av_packet_unref(packet.get());
    if (decode_error) {
      break;
    }
  }
done_reading:

  // Flush whatever swresample is still holding, and pad the final partial
  // chunk with silence rather than dropping the last fraction of a second.
  if (!is_superseded() && !decode_error) {
    int64_t max_out_samples =
        av_rescale_rnd(swr_get_delay(swr.get(), kOutputRate), kOutputRate, kOutputRate, AV_ROUND_UP) +
        1024;
    size_t prev_size = stereo.size();
    stereo.resize(prev_size + size_t(max_out_samples) * 2);
    resample_out[0] = reinterpret_cast<uint8_t*>(stereo.data() + prev_size);
    int converted = swr_convert(swr.get(), resample_out, int(max_out_samples), nullptr, 0);
    stereo.resize(prev_size + size_t(std::max(converted, 0)) * 2);
    while (stereo.size() >= kFrameChannelSamples * 2) {
      if (!submit_chunk()) {
        break;
      }
    }
    if (!stereo.empty() && !is_superseded()) {
      stereo.resize(kFrameChannelSamples * 2, 0.0f);
      submit_chunk();
    }
  }

  memory_->SystemHeapFree(frame_addr);
  audio_system->DestroyHostDriver(driver);
  format_ctx.reset();
  free_avio();
  vfs_file->Destroy();

  return !decode_error;
}

void XmpApp::WorkerThreadMain() {
  while (worker_running_) {
    if (state_ != State::kPlaying) {
      resume_fence_.Wait();
      continue;
    }
    Playlist* playlist = active_playlist_;
    if (!playlist || playlist->songs.empty()) {
      state_ = State::kIdle;
      continue;
    }
    int song_index = active_song_index_;
    auto utf8_path = rex::string::to_utf8(playlist->songs[song_index]->file_path);
    REXKRNL_INFO("XMP: playing [{}] {}", song_index, utf8_path);

    bool ok = PlayFile(utf8_path, playlist, song_index);
    if (!ok) {
      REXKRNL_ERROR("XMP: playback failed for {}", utf8_path);
      rex::thread::Sleep(std::chrono::seconds(1));
      continue;
    }

    // If nothing else touched playback while that song was decoding, it ended
    // on its own: move on to the next song and keep the playlist going. A
    // title that got this far wants continuous background music, not silence
    // once the list is exhausted.
    if (active_playlist_ == playlist && active_song_index_ == song_index &&
        state_ == State::kPlaying) {
      active_song_index_ = (song_index + 1) % int(playlist->songs.size());
    }
  }
}

X_HRESULT XmpApp::XMPGetStatus(uint32_t state_ptr) {
  if (!XThread::GetCurrentThread()->main_thread()) {
    // Some stupid games will hammer this on a thread - induce a delay
    // here to keep from starving real threads.
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }

  REXKRNL_TRACE("XMPGetStatus({:08X})", state_ptr);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                                   static_cast<uint32_t>(state_));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                         uint32_t playlist_name_ptr,
                                         const std::u16string& playlist_name, uint32_t flags,
                                         uint32_t out_song_handles, uint32_t out_playlist_handle) {
  REXKRNL_DEBUG(
      "XMPCreateTitlePlaylist({:08X}, {:08X}, {:08X}({}), {:08X}, {:08X}, "
      "{:08X})",
      songs_ptr, song_count, playlist_name_ptr, rex::string::to_utf8(playlist_name), flags,
      out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = playlist_name;
  playlist->flags = flags;
  if (songs_ptr) {
    for (uint32_t i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      uint8_t* song_base = memory_->TranslateVirtual(songs_ptr + (i * 36));
      song->file_path = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 0)));
      song->name = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 4)));
      song->artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 8)));
      song->album = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 12)));
      song->album_artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 16)));
      song->genre = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 20)));
      song->track_number = memory::load_and_swap<uint32_t>(song_base + 24);
      song->duration_ms = memory::load_and_swap<uint32_t>(song_base + 28);
      song->format = static_cast<Song::Format>(memory::load_and_swap<uint32_t>(song_base + 32));
      if (out_song_handles) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_song_handles + (i * 4)),
                                         song->handle);
      }
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_playlist_handle),
                                     playlist->handle);
  }

  auto global_lock = global_critical_region_.Acquire();
  playlists_.insert({playlist->handle, playlist.get()});
  playlist.release();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  REXKRNL_DEBUG("XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  auto global_lock = global_critical_region_.Acquire();
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
    return X_E_NOTFOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  delete playlist;
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle) {
  REXKRNL_DEBUG("XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
      return X_E_NOTFOUND;
    }
    playlist = it->second;
  }

  // Burnout Revenge (and evidently other titles) sets PlaybackClient::kSystem
  // and then still expects its own playlist to play - on real hardware that
  // flag arbitrates with the dashboard's own "system" music, which this
  // runtime has none of, so returning early here left every title with a
  // licensed soundtrack silent regardless of what it asked to play.
  //
  // This call is not necessarily a one-shot "start the music" - Burnout
  // Revenge calls it many times a second, apparently to assert "this playlist
  // should be playing" rather than to mean "start over". The stub this
  // replaced tolerated that fine, since it did nothing; actually decoding
  // means restarting from song 0 on every one of those calls would chop the
  // first fraction of a second off song after song, forever - which sounds
  // exactly like noise, not music. Only (re)start when this would actually
  // change something.
  if (playlist == active_playlist_ && state_ == State::kPlaying) {
    OnStateChanged();
    kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
    return X_E_SUCCESS;
  }
  EnsureWorkerStarted();
  active_playlist_ = playlist;
  active_song_index_ = 0;
  state_ = State::kPlaying;
  resume_fence_.Signal();
  OnStateChanged();
  kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPContinue() {
  REXKRNL_DEBUG("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
    resume_fence_.Signal();
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(uint32_t unk) {
  assert_zero(unk);
  REXKRNL_DEBUG("XMPStop({:08X})", unk);
  active_playlist_ = nullptr;  // ?
  active_song_index_ = 0;
  state_ = State::kIdle;
  resume_fence_.Signal();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  REXKRNL_DEBUG("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  REXKRNL_DEBUG("XMPNext()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ = (active_song_index_ + 1) % active_playlist_->songs.size();
  resume_fence_.Signal();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  REXKRNL_DEBUG("XMPPrevious()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  if (!active_song_index_) {
    active_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
  } else {
    --active_song_index_;
  }
  resume_fence_.Signal();
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged, static_cast<uint32_t>(state_));
}

X_HRESULT XmpApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t storage_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t song_handle = memory::load_and_swap<uint32_t>(buffer + 8);  // 0?
      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(storage_ptr));
      assert_true(xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t unk = memory::load_and_swap<uint32_t>(buffer + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode;
        rex::be<uint32_t> repeat_mode;
        rex::be<uint32_t> flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})", uint32_t(args->playback_mode),
                    uint32_t(args->repeat_mode), uint32_t(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(uint32_t(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(uint32_t(args->repeat_mode));
      unknown_flags_ = args->flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t state_ptr = memory::load_and_swap<uint32_t>(buffer + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetVolume({:08X})", uint32_t(args->volume_ptr));
      memory::store_and_swap<float>(memory_->TranslateVirtual(args->volume_ptr), volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<float> value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPSetVolume({:g})", float(args->value));
      volume_ = args->value;
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
        rex::be<uint32_t> storage_size;
        rex::be<uint32_t> songs_ptr;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> playlist_name_ptr;
        rex::be<uint32_t> flags;
        rex::be<uint32_t> song_handles_ptr;
        rex::be<uint32_t> playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 36);

      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playlist_handle_ptr),
                                       args->storage_ptr);
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = memory::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // dummy_alloc_ptr is the result of a XamAlloc of storage_size.
      assert_true(uint32_t(args->storage_size) == 4 + uint32_t(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->songs_ptr, args->song_count, args->playlist_name_ptr,
                                    playlist_name, args->flags, args->song_handles_ptr,
                                    args->storage_ptr);
    }
    case 0x0007000E: {
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> unk_ptr;  // 0
        rex::be<uint32_t> info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      auto info = memory_->TranslateVirtual(args->info_ptr);
      assert_true(args->xmp_client == 0x00000002);
      assert_zero(args->unk_ptr);
      REXKRNL_ERROR("XMPGetInfo?({:08X}, {:08X})", uint32_t(args->unk_ptr),
                    uint32_t(args->info_ptr));
      if (!active_playlist_) {
        return X_E_FAIL;
      }
      auto& song = active_playlist_->songs[active_song_index_];
      memory::store_and_swap<uint32_t>(info + 0, song->handle);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 0, song->name);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 40, song->artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 80, song->album);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 120, song->album_artist);
      memory::store_and_swap<std::u16string>(info + 4 + 572 + 160, song->genre);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 200, song->track_number);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 204, song->duration_ms);
      memory::store_and_swap<uint32_t>(info + 4 + 572 + 208, static_cast<uint32_t>(song->format));
      return X_E_SUCCESS;
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(args->storage_ptr));
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      // XMPSetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller;
        rex::be<uint32_t> playback_client;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true((args->xmp_client == 0x00000002 && args->controller == 0x00000000) ||
                  (args->xmp_client == 0x00000000 && args->controller == 0x00000001));
      REXKRNL_DEBUG("XMPSetPlaybackController({:08X}, {:08X})", uint32_t(args->controller),
                    uint32_t(args->playback_client));

      playback_client_ = PlaybackClient(uint32_t(args->playback_client));
      kernel_state_->BroadcastNotification(kMsgPlaybackControllerChanged, !args->playback_client);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      // XMPGetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller_ptr;
        rex::be<uint32_t> locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetPlaybackController({:08X}, {:08X}, {:08X})", uint32_t(args->xmp_client),
                    uint32_t(args->controller_ptr), uint32_t(args->locked_ptr));
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->controller_ptr), 0);
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->locked_ptr), 0);

      if (!XThread::GetCurrentThread()->main_thread()) {
        // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
        rex::thread::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      // XMPGetPlaybackBehavior
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode_ptr;
        rex::be<uint32_t> repeat_mode_ptr;
        rex::be<uint32_t> unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                    uint32_t(args->playback_mode_ptr), uint32_t(args->repeat_mode_ptr),
                    uint32_t(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playback_mode_ptr),
                                         static_cast<uint32_t>(playback_mode_));
      }
      if (args->repeat_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->repeat_mode_ptr),
                                         static_cast<uint32_t>(repeat_mode_));
      }
      if (args->unk3_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->unk3_ptr), unknown_flags_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      // We don't use the storage, so just fudge the number.
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->size_ptr),
                                       4 + uint32_t(args->song_count) * 128);
      return X_E_SUCCESS;
    }
    case 0x0007003D: {
      // XMPCaptureOutput - not sure how this works :/
      REXKRNL_DEBUG("XMPCaptureOutput(...)");
      assert_always("XMP output not unimplemented");
      return X_E_FAIL;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
