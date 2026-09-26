/**
 * @file        system/interfaces/graphics.h
 * @brief       Abstract graphics system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/system/xtypes.h>

// Forward declarations
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class Window;
class WindowedAppContext;
}  // namespace rex::ui
namespace rex::system {
class KernelState;
}

namespace rex::system {

class IGraphicsSystem {
 public:
  virtual ~IGraphicsSystem() = default;

  // Build the provider + presenter. Safe to call standalone (without a
  // Runtime) to stand up a window + ImGui for an installer. Idempotent.
  // Must be called before SetupGuestGpu if presentation is desired: some
  // backends (e.g. Vulkan) bake swapchain support into the provider, and
  // a headless provider from SetupGuestGpu cannot be upgraded in place.
  virtual X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) = 0;

  // Wire the GPU into the guest address space: MMIO, command processor,
  // vsync worker. Needs the Runtime's dispatcher + kernel state. If
  // SetupPresentation has not been called, a headless provider is built.
  virtual X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                 KernelState* kernel_state) = 0;

  virtual bool has_presentation() const = 0;

  // Plume and other direct-present backends bind to the host window after it
  // exists. Called from the UI thread immediately after Window::Open().
  virtual void AttachPresentationWindow(ui::Window* window) {
    (void)window;
  }

  // When true, VdSwap bypasses PM4 injection and calls PresentGuestFrame().
  virtual bool uses_direct_presentation() const { return false; }

  // Host present hook for direct backends (UI or guest thread).
  virtual void PresentGuestFrame(uint32_t width, uint32_t height) {
    (void)width;
    (void)height;
  }

  // Plume D3D hook path: create a guest-visible shader object from a microcode
  // container. Returns guest VA, or 0 on failure / unsupported backend.
  virtual uint32_t CreateGuestShader(const void* shader_container, bool pixel_shader) {
    (void)shader_container;
    (void)pixel_shader;
    return 0;
  }

  // Plume D3D hook path: guest draw notifications (stats / future draw path).
  virtual void SubmitGuestDrawVerticesUP(uint32_t primitive_type, uint32_t vertex_count,
                                         uint32_t vertex_stride, uint32_t data_guest_va) {
    (void)primitive_type;
    (void)vertex_count;
    (void)vertex_stride;
    (void)data_guest_va;
  }

  virtual void NotifyGuestDrawIndexed(uint32_t index_count) { (void)index_count; }

  // Plume: guest IDirect3DDevice9 VA so VS/PS ALU can be copied from the D3D
  // shadow (device+0x700 / +0x1700) when PM4 never flushes them.
  virtual void BindGuestD3DDevice(uint32_t device_guest) { (void)device_guest; }

  // Whether a frame has reached the screen lately. Presentation is normally
  // driven by the swap packet in the command ring, but a title can stop
  // submitting to that ring while still drawing - and then the screen freezes
  // on the last frame that made it through. A backend that answers false here
  // is asking its Direct3D swap hook to present instead.
  virtual bool PresentedRecently() const { return true; }

  // The title asked for its render target to be cleared. Flags are Direct3D's
  // own on this console: the low four bits are the colour targets, 0x10 depth,
  // 0x20 stencil. A backend that draws the title's Direct3D calls itself has to
  // honour these where they happen - depth above all, since this title tests
  // with GEQUAL and clears to its own value rather than to 1.
  // Direct3D has just handed the GPU a shader's microcode: `size` bytes at the
  // guest virtual address `microcode`, for the shader object `object`. These
  // are the exact bytes the GPU receives - after Direct3D's own patching - so
  // they identify the translated shader precisely, without the command ring.
  virtual void NoteShaderLoad(uint32_t object, bool pixel_shader, uint32_t microcode,
                              uint32_t size) {
    (void)object;
    (void)pixel_shader;
    (void)microcode;
    (void)size;
  }

  virtual void NoteGuestClear(uint32_t flags, float depth, uint32_t stencil) {
    (void)flags;
    (void)depth;
    (void)stencil;
  }

  // The same, with the colour Direct3D was asked to clear to, and whether the
  // clear covers the whole target (no rectangles). Backends that draw every
  // target into one buffer, as EDRAM is one buffer, clear colour only then.
  virtual void NoteGuestClearColor(uint32_t flags, const float color[4], bool whole_target,
                                   float depth) {
    (void)flags;
    (void)color;
    (void)whole_target;
    (void)depth;
  }

  // D3DDevice_Resolve: what is in the render target now is copied into the
  // texture whose memory starts at `dest_physical`. Later draws that sample
  // that texture want this picture, not what guest memory holds there.
  // `source_width`/`source_height` give the render target's own size (0 if
  // unknown), `face` the cube face or slice copied into, `cube` whether the
  // destination is a cube map.
  virtual void NoteGuestResolve(uint32_t flags, uint32_t dest_physical,
                                uint32_t source_width = 0, uint32_t source_height = 0,
                                uint32_t face = 0, bool cube = false) {
    (void)flags;
    (void)dest_physical;
    (void)source_width;
    (void)source_height;
    (void)face;
    (void)cube;
  }

  // The title called Swap: everything it issued before this belongs to the
  // frame being presented, anything after it to the next one.
  virtual void NoteGuestFrameEnd() {}

  // A draw the title issued through Direct3D. A backend that reads the command
  // ring sees these as packets and can ignore this; one that does not - or one
  // whose ring never carries them - gets the draw here instead, and fills in
  // the rest from the state it already tracks.
  struct GuestDrawBuffers {
    uint32_t vertex_buffer = 0;
    uint32_t vertex_stride = 0;
    uint32_t index_buffer = 0;
    uint32_t base_vertex = 0;
    uint32_t start_index = 0;
    uint32_t vertex_shader_object = 0;
    uint32_t pixel_shader_object = 0;
    // The textures bound when the draw was made. A draw that never goes
    // through the command ring brings no texture fetch constants with it, and
    // without these it samples whatever was last bound for something else -
    // or nothing, and comes out black however right its geometry is.
    uint32_t textures[8] = {};
    // The size of the render target being drawn into. Full-screen passes are
    // drawn in its pixels, with the viewport transform off, so their positions
    // mean nothing without it.
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    // 32-bit indices: bit 31 of the index buffer's Common word, which is what
    // D3DDevice_DrawIndexedVertices tests.
    bool index_32bit = false;
    // Every stream the title set with SetStreamSource: data address (offset
    // applied) and stride in bytes. Stream i is vertex fetch constant 95 - i.
    uint32_t stream_address[4] = {};
    uint32_t stream_stride[4] = {};
  };
  virtual void NoteGuestDraw(uint32_t primitive_type, uint32_t index_count,
                             const GuestDrawBuffers& buffers) {
    (void)primitive_type;
    (void)index_count;
    (void)buffers;
  }

  // Tell the backend which translated shader a guest shader object stands for.
  // The title creates its shaders through Direct3D and then refers to them by
  // object address for the rest of the run, so a backend driven from those
  // calls needs the association; one driven from the command ring sees the
  // microcode directly and can ignore this.
  virtual void RegisterGuestShaderObject(uint32_t object_guest, const void* shader_container,
                                         bool pixel_shader) {
    (void)object_guest;
    (void)shader_container;
    (void)pixel_shader;
  }

  // --- Optional capabilities, default no-op -------------------------------

  // Host presentation objects for ReXApp's overlay wiring; custom systems may
  // leave these null.
  virtual ui::GraphicsProvider* provider() const { return nullptr; }
  virtual ui::Presenter* presenter() const { return nullptr; }

  // Guest GPU services reached from the xboxkrnl Vd* exports.
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    (void)callback;
    (void)user_data;
  }
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    (void)ptr;
    (void)size_log2;
  }
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    (void)ptr;
    (void)block_size_log2;
  }

  // Persistent shader/pipeline storage under the cache root. Default: none.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking) {
    (void)cache_root;
    (void)title_id;
    (void)blocking;
  }

  // One-shot convenience for callers that don't care about the split.
  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher, KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) {
    if (with_presentation && !has_presentation()) {
      X_STATUS status = SetupPresentation(app_context);
      if (XFAILED(status)) {
        return status;
      }
    }
    return SetupGuestGpu(function_dispatcher, kernel_state);
  }

  virtual void Shutdown() = 0;
};

}  // namespace rex::system
