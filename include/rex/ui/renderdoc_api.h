#pragma once
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

#include <memory>

#include <rex/platform/dynlib.h>

#if REX_PLATFORM_MAC
struct RENDERDOC_API_1_0_0;
#else
#include <renderdoc_app.h>
#endif

namespace rex {
namespace ui {

class RenderDocAPI {
 public:
  static std::unique_ptr<RenderDocAPI> CreateIfConnected();

  RenderDocAPI(const RenderDocAPI&) = delete;
  RenderDocAPI& operator=(const RenderDocAPI&) = delete;

  ~RenderDocAPI();

  // Always present if this object exists.
  const RENDERDOC_API_1_0_0* api_1_0_0() const { return api_1_0_0_; }
  // DiscardFrameCapture is there (RenderDoc 1.4.2 and later).
  bool can_discard() const { return can_discard_; }
  // Another copy of the library already in the process whose frame capture works
  // (a frame started there is being captured), its API taken in place of this
  // one's. Two copies end up loaded when the Vulkan layer RenderDoc registered
  // is another install than the library found by name: the layer's copy hooks
  // Vulkan and F12 works, the other knows of no device and captures nothing.
  // Call once a frame has been presented. False when none was found.
  bool UseCapturingCopy();

 private:
  explicit RenderDocAPI() = default;

  rex::platform::DynamicLibrary library_;

  const RENDERDOC_API_1_0_0* api_1_0_0_ = nullptr;
  bool can_discard_ = false;
};

}  // namespace ui
}  // namespace rex
