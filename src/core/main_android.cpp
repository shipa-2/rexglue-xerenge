/**
 * @file        main_android.cpp
 * @brief       Android process setup.
 */

#include <android/api-level.h>
#include <android/log.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include <rex/main_android.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace rex {

namespace {

// An app's stdout and stderr go nowhere on Android, and libraries (plume, the
// Vulkan loader, SDL) report their failures there. A pipe in their place,
// read line by line into logcat under the tag "rex".
void RedirectStdioToLogcat() {
  int fds[2];
  if (pipe(fds) != 0) {
    return;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  std::thread([read_fd = fds[0]] {
    char buffer[1024];
    size_t used = 0;
    for (;;) {
      const ssize_t got = read(read_fd, buffer + used, sizeof(buffer) - 1 - used);
      if (got <= 0) {
        break;
      }
      used += size_t(got);
      size_t start = 0;
      for (size_t i = 0; i < used; ++i) {
        if (buffer[i] == '\n') {
          buffer[i] = '\0';
          __android_log_write(ANDROID_LOG_INFO, "rex", buffer + start);
          start = i + 1;
        }
      }
      if (start == 0 && used == sizeof(buffer) - 1) {
        buffer[used] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "rex", buffer);
        used = 0;
      } else {
        std::memmove(buffer, buffer + start, used - start);
        used -= start;
      }
    }
  }).detach();
}

}  // namespace

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
  RedirectStdioToLogcat();
  memory::AndroidInitialize();
  thread::AndroidInitialize();
}

void ShutdownAndroidApp() {
  thread::AndroidShutdown();
  memory::AndroidShutdown();
}

}  // namespace rex
