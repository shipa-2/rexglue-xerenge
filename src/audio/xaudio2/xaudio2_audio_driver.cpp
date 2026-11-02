/**
 * @file        audio/xaudio2/xaudio2_audio_driver.cpp
 * @brief       XAudio2 output (UWP)
 */

#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/xaudio2/xaudio2_audio_driver.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

namespace rex::audio::xaudio2 {

struct XAudio2AudioDriver::VoiceCallback final : IXAudio2VoiceCallback {
  explicit VoiceCallback(XAudio2AudioDriver* driver) : driver(driver) {}
  void STDMETHODCALLTYPE OnBufferEnd(void* context) override { driver->OnBufferEnd(context); }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
    REXAPU_ERROR("XAudio2 voice error {:08X}", static_cast<uint32_t>(error));
  }
  XAudio2AudioDriver* driver;
};

XAudio2AudioDriver::XAudio2AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

XAudio2AudioDriver::~XAudio2AudioDriver() {
  assert_true(buffers_unused_.empty());
}

bool XAudio2AudioDriver::Initialize() {
  HRESULT hr = XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    REXAPU_ERROR("XAudio2Create failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }
  hr = xaudio2_->CreateMasteringVoice(&mastering_voice_);
  if (FAILED(hr)) {
    REXAPU_ERROR("CreateMasteringVoice failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }
  WAVEFORMATEX format = {};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = 2;
  format.nSamplesPerSec = frame_frequency_;
  format.wBitsPerSample = 32;
  format.nBlockAlign = format.nChannels * sizeof(float);
  format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
  callback_ = new VoiceCallback(this);
  hr = xaudio2_->CreateSourceVoice(&source_voice_, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                   callback_);
  if (FAILED(hr)) {
    REXAPU_ERROR("CreateSourceVoice failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }
  source_voice_->Start();
  REXAPU_INFO("audio: XAudio2, 2 ch, {} Hz", frame_frequency_);
  return true;
}

void XAudio2AudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output;
  {
    std::unique_lock<std::mutex> guard(buffers_mutex_);
    if (buffers_unused_.empty()) {
      output = new float[output_samples_];
    } else {
      output = buffers_unused_.top();
      buffers_unused_.pop();
    }
  }
  if (REXCVAR_GET(audio_mute)) {
    std::memset(output, 0, output_samples_ * sizeof(float));
  } else {
    conversion::sequential_6_BE_to_interleaved_2_LE(output, input_frame, channel_samples_,
                                                    GetStereoFold(), GetOutputGain());
  }
  XAUDIO2_BUFFER buffer = {};
  buffer.AudioBytes = output_samples_ * sizeof(float);
  buffer.pAudioData = reinterpret_cast<const BYTE*>(output);
  buffer.pContext = output;
  if (FAILED(source_voice_->SubmitSourceBuffer(&buffer))) {
    OnBufferEnd(output);
  }
}

void XAudio2AudioDriver::OnBufferEnd(void* context) {
  {
    std::unique_lock<std::mutex> guard(buffers_mutex_);
    buffers_unused_.push(static_cast<float*>(context));
  }
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

void XAudio2AudioDriver::DiscardPendingFrames() {
  // Flushed buffers still end, and each frees its slot as it does.
  if (source_voice_) {
    source_voice_->FlushSourceBuffers();
  }
}

void XAudio2AudioDriver::Shutdown() {
  if (source_voice_) {
    source_voice_->Stop();
    source_voice_->FlushSourceBuffers();
    source_voice_->DestroyVoice();
    source_voice_ = nullptr;
  }
  if (mastering_voice_) {
    mastering_voice_->DestroyVoice();
    mastering_voice_ = nullptr;
  }
  if (xaudio2_) {
    xaudio2_->Release();
    xaudio2_ = nullptr;
  }
  delete callback_;
  callback_ = nullptr;
  std::unique_lock<std::mutex> guard(buffers_mutex_);
  while (!buffers_unused_.empty()) {
    delete[] buffers_unused_.top();
    buffers_unused_.pop();
  }
}

}  // namespace rex::audio::xaudio2
