/**
 * @file        ui/window_uwp.cpp
 * @brief       UWP (CoreWindow) implementation of the Window abstraction - one
 *              full-screen window, the app's own; nothing to open or move
 */

#include <rex/ui/window_uwp.h>

#include <cmath>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <rex/logging.h>
#include <rex/ui/windowed_app_context_uwp.h>

namespace rex::ui {

using winrt::Windows::Graphics::Display::DisplayInformation;
using winrt::Windows::UI::Core::CoreWindow;

namespace {
WindowUWP* g_window = nullptr;
winrt::event_token g_size_token, g_visibility_token, g_closed_token;
}  // namespace

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title) {
  return std::make_unique<WindowUWP>(app_context, title, 1920, 1080);
}

WindowUWP::WindowUWP(WindowedAppContext& app_context, const std::string_view title,
                     uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

WindowUWP::~WindowUWP() {
  EnterDestructor();
  if (g_window == this) {
    CoreWindow window = CoreWindow::GetForCurrentThread();
    if (window) {
      window.SizeChanged(g_size_token);
      window.VisibilityChanged(g_visibility_token);
      window.Closed(g_closed_token);
    }
    g_window = nullptr;
  }
}

void* WindowUWP::GetNativeWindowHandle() const {
  return UWPWindowedAppContext::CoreWindowAbi();
}

void WindowUWP::PhysicalSize(uint32_t& width, uint32_t& height) const {
  CoreWindow window = CoreWindow::GetForCurrentThread();
  double scale = 1.0;
  if (auto info = DisplayInformation::GetForCurrentView()) {
    scale = info.RawPixelsPerViewPixel();
  }
  auto bounds = window ? window.Bounds() : winrt::Windows::Foundation::Rect{0, 0, 1920, 1080};
  width = uint32_t(std::lround(bounds.Width * scale));
  height = uint32_t(std::lround(bounds.Height * scale));
  if (!width || !height) {
    width = 1920;
    height = 1080;
  }
}

bool WindowUWP::GetDisplayPixelSize(uint32_t& width, uint32_t& height) const {
  if (auto info = DisplayInformation::GetForCurrentView()) {
    width = info.ScreenWidthInRawPixels();
    height = info.ScreenHeightInRawPixels();
    if (width && height) {
      return true;
    }
  }
  PhysicalSize(width, height);
  return true;
}

bool WindowUWP::OpenImpl() {
  CoreWindow window = CoreWindow::GetForCurrentThread();
  if (!window) {
    REXLOG_ERROR("UWP: no CoreWindow on the UI thread");
    return false;
  }
  // The whole screen, not the TV-safe area the Xbox gives an app by default.
  winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
      winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
  g_window = this;
  g_size_token = window.SizeChanged([](auto&&, auto&&) {
    if (g_window) g_window->HandleSizeChanged();
  });
  g_visibility_token = window.VisibilityChanged([](auto&&, auto&& args) {
    if (g_window) g_window->HandleVisibilityChanged(args.Visible());
  });
  g_closed_token = window.Closed([](auto&&, auto&&) {
    if (g_window) g_window->HandleClosed();
  });
  window.Activate();
  open_ = true;

  uint32_t width = 0, height = 0;
  PhysicalSize(width, height);
  REXLOG_INFO("UWP: CoreWindow {}x{}", width, height);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  OnFocusUpdate(true, destruction_receiver);
  return true;
}

void WindowUWP::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  open_ = false;
  OnAfterClose();
}

std::unique_ptr<Surface> WindowUWP::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  // plume makes its swap chain from the CoreWindow itself.
  (void)allowed_types;
  return nullptr;
}

void WindowUWP::RequestPaintImpl() {
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  app_context().CallInUIThreadDeferred([this] {
    paint_pending_.store(false, std::memory_order_release);
    if (open_) {
      OnPaint();
    }
  });
}

void WindowUWP::HandleSizeChanged() {
  uint32_t width = 0, height = 0;
  PhysicalSize(width, height);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
}

void WindowUWP::HandleVisibilityChanged(bool visible) {
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(visible, destruction_receiver);
}

void WindowUWP::HandleClosed() {
  if (open_) {
    RequestCloseImpl();
  }
}

}  // namespace rex::ui
