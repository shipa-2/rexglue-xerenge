/**
 * @file        ui/window_uwp.h
 * @brief       UWP (CoreWindow) implementation of the Window abstraction
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/ui/window.h>

namespace rex::ui {

class WindowUWP final : public Window {
 public:
  WindowUWP(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~WindowUWP() override;

  // The CoreWindow (IUnknown*): plume makes its swap chain for it.
  void* GetNativeWindowHandle() const override;
  bool GetDisplayPixelSize(uint32_t& width, uint32_t& height) const override;

  // From the CoreWindow's events, on the UI thread.
  void HandleSizeChanged();
  void HandleVisibilityChanged(bool visible);
  void HandleClosed();

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void PhysicalSize(uint32_t& width, uint32_t& height) const;

  std::atomic<bool> paint_pending_{false};
  bool open_ = false;
};

}  // namespace rex::ui
