/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdlib>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/renderdoc_api.h>

namespace rex {
namespace ui {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  // The vendored RenderDoc app API header doesn't expose a macOS path.
  // Keep RenderDoc optional and simply report "not connected" on this platform.
  return nullptr;
#else
  // XERENGE_NO_RENDERDOC=1 (Debug > RenderDoc off): never loaded, by any of the
  // instances that would, so RenderDoc's layer and overlay stay out.
  if (const char* off = std::getenv("XERENGE_NO_RENDERDOC"); off && *off == '1') {
    return nullptr;
  }
  std::unique_ptr<RenderDocAPI> renderdoc_api(new RenderDocAPI());

  pRENDERDOC_GetAPI get_api = nullptr;

  // Try to load the RenderDoc library. If RenderDoc is attached, the library
  // should already be loaded into the process and this will increment the
  // reference count. If not attached, the load will fail and we return nullptr.
  if (!renderdoc_api->library_.Load(platform::lib_names::kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  // get_api will be null if RenderDoc is not connected, or the API isn't
  // available on this platform, or there was an error.
  // 1.4.2 has DiscardFrameCapture (a frame captured and then thrown away);
  // an older RenderDoc answers only 1.0.0.
  if (get_api && get_api(eRENDERDOC_API_Version_1_4_2, (void**)&renderdoc_api->api_1_0_0_) &&
      renderdoc_api->api_1_0_0_) {
    renderdoc_api->can_discard_ = true;
  } else if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, (void**)&renderdoc_api->api_1_0_0_) ||
             !renderdoc_api->api_1_0_0_) {
    return nullptr;
  }

  REXLOG_INFO("RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace ui
}  // namespace rex

#if !REX_PLATFORM_WIN32 && !defined(__APPLE__)
#include <dlfcn.h>
#include <link.h>

#include <string>
#include <vector>

namespace rex {
namespace ui {

bool RenderDocAPI::UseCapturingCopy() {
  std::vector<std::string> copies;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        const std::string name = info->dlpi_name ? info->dlpi_name : "";
        if (name.find("librenderdoc") != std::string::npos) {
          static_cast<std::vector<std::string>*>(data)->push_back(name);
        }
        return 0;
      },
      &copies);
  for (const std::string& copy : copies) {
    void* handle = dlopen(copy.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!handle) {
      continue;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(handle, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_4_2* api = nullptr;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_4_2, (void**)&api) || !api) {
      continue;
    }
    api->StartFrameCapture(nullptr, nullptr);
    const bool capturing = api->IsFrameCapturing() != 0;
    if (capturing) {
      api->DiscardFrameCapture(nullptr, nullptr);
    }
    REXLOG_INFO("RenderDoc: {} {}", copy, capturing ? "captures - used" : "captures nothing");
    if (capturing) {
      api_1_0_0_ = api;
      can_discard_ = true;
      return true;
    }
  }
  return false;
}

}  // namespace ui
}  // namespace rex
#else
namespace rex {
namespace ui {
bool RenderDocAPI::UseCapturingCopy() {
  return false;
}
}  // namespace ui
}  // namespace rex
#endif
