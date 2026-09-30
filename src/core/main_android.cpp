/**
 * @file        main_android.cpp
 * @brief       Android process setup.
 */

#include <android/api-level.h>

#include <rex/main_android.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace rex {

int32_t GetAndroidApiLevel() {
  static const int32_t level = android_get_device_api_level();
  return level;
}

void InitializeAndroidApp() {
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  memory::AndroidInitialize();
  thread::AndroidInitialize();
}

void ShutdownAndroidApp() {
  thread::AndroidShutdown();
  memory::AndroidShutdown();
}

}  // namespace rex
