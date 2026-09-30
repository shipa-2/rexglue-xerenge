#pragma once
/**
 * @file        main_android.h
 * @brief       Android process setup that has no counterpart elsewhere.
 */

#include <cstdint>

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

namespace rex {

// The device's API level (not the one the app was built for), read once.
int32_t GetAndroidApiLevel();

// Resolves the API-level-dependent functions the memory and threading layers
// load at run time (ASharedMemory_create, pthread_getname_np...). Called once,
// from the entry point, before anything maps guest memory or names a thread.
void InitializeAndroidApp();
void ShutdownAndroidApp();

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
