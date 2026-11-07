/**
 * @file        ui/windowed_app_main_uwp.cpp
 * @brief       Entry point for windowed applications on UWP (the Xbox One in
 *              Dev Mode): the CoreApplication view whose thread is the UI thread
 *
 * There is no command line. The arguments are made here - the game in the
 * package's `game` folder, or on the developer scratch drive, or (if that
 * drive cannot be written) in LocalState\game. Saves and logs stay in
 * LocalState, and LocalState\args.txt adds one argument per line.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

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

std::vector<std::string>& ConfArguments();

// Dev Home's "Manage developer storage" resizes this drive, not LocalState,
// which keeps a few GB - too little for the extracted disc.
const std::filesystem::path kScratchGame(LR"(D:\DevelopmentFiles\Xerenge\game)");

bool ScratchAcceptsFiles() {
  std::error_code ec;
  std::filesystem::create_directories(kScratchGame.parent_path(), ec);
  if (ec) {
    return false;
  }
  const auto probe = kScratchGame.parent_path() / L".write_probe";
  {
    std::ofstream out(probe, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.put('x');
  }
  std::filesystem::remove(probe, ec);
  return true;
}

// A game already on disk wins. Otherwise the scratch drive, then LocalState.
std::filesystem::path GameDirectory(const std::filesystem::path& package,
                                    const std::filesystem::path& local) {
  if (std::filesystem::exists(package / "game" / "default.xex")) {
    return package / "game";
  }
  if (std::filesystem::exists(kScratchGame / "default.xex")) {
    return kScratchGame;
  }
  if (std::filesystem::exists(local / "game" / "default.xex")) {
    return local / "game";
  }
  if (ScratchAcceptsFiles()) {
    return kScratchGame;
  }
  return local / "game";
}

std::vector<std::string> MakeArguments() {
  const std::filesystem::path package =
      std::wstring(winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
  const std::filesystem::path local = std::wstring(
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
  const std::filesystem::path game = GameDirectory(package, local);
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
  args.insert(args.end(), ConfArguments().begin(), ConfArguments().end());
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

// There is no console: what the renderer prints to stderr (a pipeline it could
// not make, among others) is kept beside burnout.log.
void KeepStderr() {
  const std::filesystem::path logs =
      std::filesystem::path(std::wstring(
          winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path())) /
      "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs, ec);
  FILE* file = nullptr;
  if (_wfreopen_s(&file, (logs / "stderr.log").c_str(), L"w", stderr) == 0 && file) {
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::fputs("stderr kept here\n", stderr);
  }
}

// Arguments ApplyConf adds; MakeArguments puts them before args.txt's.
std::vector<std::string>& ConfArguments() {
  static std::vector<std::string> args;
  return args;
}

// What the desktop launcher (installer/launcher.cpp, Prepare) sets from
// xerenge.conf before it starts the game; nothing starts this one but the
// shell, so the same is done here. Without the plume modes (XERENGE_D3D_TARGETS
// and the rest) the picture is a dark smear.
void ApplyConf(const std::filesystem::path& conf_path) {
  std::map<std::string, std::string> conf;
  std::ifstream in(conf_path);
  for (std::string line; std::getline(in, line);) {
    const auto trim = [](std::string s) {
      const size_t first = s.find_first_not_of(" \t\r");
      const size_t last = s.find_last_not_of(" \t\r");
      return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    };
    line = trim(line);
    const size_t equals = line.find('=');
    if (line.empty() || line[0] == '#' || equals == std::string::npos) {
      continue;
    }
    const std::string key = trim(line.substr(0, equals));
    if (!key.empty()) {
      conf[key] = trim(line.substr(equals + 1));
    }
  }
  const auto get = [&](const char* key) {
    const auto found = conf.find(key);
    return found == conf.end() ? std::string() : found->second;
  };
  const auto has = [&](const char* key) { return !get(key).empty(); };
  const auto is = [&](const char* key, const char* value) {
    std::string v = get(key);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return v == value;
  };
  const auto env = [](const char* name, const std::string& value) {
    _putenv_s(name, value.c_str());
  };

  std::fprintf(stderr, "settings: %s (%s)\n", conf_path.string().c_str(),
               std::filesystem::exists(conf_path) ? "read" : "missing, defaults");
  if (!is("bloom", "true")) env("XERENGE_NO_BLOOM", "1");
  if (!is("motion_blur", "true")) env("XERENGE_NO_MOTION_BLUR", "1");
  // plume is the only renderer this build has.
  for (const char* name : {"XERENGE_D3D_TARGETS", "XERENGE_D3D_UI", "XERENGE_SKIP_LOGOS",
                           "XERENGE_REAL_SHADERS", "XERENGE_D3D_DRAWS", "XERENGE_SECONDARY_TICKS"}) {
    env(name, "1");
  }
  if (has("language")) env("XERENGE_LANGUAGE", get("language"));
  if (is("async_present", "true")) env("XERENGE_ASYNC_PRESENT", "1");
  if (is("early_submit", "false")) env("XERENGE_ACQUIRE_FIRST", "1");
  if (is("packed_vertices", "false")) env("XERENGE_FULL_VERTICES", "1");
  if (is("culling", "true")) env("XERENGE_CULL", "1");
  if (is("reflection_split", "true")) env("XERENGE_REFLECTION_SPLIT", "1");
  if (has("render_resolution")) env("XERENGE_RENDER_RESOLUTION", get("render_resolution"));
  if (is("anisotropy", "off")) {
    env("XERENGE_MAX_ANISOTROPY", "1");
  } else if (has("anisotropy")) {
    env("XERENGE_MAX_ANISOTROPY", get("anisotropy"));
  }
  if (has("colour_grade")) env("XERENGE_COLOUR_GRADE", get("colour_grade"));
  if (has("aa")) env("XERENGE_AA", get("aa"));
  if (has("sharpen")) env("XERENGE_SHARPEN", get("sharpen"));
  if (has("brightness")) env("XERENGE_BRIGHTNESS", get("brightness"));
  if (has("shadow_lift")) env("XERENGE_SHADOW_LIFT", get("shadow_lift"));
  if (has("power_mode")) env("XERENGE_POWER_MODE", get("power_mode"));
  if (has("spin_lead_ms")) env("XERENGE_SPIN_LEAD_MS", get("spin_lead_ms"));
  if (is("fps_counter", "true")) env("XERENGE_FPS_SHOW", "1");
  if (has("prompts")) env("XERENGE_PROMPTS", get("prompts"));
  if (is("fps_30", "true")) env("XERENGE_FPS_30", "1");
  if (is("frame_generation", "true")) env("XERENGE_FRAME_GENERATION", "120");
  if (is("crash_interpolation", "false")) env("XERENGE_CRASH_INTERPOLATION", "0");
  if (!is("aspect_16_9", "false")) env("XERENGE_ASPECT_16_9", "1");
  if (has("menu_video")) env("XERENGE_MENU_VIDEO", get("menu_video"));
  env("XERENGE_NO_RENDERDOC", "1");
  if (is("debug", "true")) {
    env("XERENGE_DEBUG", "1");
    if (is("debug_gpu_trace", "true")) env("XERENGE_GPU_TRACE", "1");
    if (is("debug_movie_trace", "true")) env("XERENGE_MOVIE_TRACE", "1");
    if (is("debug_pipeline_log", "true")) env("XERENGE_PIPELINE_LOG", "1");
    if (!is("debug_vertex_trace", "false")) env("XERENGE_VERTEX_TRACE", "1");
    if (is("debug_constant_trace", "true")) env("XERENGE_CONSTANT_TRACE", "1");
    if (has("debug_video_gpu")) env("XERENGE_VIDEO_GPU", get("debug_video_gpu"));
  }
  // The launcher's arguments from the conf (Prepare's end): the gamertag, and
  // online play. There is no installer here to turn it on (its Network tab),
  // so on the Xbox it is on unless the conf says online = false: this copy
  // runs its own lobby and finds the others on the LAN, or uses lobby_server.
  if (has("gamertag")) ConfArguments().push_back("--gamertag=" + get("gamertag"));
  if (has("lobby_server") || !is("online", "false")) {
    ConfArguments().push_back("--online");
    ConfArguments().push_back("--online_fake_lobby=false");
    if (has("lobby_server")) ConfArguments().push_back("--lobby_server=" + get("lobby_server"));
  }
}

// There is no environment to start the game with either: LocalState\env.txt
// sets one NAME=VALUE per line (XERENGE_NO_BC=1 and the like), after the ones
// set here, so it can override them.
void ApplyEnvFile() {
  const std::filesystem::path local = std::wstring(
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
  std::ifstream in(local / "env.txt");
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    const size_t equals = line.find('=');
    if (line.empty() || line[0] == '#' || equals == std::string::npos || equals == 0) {
      continue;
    }
    const std::string name = line.substr(0, equals), value = line.substr(equals + 1);
    _putenv_s(name.c_str(), value.c_str());
    std::fprintf(stderr, "env.txt: %s=%s\n", name.c_str(), value.c_str());
  }
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
  void SetWindow(winrt::Windows::UI::Core::CoreWindow const& window) {
    rex::ui::UWPWindowedAppContext::SetCoreWindow(winrt::get_abi(window));
    // B on an Xbox pad is the system's Back: left unhandled, it closes the app
    // and drops the player into Dev Home. The game reads B itself.
    winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [](auto const&, winrt::Windows::UI::Core::BackRequestedEventArgs const& args) {
          args.Handled(true);
        });
  }
  void Run() {
    // No game yet: the app's first-run step (the disc image, for Burnout).
    const std::filesystem::path package = std::wstring(
        winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());
    const std::filesystem::path local = std::wstring(
        winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    // Activated once, here, as the probe that drew on this console did.
    winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Activate();
    KeepStderr();
    const std::filesystem::path game = GameDirectory(package, local);
    std::fprintf(stderr, "game folder: %s (%s)\n", game.string().c_str(),
                 std::filesystem::exists(game / "default.xex") ? "installed" : "not installed");
    if (!std::filesystem::exists(game / "default.xex")) {
      auto setup = rex::ui::GetGameSetupForRun();
      if (!setup || !setup(game)) {
        CoreApplication::Exit();
        return;
      }
    }
    // The renderer and shaders an Xbox One has: D3D12, shader model 5.1.
    _putenv_s("XERENGE_RENDER_API", "d3d12");
    _putenv_s("XERENGE_D3D12_SHADERS", "dxbc");
    ApplyConf(game.parent_path() / "xerenge.conf");
    ApplyEnvFile();
    int result = RunWindowedApp();
    CoreApplication::Exit();
    (void)result;
  }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment();
  CoreApplication::Run(winrt::make<View>());
  return 0;
}
