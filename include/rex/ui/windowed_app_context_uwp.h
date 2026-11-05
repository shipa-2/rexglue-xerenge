/**
 * @file        ui/windowed_app_context_uwp.h
 * @brief       UWP (CoreWindow) implementation of the windowed app UI loop -
 *              the Xbox One in Dev Mode, where SDL has no backend
 */

#pragma once

#include <rex/ui/windowed_app_context.h>

namespace rex::ui {

class UWPWindowedAppContext final : public WindowedAppContext {
 public:
  // Constructed on the CoreWindow's thread, which becomes the UI thread.
  UWPWindowedAppContext() = default;
  ~UWPWindowedAppContext() override;

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  int RunMainMessageLoop();

  // The CoreWindow, as an IUnknown* (winrt::get_abi).
  static void* CoreWindowAbi();
  // Keep the window IFrameworkView::SetWindow hands us; GetForCurrentThread
  // is not enough on Xbox for the compositor to replace the splash.
  static void SetCoreWindow(void* abi);
  // Activate and drain the dispatcher so the splash can go away.
  static void PumpEvents();
  static bool UILoopRunning();
};

}  // namespace rex::ui
