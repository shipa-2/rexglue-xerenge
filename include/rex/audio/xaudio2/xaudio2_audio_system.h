/**
 * @file        audio/xaudio2/xaudio2_audio_system.h
 * @brief       XAudio2 audio backend - UWP (the Xbox One), where SDL has no backend
 */

#pragma once

#include <rex/audio/audio_system.h>

namespace rex::audio::xaudio2 {

class XAudio2AudioSystem : public AudioSystem {
 public:
  explicit XAudio2AudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~XAudio2AudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;

 protected:
  void Initialize() override;
};

}  // namespace rex::audio::xaudio2
