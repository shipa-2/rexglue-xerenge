/**
 * @file        ui/windowed_app_context_uwp.cpp
 * @brief       UWP (CoreWindow) implementation of the windowed app UI loop
 */

#include <rex/ui/windowed_app_context_uwp.h>

#include <cstdlib>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

namespace rex::ui {

using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Core::CoreProcessEventsOption;
using winrt::Windows::UI::Core::CoreWindow;

namespace {
// The UI thread's window, kept for the threads that post to its dispatcher.
CoreWindow g_core_window{nullptr};
}  // namespace

UWPWindowedAppContext::~UWPWindowedAppContext() {
  ExecutePendingFunctionsFromUIThread();
}

void* UWPWindowedAppContext::CoreWindowAbi() {
  if (!g_core_window) {
    g_core_window = CoreWindow::GetForCurrentThread();
  }
  return winrt::get_abi(g_core_window);
}

void UWPWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // RunAsync may be called from any thread; the dispatcher runs the call on
  // the UI thread from inside ProcessEvents.
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
  auto dispatcher = g_core_window.Dispatcher();
  // Anything posted before the loop started.
  ExecutePendingFunctionsFromUIThread();
  while (!HasQuitFromUIThread()) {
    // Blocks until there is an event or a posted call, then handles all of them.
    dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
  }
  return EXIT_SUCCESS;
}

}  // namespace rex::ui
