/**
 * @file        rex/core/fiber_android.cpp
 * @brief       Android backend for rex::thread::Fiber
 *
 * Bionic has no getcontext/makecontext/swapcontext, so the switch is done by
 * hand: the callee-saved registers the AAPCS64 obliges a call to preserve
 * (x19-x30, d8-d15) go onto the running stack, the stack pointer is stored in
 * the fiber being left, and the target's stack pointer is loaded and its
 * registers popped. A new fiber's stack starts with such a frame whose x30 is
 * Trampoline, so the first switch to it "returns" there.
 */

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

#if !defined(__aarch64__)
#error "rex::thread::Fiber on Android is implemented for aarch64 only"
#endif

// void rex_fiber_switch(void** save_sp, void* load_sp)
extern "C" void rex_fiber_switch(void** save_sp, void* load_sp);
asm(R"(
    .text
    .p2align 4
    .globl rex_fiber_switch
    .hidden rex_fiber_switch
    .type rex_fiber_switch, %function
rex_fiber_switch:
    sub  sp, sp, #160
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x2, sp
    str  x2, [x0]
    mov  sp, x1
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #160
    ret
    .size rex_fiber_switch, .-rex_fiber_switch
)");

namespace rex::thread {

namespace {
constexpr size_t kFrameBytes = 160;
constexpr size_t kLinkRegisterOffset = 88;  // x30 in the frame above
}  // namespace

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  // sp_ is filled in by the first switch away from this thread.
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  auto top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size()) & ~uintptr_t(15);
  auto* frame = reinterpret_cast<uint8_t*>(top - kFrameBytes);
  std::memset(frame, 0, kFrameBytes);
  const auto start = reinterpret_cast<uintptr_t>(&Fiber::Trampoline);
  std::memcpy(frame + kLinkRegisterOffset, &start, sizeof(start));
  f->sp_ = frame;
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before the switch landed here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // A fiber's entry never returns (the ucontext backend would end the thread),
  // and there is no frame below this one to return to.
  std::abort();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_ANDROID
