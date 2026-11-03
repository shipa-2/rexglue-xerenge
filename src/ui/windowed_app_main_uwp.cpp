/**
 * @file        ui/windowed_app_main_uwp.cpp
 * @brief       Entry point for windowed applications on UWP (the Xbox One in
 *              Dev Mode): the CoreApplication view whose thread is the UI thread
 *
 * There is no command line. The arguments are made here - the game in the
 * package's `game` folder (or LocalState\game), everything written into
 * LocalState - and LocalState\args.txt adds one argument per line.
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_uwp.h>
#include <rex/ui/uwp_game_setup.h>

namespace rex::ui {
namespace {
UwpGameSetupFn& GameSetup() {
  static UwpGameSetupFn setup = nullptr;
  return setup;
}
}  // namespace
void SetUwpGameSetup(UwpGameSetupFn setup) {
  GameSetup() = setup;
}
UwpGameSetupFn GetGameSetupForRun() {
  return GameSetup();
}
}  // namespace rex::ui

namespace {

using namespace winrt::Windows::ApplicationModel::Core;

std::vector<std::string> MakeArguments() {
  const std::filesystem::path package =
      std::wstring(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
  const std::filesystem::path local = std::wstring(
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
  std::filesystem::path game = package / "game";
  if (!std::filesystem::exists(game / "default.xex")) {
    game = local / "game";
  }
  std::error_code ec;
  std::filesystem::create_directories(local / "logs", ec);
  std::vector<std::string> args = {
      "burnout",
      "--game_data_root", game.string(),
      "--user_data_root", (local / "userdata").string(),
      "--cache_root", (local / "cache").string(),
      "--gpu_plugin", "plume",
      "--gpu_backend", "plume",
      "--input_backend", "xinput",
      "--log_file", (local / "logs" / "burnout.log").string(),
      "--log_max_file_size_mb", "32",
      "--log_max_files", "3",
      "--fullscreen",
  };
  std::ifstream extra(local / "args.txt");
  for (std::string line; std::getline(extra, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty() && line[0] != '#') {
      args.push_back(line);
    }
  }
  return args;
}

int RunWindowedApp() {
  auto utf8_args = MakeArguments();
  std::vector<char*> argv;
  for (auto& s : utf8_args) {
    argv.push_back(s.data());
  }
  auto remaining = rex::cvar::Init(int(argv.size()), argv.data());
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::UWPWindowedAppContext app_context;
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));
    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
    app->InvokeOnDestroy();
  }
  return result;
}

struct View : winrt::implements<View, IFrameworkViewSource, IFrameworkView> {
  IFrameworkView CreateView() { return *this; }
  void Initialize(CoreApplicationView const&) {}
  void Load(winrt::hstring const&) {}
  void Uninitialize() {}
  void SetWindow(winrt::Windows::UI::Core::CoreWindow const&) {}
  void Run() {
    // No game yet: the app's first-run step (the disc image, for Burnout).
    const std::filesystem::path package = std::wstring(
        winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    const std::filesystem::path local = std::wstring(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    if (!std::filesystem::exists(package / "game" / "default.xex") &&
        !std::filesystem::exists(local / "game" / "default.xex")) {
      auto setup = rex::ui::GetGameSetupForRun();
      winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Activate();
      if (!setup || !setup(local / "game")) {
        CoreApplication::Exit();
        return;
      }
    }
    // The renderer and shaders an Xbox One has: D3D12, shader model 5.1.
    _putenv_s("XERENGE_RENDER_API", "d3d12");
    _putenv_s("XERENGE_D3D12_SHADERS", "dxbc");
    int result = RunWindowedApp();
    CoreApplication::Exit();
    (void)result;
  }
};

}  // namespace

int __stdcall wWinMain(void*, void*, wchar_t*, int) {
  winrt::init_apartment();
  CoreApplication::Run(winrt::make<View>());
  return 0;
}
