/**
 * @file        ui/surface_android.cpp
 * @brief       The Android window surface.
 */

#include <android/native_window.h>

#include <rex/ui/surface_android.h>

namespace rex {
namespace ui {

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!window_) {
    return false;
  }
  const int32_t width = ANativeWindow_getWidth(window_);
  const int32_t height = ANativeWindow_getHeight(window_);
  if (width <= 0 || height <= 0) {
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace ui
}  // namespace rex
