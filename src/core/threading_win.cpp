/**
 * @file        core/threading_win.cpp
 * @brief       Windows platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include "platform_win.h"

#include <spdlog/spdlog.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>

#define LOG_LASTERROR() \
  { spdlog::error("Win32 Error 0x{:08X} in {}(...)", GetLastError(), __FUNCTION__); }

typedef HANDLE (*SetThreadDescriptionFn)(HANDLE hThread, PCWSTR lpThreadDescription);

namespace rex::thread {

void EnableAffinityConfiguration() {
  HANDLE process_handle = GetCurrentProcess();
  DWORD_PTR process_affinity_mask;
  DWORD_PTR system_affinity_mask;
  GetProcessAffinityMask(process_handle, &process_affinity_mask, &system_affinity_mask);
  SetProcessAffinityMask(process_handle, system_affinity_mask);
}

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(GetCurrentThreadId());
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
struct THREADNAME_INFO {
  DWORD dwType;      // Must be 0x1000.
  LPCSTR szName;     // Pointer to name (in user addr space).
  DWORD dwThreadID;  // Thread ID (-1=caller thread).
  DWORD dwFlags;     // Reserved for future use, must be zero.
};
#pragma pack(pop)

void raise_thread_name_exception(HANDLE thread, const std::string& name) {
  if (!IsDebuggerPresent()) {
    return;
  }
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name.c_str();
  info.dwThreadID = ::GetThreadId(thread);
  info.dwFlags = 0;
  __try {
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<ULONG_PTR*>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
}

static void set_thread_name_impl(HANDLE thread, const std::string_view name) {
  auto kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto func = (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription");
    if (func) {
      // Convert UTF-8 name to UTF-16 using Windows API
      int size_needed =
          MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
      std::wstring u16name(size_needed, 0);
      MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), u16name.data(),
                          size_needed);
      func(thread, u16name.c_str());
    }
  }
  raise_thread_name_exception(thread, std::string(name));
}

namespace {
void MonitorCurrentThread(std::string_view name);
}  // namespace

void set_current_thread_name(const std::string_view name) {
  set_thread_name_impl(GetCurrentThread(), name);
  MonitorCurrentThread(name);
}

void MaybeYield() {
  SwitchToThread();
  MemoryBarrier();
}

void SyncMemory() {
  MemoryBarrier();
}

void Sleep(std::chrono::microseconds duration) {
  if (duration.count() < 100) {
    MaybeYield();
  } else {
    ::Sleep(static_cast<DWORD>(duration.count() / 1000));
  }
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  if (SleepEx(static_cast<DWORD>(duration.count() / 1000), TRUE) == WAIT_IO_COMPLETION) {
    return SleepResult::kAlerted;
  }
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  return TlsAlloc();
}

bool FreeTlsHandle(TlsHandle handle) {
  return TlsFree(handle) ? true : false;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(TlsGetValue(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return TlsSetValue(handle, reinterpret_cast<void*>(value)) ? true : false;
}

template <typename T>
class Win32Handle : public T {
 public:
  explicit Win32Handle(HANDLE handle) : handle_(handle) { assert_not_null(handle); }
  ~Win32Handle() override {
    CloseHandle(handle_);
    handle_ = nullptr;
  }

 protected:
  void* native_handle() const override { return handle_; }

  HANDLE handle_ = nullptr;
};

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle = wait_handle->native_handle();
  DWORD result = WaitForSingleObjectEx(handle, DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  HANDLE handle_to_signal = wait_handle_to_signal->native_handle();
  HANDLE handle_to_wait_on = wait_handle_to_wait_on->native_handle();
  DWORD result = SignalObjectAndWait(handle_to_signal, handle_to_wait_on, DWORD(timeout.count()),
                                     is_alertable ? TRUE : FALSE);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_IO_COMPLETION:
      return WaitResult::kUserCallback;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<HANDLE> handles(wait_handle_count);
  for (size_t i = 0; i < wait_handle_count; ++i) {
    handles[i] = wait_handles[i]->native_handle();
  }
  DWORD result =
      WaitForMultipleObjectsEx(DWORD(handles.size()), handles.data(), wait_all ? TRUE : FALSE,
                               DWORD(timeout.count()), is_alertable ? TRUE : FALSE);
  if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kSuccess, result - WAIT_OBJECT_0);
  } else if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + handles.size()) {
    return std::pair<WaitResult, size_t>(WaitResult::kAbandoned, result - WAIT_ABANDONED_0);
  }
  switch (result) {
    case WAIT_IO_COMPLETION:
      return std::pair<WaitResult, size_t>(WaitResult::kUserCallback, 0);
    case WAIT_TIMEOUT:
      return std::pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
    default:
    case WAIT_FAILED:
      return std::pair<WaitResult, size_t>(WaitResult::kFailed, 0);
  }
}

class Win32Event : public Win32Handle<Event> {
 public:
  explicit Win32Event(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Event() override = default;
  void Set() override { SetEvent(handle_); }
  void Reset() override { ResetEvent(handle_); }
#if REX_PLATFORM_UWP
  // No PulseEvent on UWP: wakes whoever is already waiting, near enough.
  void Pulse() override {
    SetEvent(handle_);
    ResetEvent(handle_);
  }
#else
  void Pulse() override { PulseEvent(handle_); }
#endif
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, TRUE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  HANDLE handle = CreateEvent(nullptr, FALSE, initial_state ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Semaphore : public Win32Handle<Semaphore> {
 public:
  explicit Win32Semaphore(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Semaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    return ReleaseSemaphore(handle_, release_count, reinterpret_cast<LPLONG>(out_previous_count))
               ? true
               : false;
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  HANDLE handle = CreateSemaphore(nullptr, initial_count, maximum_count, nullptr);
  if (handle) {
    return std::make_unique<Win32Semaphore>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Mutant : public Win32Handle<Mutant> {
 public:
  explicit Win32Mutant(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Mutant() = default;
  bool Release() override { return ReleaseMutex(handle_) ? true : false; }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  HANDLE handle = CreateMutex(nullptr, initial_owner ? TRUE : FALSE, nullptr);
  if (handle) {
    return std::make_unique<Win32Mutant>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Timer : public Win32Handle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit Win32Timer(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Timer() = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback) override {
    return SetOnceAt(WClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(GClock_::time_point due_time, std::function<void()> opt_callback) override {
    return SetOnceAt(std::chrono::clock_cast<WClock_>(due_time), std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time, std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = WClock_::to_file_time(due_time);
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : NULL;
    return SetWaitableTimer(handle_, &due_time_li, 0, completion_routine, this, FALSE) ? true
                                                                                       : false;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(WClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) {
    return SetRepeatingAt(std::chrono::clock_cast<WClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(opt_callback);
    LARGE_INTEGER due_time_li;
    due_time_li.QuadPart = WClock_::to_file_time(due_time);
    auto completion_routine =
        callback_ ? reinterpret_cast<PTIMERAPCROUTINE>(CompletionRoutine) : NULL;
    return SetWaitableTimer(handle_, &due_time_li, int32_t(period.count()), completion_routine,
                            this, FALSE)
               ? true
               : false;
  }

  bool Cancel() override {
    // Reset the callback immediately so that any completions don't call it.
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = nullptr;
    return CancelWaitableTimer(handle_) ? true : false;
  }

 private:
  static void CompletionRoutine(Win32Timer* timer, DWORD timer_low, DWORD timer_high) {
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    callback();
  }

  std::mutex mutex_;
  std::function<void()> callback_;
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, TRUE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, FALSE, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

// XERENGE_THREAD_CPU: every thread made here, with its name, and every two
// seconds the share of a core each one used - which of them a frame waits on.
namespace {
struct ThreadCpuEntry {
  HANDLE handle = nullptr;
  std::string name;
  uint64_t last = 0;
};
std::mutex g_thread_cpu_mutex;
std::unordered_map<DWORD, ThreadCpuEntry>& ThreadCpuTable() {
  static std::unordered_map<DWORD, ThreadCpuEntry> table;
  return table;
}
// Asked each time: the first threads start before env.txt is read.
bool ThreadCpuWanted() { return std::getenv("XERENGE_THREAD_CPU") != nullptr; }
uint64_t ThreadCpu100ns(HANDLE handle) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(handle, &created, &exited, &kernel, &user)) {
    return 0;
  }
  return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
         (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}
// XERENGE_PROFILE (with XERENGE_THREAD_CPU): the busiest thread of the last two
// seconds sampled about every millisecond - paused, its instruction pointer
// read, resumed - and every ten seconds the most frequent addresses logged as
// module + offset, for the build's symbols to name (llvm-symbolizer).
std::atomic<HANDLE> g_profile_target{nullptr};
std::string g_profile_target_name;
void ProfileSampler() {
  // Per thread, for the whole run: the busiest changes from screen to screen.
  std::map<std::string, std::unordered_map<uint64_t, uint32_t>> all_hits;
  std::map<std::string, uint32_t> all_samples;
  std::unordered_map<uint64_t, uint32_t> callers;  // the title code above a sample outside it
  std::unordered_map<uint64_t, uint32_t> hits;
  uint32_t samples = 0;
  ULONGLONG since = GetTickCount64();
  for (;;) {
    ::Sleep(1);
    const HANDLE target = g_profile_target.load();
    if (target && SuspendThread(target) != DWORD(-1)) {
      uint64_t sampled_rip = 0, sampled_caller = 0;
      // Only kernel calls while the thread is paused: its registers and a copy
      // of the top of its stack. Unwinding there took the function-table lock,
      // which the paused thread could hold, and the game froze.
      static uint64_t stack_copy[512];
      SIZE_T stack_got = 0;
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(target, &context)) {
        sampled_rip = context.Rip;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), stack_copy,
                          sizeof(stack_copy), &stack_got);
      }
      ResumeThread(target);
      // The title's own code (burnout.exe) above the sample: the first return
      // address into it on the stack - the guest function a wait or a helper is
      // reached from.
      if (sampled_rip) {
        static const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        static const uint64_t exe_end = [] {
          const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
          const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
          return exe + nt->OptionalHeader.SizeOfImage;
        }();
        // And the renderer's DLL: for its threads, the plume code above a wait.
        static const uint64_t plume = reinterpret_cast<uint64_t>(GetModuleHandleW(L"rexgpu-plume.dll"));
        static const uint64_t plume_end = [] {
          if (!plume) return uint64_t(0);
          const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(plume);
          const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(plume + dos->e_lfanew);
          return plume + nt->OptionalHeader.SizeOfImage;
        }();
        const auto ours = [](uint64_t a) {
          return (a >= exe && a < exe_end) || (a >= plume && a < plume_end);
        };
        if (!ours(sampled_rip)) {
          for (size_t w = 0; w < stack_got / 8; ++w) {
            if (ours(stack_copy[w])) {
              sampled_caller = stack_copy[w];
              break;
            }
          }
        }
      }
      if (sampled_rip) {
        if (sampled_caller && sampled_caller != sampled_rip) {
          ++callers[sampled_caller];
        }
        ++hits[sampled_rip];
        ++samples;
        std::string who;
        {
          std::lock_guard lock(g_thread_cpu_mutex);
          who = g_profile_target_name;
        }
        ++all_hits[who][sampled_rip];
        ++all_samples[who];
      }
    }
    if (GetTickCount64() - since < 10000) {
      continue;
    }
    since = GetTickCount64();
    // By module and offset: gathered per address, summed per module + offset.
    std::vector<std::pair<uint32_t, std::string>> top;
    for (const auto& [rip, count] : hits) {
      HMODULE module = nullptr;
      std::string where;
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(rip), &module) && module) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring name(path);
        name = name.substr(name.find_last_of(L"\\/") + 1);
        where = fmt::format("{}+0x{:X}", std::string(name.begin(), name.end()),
                            rip - reinterpret_cast<uint64_t>(module));
      } else {
        where = fmt::format("0x{:X}", rip);
      }
      top.emplace_back(count, where);
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string line;
    for (size_t i = 0; i < top.size() && i < 40; ++i) {
      line += fmt::format(" {}={:.1f}%", top[i].second, 100.0 * top[i].first / std::max(samples, 1u));
    }
    std::string name;
    {
      std::lock_guard lock(g_thread_cpu_mutex);
      name = g_profile_target_name;
    }
    REXLOG_INFO("profile of {} ({} samples):{}", name, samples, line);
    // Every address, to be named and summed per function offline.
    if (const char* dir = std::getenv("XERENGE_PROFILE_FILE")) {
      if (std::FILE* out = std::fopen(dir, "w")) {
        {
          std::fprintf(out, "## callers (title or renderer code) of samples outside them\n");
          for (const auto& [rip, count] : callers) {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(rip), &module) && module) {
              wchar_t path[MAX_PATH] = L"";
              GetModuleFileNameW(module, path, MAX_PATH);
              std::wstring file(path);
              file = file.substr(file.find_last_of(L"\\/") + 1);
              std::fprintf(out, "%u caller %ls 0x%llX\n", count, file.c_str(),
                           (unsigned long long)(rip - reinterpret_cast<uint64_t>(module)));
            }
          }
        }
        for (const auto& [who, thread_hits] : all_hits) {
          std::fprintf(out, "# %s %u samples\n", who.c_str(), all_samples[who]);
          for (const auto& [rip, count] : thread_hits) {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(rip), &module) && module) {
              wchar_t path[MAX_PATH] = L"";
              GetModuleFileNameW(module, path, MAX_PATH);
              std::wstring file(path);
              file = file.substr(file.find_last_of(L"\\/") + 1);
              std::fprintf(out, "%u %ls 0x%llX\n", count, file.c_str(),
                           (unsigned long long)(rip - reinterpret_cast<uint64_t>(module)));
            }
          }
        }
        std::fclose(out);
      }
    }
    hits.clear();
    samples = 0;
  }
}

void ThreadCpuMonitor() {
  for (;;) {
    ::Sleep(2000);
    uint64_t busiest = 0;
    std::vector<std::pair<uint64_t, std::string>> used;
    {
      std::lock_guard lock(g_thread_cpu_mutex);
      for (auto& [id, entry] : ThreadCpuTable()) {
        const uint64_t now = ThreadCpu100ns(entry.handle);
        if (now > entry.last) {
          used.emplace_back(now - entry.last, entry.name.empty() ? std::to_string(id) : entry.name);
          // XERENGE_PROFILE_THREAD=<part of a name> keeps the profiler on that
          // thread; otherwise it follows the busiest.
          static const char* const wanted = std::getenv("XERENGE_PROFILE_THREAD");
          const bool named = wanted && *wanted && entry.name.find(wanted) != std::string::npos;
          if (named || (!(wanted && *wanted) && now - entry.last > busiest)) {
            busiest = now - entry.last;
            g_profile_target.store(entry.handle);
            g_profile_target_name = entry.name;
          }
        }
        entry.last = now;
      }
    }
    std::sort(used.begin(), used.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string line;
    for (size_t i = 0; i < used.size() && i < 12; ++i) {
      // 2 s of 100 ns ticks is 20,000,000: per cent of one core.
      line += fmt::format(" {}={}%", used[i].second, used[i].first / 200000);
    }
    REXLOG_INFO("threads (share of one core over 2 s):{}", line);
  }
}
// XERENGE_FREEZE_DUMP=N: a snapshot of every thread of the process - the
// renderer's and the driver's too, found by trying each thread id - written to
// XERENGE_FREEZE_FILE (default freeze.txt here): once, when the process has
// used under 3% of a core for N seconds, or each time a file named freeze-now
// appears beside it (put there through Device Portal).
//
// Built for freezes in which a lock every thread needs is stuck - the heap's,
// the loader's, the logger's. Nothing here takes one: the buffers are made
// when the watchdog starts; a paused thread is only read by kernel calls (its
// registers, a copy of its stack); the frames are then found by scanning that
// copy for addresses inside the modules noted at start, formatted by hand into
// a fixed buffer and written with WriteFile. Not the logger, not malloc, not
// stdio, not the unwinder (whose function-table lock a paused thread may hold).
uint64_t ProcessCpu100ns() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    return 0;
  }
  return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
         (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}

struct KnownModule {
  uint64_t base = 0, end = 0;
  char name[24] = {};
};
KnownModule g_modules[24];
int g_module_count = 0;

void NoteKnownModules() {
  static const wchar_t* const names[] = {
      L"burnout.exe",   L"rexruntime.dll", L"rexgpu-plume.dll", L"ntdll.dll",
      L"KERNELBASE.dll", L"kernel32.dll",  L"ucrtbase.dll",     L"d3d12.dll",
      L"D3D12Core.dll", L"d3d12x.dll",     L"combase.dll",      L"MinUser.dll",
      L"XAudio2_9.dll", L"msvcp140.dll",   L"vcruntime140.dll", L"ws2_32.dll",
      L"dxgi.dll",      L"shcore.dll",     L"mswsock.dll"};
  for (const wchar_t* name : names) {
    HMODULE m = GetModuleHandleW(name);
    if (!m || g_module_count >= 24) {
      continue;
    }
    const auto base = reinterpret_cast<uint64_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    KnownModule& k = g_modules[g_module_count++];
    k.base = base;
    k.end = base + nt->OptionalHeader.SizeOfImage;
    for (int i = 0; name[i] && i < 23; ++i) {
      k.name[i] = char(name[i]);
    }
  }
}
const KnownModule* ModuleOf(uint64_t address) {
  for (int i = 0; i < g_module_count; ++i) {
    if (address >= g_modules[i].base && address < g_modules[i].end) {
      return &g_modules[i];
    }
  }
  return nullptr;
}

// Text into a fixed buffer, by hand.
struct Out {
  char* data = nullptr;
  size_t size = 0, used = 0;
  void Text(const char* s) {
    while (*s && used < size) data[used++] = *s++;
  }
  void Hex(uint64_t v) {
    char digits[17];
    int n = 0;
    do {
      digits[n++] = "0123456789ABCDEF"[v & 15];
      v >>= 4;
    } while (v);
    Text("0x");
    while (n && used < size) data[used++] = digits[--n];
  }
  void Dec(uint64_t v) {
    char digits[21];
    int n = 0;
    do {
      digits[n++] = char('0' + v % 10);
      v /= 10;
    } while (v);
    while (n && used < size) data[used++] = digits[--n];
  }
  void Frame(uint64_t address) {
    if (const KnownModule* m = ModuleOf(address)) {
      Text(m->name);
      Text("+");
      Hex(address - m->base);
    } else {
      Hex(address);
    }
  }
};

// The last exceptions any thread raised (first chance), for the snapshot: a
// thread that vanished leaves its cause here. Lock-free and fixed size; the
// faulting thread unwinds its own stack.
struct ExceptionNote {
  std::atomic<uint32_t> sequence{0};
  DWORD code = 0, thread = 0;
  uint64_t address = 0;
  uint64_t frames[8] = {};
};
ExceptionNote g_exception_notes[32];
std::atomic<uint32_t> g_exception_count{0};

LONG CALLBACK NoteException(PEXCEPTION_POINTERS info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == 0x406D1388) {  // a debugger's thread name
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uint32_t n = g_exception_count.fetch_add(1, std::memory_order_relaxed);
  ExceptionNote& note = g_exception_notes[n % 32];
  note.code = code;
  note.thread = GetCurrentThreadId();
  note.address = reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
  CONTEXT walk = *info->ContextRecord;
  for (int i = 0; i < 8; ++i) {
    note.frames[i] = walk.Rip;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION entry = walk.Rip ? RtlLookupFunctionEntry(walk.Rip, &image, nullptr) : nullptr;
    if (!entry) {
      break;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, entry, &walk, &handler_data, &establisher,
                     nullptr);
  }
  note.sequence.store(n + 1, std::memory_order_release);
  return EXCEPTION_CONTINUE_SEARCH;
}

constexpr size_t kMaxThreads = 160;
constexpr size_t kStackCopy = 48 * 1024;
constexpr size_t kTextBytes = 4 << 20;
struct Captured {
  DWORD id = 0;
  uint64_t rip = 0, rsp = 0;
  size_t stack_bytes = 0;
};
Captured* g_captured = nullptr;
uint8_t* g_stacks = nullptr;
char* g_text = nullptr;

// Where the watchdog is, in a file beside the snapshot (freeze-state), so a
// snapshot that does not come back says at which step it stopped. Kernel
// calls only, like the snapshot itself.
std::wstring g_mark_path;
void Mark(const char* what, uint64_t n) {
  if (g_mark_path.empty()) {
    return;
  }
  char line[96];
  Out out{line, sizeof(line) - 1, 0};
  out.Text(what);
  out.Text(" ");
  out.Dec(n);
  out.Text("\n");
  HANDLE f = CreateFile2(g_mark_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(f, line, DWORD(out.used), &written, nullptr);
    CloseHandle(f);
  }
}

void DumpAllThreads(const wchar_t* file) {
  Mark("dump: start", 0);
  const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
  size_t count = 0;
  for (DWORD id = 4; id < 0x40000 && count < kMaxThreads; id += 4) {
    if (id == self) {
      continue;
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, id);
    if (!h) {
      continue;
    }
    if (GetProcessIdOfThread(h) != process) {
      CloseHandle(h);
      continue;
    }
    Mark("dump: pausing thread", id);
    Captured& c = g_captured[count];
    c = Captured{id};
    uint8_t* stack = g_stacks + count * kStackCopy;
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(h, &context)) {
        c.rip = context.Rip;
        c.rsp = context.Rsp;
        // As much of the stack above rsp as can be read, by the kernel: a
        // fault here would be this thread's, with the other one still paused.
        for (size_t want = kStackCopy; want >= 4096; want /= 2) {
          SIZE_T got = 0;
          if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), stack,
                                want, &got) && got) {
            c.stack_bytes = got;
            break;
          }
        }
      }
      ResumeThread(h);
    }
    CloseHandle(h);
    Mark("dump: done with thread", id);
    ++count;
  }

  Out out{g_text, kTextBytes, 0};
  out.Text("freeze: ");
  out.Dec(count);
  out.Text(" threads (frames: rip, then return addresses found on the stack)\n");
  for (size_t t = 0; t < count; ++t) {
    const Captured& c = g_captured[t];
    out.Text("\nthread ");
    out.Dec(c.id);
    out.Text("\n  ");
    out.Frame(c.rip);
    out.Text("\n");
    const uint64_t* words = reinterpret_cast<const uint64_t*>(g_stacks + t * kStackCopy);
    int shown = 0;
    for (size_t w = 0; w < c.stack_bytes / 8 && shown < 48; ++w) {
      const uint64_t v = words[w];
      if (const KnownModule* m = ModuleOf(v)) {
        // Code, not data: an address just after a call is what is wanted, and
        // the text sections are what the modules' ranges mostly are.
        (void)m;
        out.Text("  ");
        out.Frame(v);
        out.Text("\n");
        ++shown;
      }
    }
  }
  const uint32_t raised = g_exception_count.load(std::memory_order_acquire);
  out.Text("\nexceptions raised: ");
  out.Dec(raised);
  out.Text(", the last of them:\n");
  for (uint32_t n = raised > 32 ? raised - 32 : 0; n < raised; ++n) {
    const ExceptionNote& note = g_exception_notes[n % 32];
    if (note.sequence.load(std::memory_order_acquire) != n + 1) {
      continue;
    }
    out.Text("#");
    out.Dec(n);
    out.Text(" code ");
    out.Hex(note.code);
    out.Text(" thread ");
    out.Dec(note.thread);
    out.Text(" at ");
    out.Frame(note.address);
    out.Text("\n");
    for (uint64_t f : note.frames) {
      if (f) {
        out.Text("    ");
        out.Frame(f);
        out.Text("\n");
      }
    }
  }
  Mark("dump: writing", out.used);
  HANDLE f = CreateFile2(file, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(f, out.data, DWORD(out.used), &written, nullptr);
    CloseHandle(f);
  }
  Mark("dump: written", out.used);
}

void FreezeWatchdog(uint32_t seconds, std::wstring file) {
  NoteKnownModules();
  g_captured = new Captured[kMaxThreads];
  g_stacks = new uint8_t[kMaxThreads * kStackCopy];
  g_text = new char[kTextBytes];
  const std::wstring trigger = file.substr(0, file.find_last_of(L"\\/") + 1) + L"freeze-now";
  g_mark_path = file.substr(0, file.find_last_of(L"\\/") + 1) + L"freeze-state";
  uint64_t history[64] = {};
  uint32_t ticks = 0;
  bool dumped_idle = false;
  for (;;) {
    ::Sleep(1000);
    if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
      DeleteFileW(trigger.c_str());
      DumpAllThreads(file.c_str());
      continue;
    }
    history[ticks % 64] = ProcessCpu100ns();
    ++ticks;
    if (ticks % 10 == 0) {
      Mark("watchdog: alive, tick", ticks);
    }
    if (dumped_idle || ticks <= seconds || seconds >= 64) {
      continue;
    }
    const uint64_t used = history[(ticks - 1) % 64] - history[(ticks - 1 - seconds) % 64];
    // 3% of one core over the window, in 100 ns ticks.
    if (used < uint64_t(seconds) * 300000) {
      Mark("watchdog: idle, dumping", ticks);
      DumpAllThreads(file.c_str());
      dumped_idle = true;
    }
  }
}

void StartFreezeWatchdog() {
  // Asked at every thread's start until it is set: the first threads start
  // before env.txt is read.
  static std::atomic<bool> started{false};
  const char* text = std::getenv("XERENGE_FREEZE_DUMP");
  if (started.load() || !text || !*text || started.exchange(true)) {
    return;
  }
  {
    const uint32_t seconds = std::max(2u, uint32_t(std::strtoul(text, nullptr, 10)));
    AddVectoredExceptionHandler(1, NoteException);
    const char* file = std::getenv("XERENGE_FREEZE_FILE");
    const std::string narrow = file && *file ? file : "freeze.txt";
    std::thread(FreezeWatchdog, seconds, std::wstring(narrow.begin(), narrow.end())).detach();
  }
}
void ThreadCpuRegister() {
  StartFreezeWatchdog();
  if (!ThreadCpuWanted()) {
    return;
  }
  HANDLE real = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, 0, FALSE,
                  DUPLICATE_SAME_ACCESS);
  {
    std::lock_guard lock(g_thread_cpu_mutex);
    ThreadCpuTable()[GetCurrentThreadId()].handle = real;
  }
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread(ThreadCpuMonitor).detach();
    if (std::getenv("XERENGE_PROFILE") != nullptr) {
      std::thread(ProfileSampler).detach();
    }
  });
}
void ThreadCpuName(DWORD id, const std::string& name) {
  std::lock_guard lock(g_thread_cpu_mutex);
  auto it = ThreadCpuTable().find(id);
  if (it != ThreadCpuTable().end()) {
    it->second.name = name;
  }
}
// A thread made elsewhere (std::thread: the renderer's workers) that names
// itself is counted and can be profiled too.
void MonitorCurrentThread(std::string_view name) {
  if (!ThreadCpuWanted()) {
    return;
  }
  bool known;
  {
    std::lock_guard lock(g_thread_cpu_mutex);
    known = ThreadCpuTable().count(GetCurrentThreadId()) != 0;
  }
  if (!known) {
    ThreadCpuRegister();
  }
  ThreadCpuName(GetCurrentThreadId(), std::string(name));
}
}  // namespace

class Win32Thread : public Win32Handle<Thread> {
 public:
  explicit Win32Thread(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Thread() = default;

  void set_name(std::string name) override {
    set_thread_name_impl(handle_, name);
    ThreadCpuName(GetThreadId(handle_), name);
    Thread::set_name(name);
  }

  int32_t priority() override { return GetThreadPriority(handle_); }
  uint32_t system_id() const override { return GetThreadId(handle_); }

  void set_priority(int32_t new_priority) override { SetThreadPriority(handle_, new_priority); }

  uint64_t affinity_mask() override {
    DWORD_PTR prev = SetThreadAffinityMask(handle_, 1);
    if (prev) {
      SetThreadAffinityMask(handle_, prev);
    }
    return static_cast<uint64_t>(prev);
  }

  void set_affinity_mask(uint64_t new_affinity_mask) override {
    SetThreadAffinityMask(handle_, new_affinity_mask);
  }

  struct ApcData {
    std::function<void()> callback;
  };
  static void NTAPI DispatchApc(ULONG_PTR parameter) {
    auto apc_data = reinterpret_cast<ApcData*>(parameter);
    apc_data->callback();
    delete apc_data;
  }

  void QueueUserCallback(std::function<void()> callback) override {
    auto apc_data = new ApcData({std::move(callback)});
    QueueUserAPC(DispatchApc, handle_, reinterpret_cast<ULONG_PTR>(apc_data));
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = ResumeThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) override {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    DWORD result = SuspendThread(handle_);
    if (result == UINT_MAX) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = result;
    }
    return true;
  }

  void Terminate(int exit_code) override { TerminateThread(handle_, exit_code); }

 private:
  void AssertCallingThread() { assert_true(GetCurrentThreadId() == GetThreadId(handle_)); }
};

thread_local std::unique_ptr<Win32Thread> current_thread_ = nullptr;

struct ThreadStartData {
  std::function<void()> start_routine;
};
DWORD WINAPI ThreadStartRoutine(LPVOID parameter) {
  ThreadCpuRegister();
  current_thread_ = std::make_unique<Win32Thread>(::GetCurrentThread());

  auto start_data = reinterpret_cast<ThreadStartData*>(parameter);
  start_data->start_routine();
  delete start_data;
  return 0;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto start_data = new ThreadStartData({std::move(start_routine)});
  HANDLE handle = CreateThread(NULL, params.stack_size, ThreadStartRoutine, start_data,
                               params.create_suspended ? CREATE_SUSPENDED : 0, NULL);
  if (handle) {
    return std::make_unique<Win32Thread>(handle);
  } else {
    LOG_LASTERROR();
    delete start_data;
    return nullptr;
  }
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_.get();
  }

  HANDLE handle = ::GetCurrentThread();
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }

  current_thread_ = std::make_unique<Win32Thread>(handle);
  return current_thread_.get();
}

void Thread::Exit(int exit_code) {
  ExitThread(exit_code);
}

}  // namespace rex::thread
