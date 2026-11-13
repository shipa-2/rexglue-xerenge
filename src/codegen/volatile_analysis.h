/**
 * @file        codegen/volatile_analysis.h
 * @brief       Which volatile registers a function keeps in locals, and what it
 *              hands to and takes from PPCContext around calls and returns
 *
 * volatile_as_local keeps r3-r10, f1-f13 and v0-v13 in local variables, as the
 * other *_as_local options do for the registers no call passes. These ones are
 * passed: a callee takes its arguments from PPCContext and leaves its results
 * there. So a function loads at its entry the registers it may read before
 * writing, writes to PPCContext before a call the ones the callee may read, and
 * reads the results back after it. What "may read" is comes from a liveness
 * analysis over the emitted C++ itself - the text of an instruction says which
 * registers it mentions, where it jumps, what it calls - carried across the
 * call graph to a fixed point.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rex::codegen {

class FunctionNode;
struct EmitContext;

/// One bit a localized volatile register: r3-r10 (0-7), f1-f13 (8-20), v0-v13 (21-34).
using VolatileMask = uint64_t;

namespace volatile_regs {
constexpr int kGprFirst = 3, kGprLast = 10, kGprBit = 0;
constexpr int kFprFirst = 1, kFprLast = 13, kFprBit = 8;
constexpr int kVrFirst = 0, kVrLast = 13, kVrBit = 21;
constexpr int kBits = 35;

constexpr VolatileMask Gpr(int n) {
  return VolatileMask{1} << (kGprBit + n - kGprFirst);
}
constexpr VolatileMask Fpr(int n) {
  return VolatileMask{1} << (kFprBit + n - kFprFirst);
}
constexpr VolatileMask Vr(int n) {
  return VolatileMask{1} << (kVrBit + n - kVrFirst);
}
constexpr bool IsGpr(size_t n) {
  return n >= kGprFirst && n <= kGprLast;
}
constexpr bool IsFpr(size_t n) {
  return n >= kFprFirst && n <= kFprLast;
}
constexpr bool IsVr(size_t n) {
  return n <= kVrLast;
}

/// The registers a call's results come back in.
constexpr VolatileMask kReturns = Gpr(3) | Gpr(4) | Fpr(1) | Vr(1);
/// Every register an argument can be passed in (v0 is scratch, never one).
constexpr VolatileMask kArguments = ((VolatileMask{1} << kBits) - 1) & ~Vr(0);
/// What ppc_trap reads.
constexpr VolatileMask kTrapReads = Gpr(3) | Gpr(4);

/// "r3", "f1", "v0" ... for a bit index.
std::string Name(int bit);
}  // namespace volatile_regs

/// What a line of an instruction's text is, as far as registers moving between
/// locals and PPCContext go.
enum class VolatileLineKind : uint8_t {
  Plain,
  Comment,
  Goto,
  Return,
  Call,          ///< NAME(ctx, base);
  IndirectCall,  ///< REX_CALL_INDIRECT_FUNC(...);
  Trap,          ///< ppc_trap(ctx, base, N);
  Unsupported,   ///< uses ctx in a way the analysis does not follow
};

struct VolatileLine {
  VolatileLineKind kind = VolatileLineKind::Plain;
  size_t tabs = 0;            ///< leading tabs
  bool guarded = false;       ///< "if (COND) <statement>" on one line
  std::string_view condition; ///< COND when guarded
  std::string_view statement; ///< the statement without the guard and the tabs
  std::string_view callee;    ///< Call: the function's name
  uint32_t target = 0;        ///< Goto: the label's address
};

VolatileLine ClassifyVolatileLine(std::string_view line);

/// What the emitter adds at one line of an instruction.
struct VolatileAction {
  uint32_t line = 0;  ///< index of the line within the instruction's text
  VolatileLineKind kind = VolatileLineKind::Plain;
  VolatileMask store = 0;  ///< written to PPCContext before the line
  VolatileMask load = 0;   ///< read back from PPCContext after it
};

struct VolatileFunctionInfo {
  /// False: the function keeps the volatile registers in PPCContext.
  bool enabled = false;
  VolatileMask referenced = 0;  ///< the registers the function has locals for
  VolatileMask entryLoad = 0;   ///< read from PPCContext at entry
  /// The body runs off its end without a return statement: results written there.
  bool fallsOffEnd = false;
  /// By the index of the instruction in emission order.
  std::unordered_map<uint32_t, std::vector<VolatileAction>> actions;
};

/// The text of each instruction of one function, in emission order.
struct InsnTextSink {
  struct Entry {
    uint32_t address = 0;
    bool labelled = false;  ///< a loc_ label was emitted in front of it
    std::string text;
  };
  std::vector<Entry> insns;
};

struct VolatileAnalysis {
  std::unordered_map<const FunctionNode*, VolatileFunctionInfo> functions;

  const VolatileFunctionInfo* find(const FunctionNode* fn) const {
    auto it = functions.find(fn);
    return it != functions.end() && it->second.enabled ? &it->second : nullptr;
  }
};

/// Emits every function once without the option, reads the text and solves.
VolatileAnalysis AnalyzeVolatiles(const std::vector<const FunctionNode*>& functions,
                                  const EmitContext& emitCtx);

/// Rewrites one instruction's text (from @p start in @p body) with its actions.
/// Throws std::runtime_error if a line is not what the analysis saw.
void ApplyVolatileActions(std::string& body, size_t start,
                          const std::vector<VolatileAction>& actions);

/// The statements writing @p mask back to PPCContext / reading it, one a line.
std::string VolatileStores(VolatileMask mask, size_t tabs);
std::string VolatileLoads(VolatileMask mask, size_t tabs);

}  // namespace rex::codegen
