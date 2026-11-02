/**
 * @file        audio/xaudio2/xaudio2_audio_system.cpp
 * @brief       XAudio2 audio backend
 */

#include <rex/audio/xaudio2/xaudio2_audio_driver.h>
#include <rex/audio/xaudio2/xaudio2_audio_system.h>

namespace rex::audio::xaudio2 {

std::unique_ptr<AudioSystem> XAudio2AudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<XAudio2AudioSystem>(function_dispatcher);
}

XAudio2AudioSystem::XAudio2AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

XAudio2AudioSystem::~XAudio2AudioSystem() {}

void XAudio2AudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS XAudio2AudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                          rex::thread::Semaphore* semaphore,
                                          AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new XAudio2AudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void XAudio2AudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto xdriver = static_cast<XAudio2AudioDriver*>(driver);
  xdriver->Shutdown();
  delete xdriver;
}

}  // namespace rex::audio::xaudio2
