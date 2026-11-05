/**
 * @file        ui/windowed_app_context_uwp.cpp
 * @brief       UWP (CoreWindow) implementation of the windowed app UI loop
 */

#include <rex/ui/windowed_app_context_uwp.h>

#include <atomic>
#include <cstdlib>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

namespace rex::ui {

using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

namespace {
CoreWindow g_core_window{nullptr};
std::atomic<bool> g_loop_running{false};
}  // namespace

UWPWindowedAppContext::~UWPWindowedAppContext() {
  ExecutePendingFunctionsFromUIThread();
}

void UWPWindowedAppContext::SetCoreWindow(void* abi) {
  g_core_window = nullptr;
  if (abi) {
    winrt::copy_from_abi(g_core_window, abi);
  }
}

bool UWPWindowedAppContext::UILoopRunning() {
  return g_loop_running.load();
}

void UWPWindowedAppContext::PumpEvents() {
  if (!g_core_window) {
    g_core_window = CoreWindow::GetForCurrentThread();
  }
  if (!g_core_window) {
    return;
  }
  g_core_window.Activate();
  g_core_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
}

void* UWPWindowedAppContext::CoreWindowAbi() {
  if (!g_core_window) {
    g_core_window = CoreWindow::GetForCurrentThread();
  }
  return winrt::get_abi(g_core_window);
}

void UWPWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  if (g_core_window) {
    g_core_window.Dispatcher().RunAsync(CoreDispatcherPriority::Normal,
                                        [this] { ExecutePendingFunctionsFromUIThread(); });
  }
}

void UWPWindowedAppContext::PlatformQuitFromUIThread() {
  NotifyUILoopOfPendingFunctions();
}

int UWPWindowedAppContext::RunMainMessageLoop() {
  if (!g_core_window) {
    g_core_window = CoreWindow::GetForCurrentThread();
  }
  g_loop_running.store(true);
  auto dispatcher = g_core_window.Dispatcher();
  ExecutePendingFunctionsFromUIThread();
  while (!HasQuitFromUIThread()) {
    dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
  }
  return EXIT_SUCCESS;
}

}  // namespace rex::ui
