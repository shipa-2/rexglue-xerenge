/**
 * @file        audio/xaudio2/xaudio2_audio_driver.h
 * @brief       XAudio2 output: each frame the title submits, folded to stereo
 *              and queued on one source voice; its end frees the title's slot
 */

#pragma once

#include <mutex>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace rex::audio::xaudio2 {

class XAudio2AudioDriver : public AudioDriver {
 public:
  XAudio2AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~XAudio2AudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void DiscardPendingFrames() override;
  void Shutdown();

  // From the voice callback, on XAudio2's thread.
  void OnBufferEnd(void* context);

 private:
  struct VoiceCallback;

  rex::thread::Semaphore* semaphore_ = nullptr;
  IXAudio2* xaudio2_ = nullptr;
  IXAudio2MasteringVoice* mastering_voice_ = nullptr;
  IXAudio2SourceVoice* source_voice_ = nullptr;
  VoiceCallback* callback_ = nullptr;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = 6 * channel_samples_;
  static const uint32_t output_samples_ = 2 * channel_samples_;
  std::stack<float*> buffers_unused_ = {};
  std::mutex buffers_mutex_ = {};
};

}  // namespace rex::audio::xaudio2
