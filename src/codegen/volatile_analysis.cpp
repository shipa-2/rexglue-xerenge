/**
 * @file        codegen/volatile_analysis.cpp
 * @brief       Liveness of the volatile registers over the emitted C++
 *
 * See volatile_analysis.h. The analysis reads the text the builders emit rather
 * than a table of what each opcode reads and writes: the text is what the C++
 * compiler will run, so whatever a builder does is seen as it is, and a builder
 * added or changed later is followed without a table to keep in step.
 *
 * What is taken from the text, an instruction at a time:
 *  - which localizable registers a statement mentions. A register whose first
 *    mention in a run of statements is a whole assignment to it (rN.u64 = ...,
 *    a 16-byte store into vN) that does not read it is killed there; any other
 *    first mention counts as a read. Counting a write as a read only makes a
 *    register live for longer, which costs a store and never an error.
 *  - goto, return, calls (NAME(ctx, base);, REX_CALL_INDIRECT_FUNC) and
 *    ppc_trap, each unconditional only when it stands alone at the body's
 *    first level.
 *
 * A function's summary is the registers live at its entry: args, with nothing
 * live at its returns - what it or its callees may read - and pass, the result
 * registers that may come out of it as they went in (a function that never
 * touches r3 returns its caller's r3). A call is then
 *     before = (after - results) + args + (pass & after)
 * and the summaries grow to a fixed point over the call graph. Callees the
 * analysis cannot see into - indirect calls, imports, functions a hook
 * replaces, functions kept on PPCContext - read every argument register.
 *
 * In a function: R is read from PPCContext at entry if live there; written
 * back before a call if the callee may read it (as above) and it may have been
 * mentioned since the last call - a register no statement has mentioned since
 * a call still holds what that callee left in PPCContext, which is the newer
 * value; read back after a call if it is a result register live there; and the
 * result registers are written back at every return.
 */

#include "volatile_analysis.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <stdexcept>

#include <fmt/format.h>

#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_node.h>

#include "codegen_logging.h"

namespace rex::codegen {

namespace volatile_regs {

std::string Name(int bit) {
  if (bit < kFprBit)
    return fmt::format("r{}", bit - kGprBit + kGprFirst);
  if (bit < kVrBit)
    return fmt::format("f{}", bit - kFprBit + kFprFirst);
  return fmt::format("v{}", bit - kVrBit + kVrFirst);
}

}  // namespace volatile_regs

namespace {

using namespace volatile_regs;

bool IsIdent(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string_view TrimRight(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

std::string_view TrimLeft(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  return s;
}

/// A register token at @p pos ("r12", "f3", "v0"): its class letter, number and
/// the position after it; false if there is none.
bool ParseRegToken(std::string_view s, size_t pos, char& cls, int& number, size_t& end) {
  if (pos >= s.size())
    return false;
  char c = s[pos];
  if (c != 'r' && c != 'f' && c != 'v')
    return false;
  size_t j = pos + 1;
  int n = 0;
  size_t digits = 0;
  while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && digits < 4) {
    n = n * 10 + (s[j] - '0');
    ++j;
    ++digits;
  }
  if (digits == 0)
    return false;
  if (j < s.size() && IsIdent(s[j]))
    return false;
  cls = c;
  number = n;
  end = j;
  return true;
}

int BitOf(char cls, int n) {
  if (cls == 'r' && n >= kGprFirst && n <= kGprLast)
    return kGprBit + n - kGprFirst;
  if (cls == 'f' && n >= kFprFirst && n <= kFprLast)
    return kFprBit + n - kFprFirst;
  if (cls == 'v' && n >= kVrFirst && n <= kVrLast)
    return kVrBit + n - kVrFirst;
  return -1;
}

/// Calls fn(bit) for every mention of a localizable volatile register in @p s,
/// as "rN" or "ctx.rN". A token after any other '.' is a member (temp.f32).
template <class F>
void ForEachMention(std::string_view s, F&& fn) {
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c != 'r' && c != 'f' && c != 'v')
      continue;
    if (i > 0) {
      char p = s[i - 1];
      if (IsIdent(p))
        continue;
      if (p == '.') {
        bool isCtx = i >= 4 && s.substr(i - 4, 4) == "ctx." &&
                     (i == 4 || (!IsIdent(s[i - 5]) && s[i - 5] != '.'));
        if (!isCtx)
          continue;
      }
    }
    char cls;
    int number;
    size_t end;
    if (!ParseRegToken(s, i, cls, number, end))
      continue;
    int bit = BitOf(cls, number);
    if (bit >= 0)
      fn(bit);
    i = end - 1;
  }
}

VolatileMask MentionsOf(std::string_view s) {
  VolatileMask mask = 0;
  ForEachMention(s, [&](int bit) { mask |= VolatileMask{1} << bit; });
  return mask;
}

bool ContainsWord(std::string_view s, std::string_view word) {
  size_t pos = 0;
  while ((pos = s.find(word, pos)) != std::string_view::npos) {
    bool left = pos == 0 || !IsIdent(s[pos - 1]);
    bool right = pos + word.size() >= s.size() || !IsIdent(s[pos + word.size()]);
    if (left && right)
      return true;
    pos += word.size();
  }
  return false;
}

/// "ctx" used whole (passed on, copied) rather than as "ctx.member".
bool UsesWholeContext(std::string_view s) {
  size_t pos = 0;
  while ((pos = s.find("ctx", pos)) != std::string_view::npos) {
    bool left = pos == 0 || !IsIdent(s[pos - 1]);
    size_t after = pos + 3;
    bool right = after >= s.size() || !IsIdent(s[after]);
    if (left && right && (after >= s.size() || s[after] != '.'))
      return true;
    pos = after;
  }
  return false;
}

/// The register a statement at the body's first level assigns whole without
/// reading it, as a bit, or -1.
int KillTarget(std::string_view stmt) {
  size_t pos = 0;
  std::string_view rhs;
  char cls = 0;
  int number = 0;
  size_t end = 0;

  static constexpr std::string_view kStoreInt = "simde_mm_store_si128((simde__m128i*)";
  static constexpr std::string_view kStorePs = "simde_mm_store_ps(";

  if (stmt.starts_with(kStoreInt) || stmt.starts_with(kStorePs)) {
    pos = stmt.starts_with(kStoreInt) ? kStoreInt.size() : kStorePs.size();
    if (stmt.substr(pos).starts_with("ctx."))
      pos += 4;
    if (!ParseRegToken(stmt, pos, cls, number, end) || cls != 'v')
      return -1;
    if (end >= stmt.size() || stmt[end] != '.')
      return -1;
    size_t field = end + 1;
    while (field < stmt.size() && (std::islower(static_cast<unsigned char>(stmt[field])) ||
                                   std::isdigit(static_cast<unsigned char>(stmt[field]))))
      ++field;
    if (!stmt.substr(field).starts_with(", "))
      return -1;
    rhs = stmt.substr(field + 2);
  } else {
    if (stmt.starts_with("ctx."))
      pos = 4;
    if (!ParseRegToken(stmt, pos, cls, number, end))
      return -1;
    std::string_view rest = stmt.substr(end);
    if (rest.starts_with(" = ")) {
      rhs = rest.substr(3);
    } else if (cls != 'v' && (rest.starts_with(".u64 = ") || rest.starts_with(".s64 = ") ||
                              rest.starts_with(".f64 = "))) {
      rhs = rest.substr(7);
    } else {
      return -1;
    }
  }

  if (rhs.empty() || rhs.back() != ';')
    return -1;
  int bit = BitOf(cls, number);
  if (bit < 0)
    return -1;
  if (MentionsOf(rhs) & (VolatileMask{1} << bit))
    return -1;
  return bit;
}

std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) {
      lines.push_back(text.substr(pos));
      break;
    }
    lines.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return lines;
}

int BraceDelta(std::string_view s) {
  int delta = 0;
  bool inString = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"' && (i == 0 || s[i - 1] != '\\'))
      inString = !inString;
    if (inString)
      continue;
    if (c == '{')
      ++delta;
    else if (c == '}')
      --delta;
  }
  return delta;
}

//=============================================================================
// A function as the analysis sees it
//=============================================================================

struct Event {
  enum Type : uint8_t { Mentions, Goto, Return, Call, Trap };
  Type type = Mentions;
  bool cond = false;
  uint32_t line = 0;
  VolatileMask gen = 0;   // Mentions: read first
  VolatileMask kill = 0;  // Mentions: assigned whole first
  VolatileMask writes = 0;  // Mentions: assigned whole by some statement, guarded or not
  int32_t target = -1;    // Goto: instruction index. Call: callee's function index, -1 unknown
  VolatileLineKind kind = VolatileLineKind::Plain;
};

struct Insn {
  uint32_t evBegin = 0;
  uint32_t evEnd = 0;
  bool fallthrough = true;
};

struct Fn {
  const FunctionNode* node = nullptr;
  bool enabled = false;
  /// Its callers cannot rely on the summary: something else may stand in its place.
  bool opaque = false;
  std::vector<Insn> insns;
  std::vector<Event> evs;
  VolatileMask referenced = 0;
  // Live at entry with nothing live at the returns, and with the results live.
  VolatileMask liveArgs = 0;
  VolatileMask liveResults = 0;
  // The registers a call of it may leave changed: the ones its statements
  // assign whole, and its callees'. Only what is certain counts - a caller
  // stops trusting its own copy of these after the call, and the title's
  // compiler knew its callees: values are left in r5, r6... across a call to a
  // function that does not touch them, for the call after it.
  VolatileMask ownWrites = 0;
  VolatileMask mayDefine = 0;
  bool isImport = false;
  std::vector<uint32_t> callers;
};

struct Summary {
  VolatileMask args;
  VolatileMask pass;
  VolatileMask defines;
};

constexpr VolatileMask kAllRegs = (VolatileMask{1} << kBits) - 1;
/// A Call event's target for an indirect call or an import: the ABI's callee.
constexpr int32_t kAbiCallee = -1;
/// A direct call to a function the analysis has no text for.
constexpr int32_t kUnreadCallee = -2;

Summary SummaryOf(const std::vector<Fn>& all, int32_t callee) {
  if (callee == kAbiCallee)
    return {kArguments, kReturns, kAllRegs};
  // Unread (kept on PPCContext, or replaced by the runtime): it may read any
  // argument, and nothing is known to be overwritten.
  if (callee < 0 || !all[callee].enabled)
    return {kArguments, kReturns, 0};
  const Fn& g = all[callee];
  // A hook may stand in its place and read the arguments as the ABI passes them.
  if (g.opaque)
    return {kArguments, kReturns, g.mayDefine};
  return {g.liveArgs, g.liveResults & ~g.liveArgs & kReturns, g.mayDefine};
}

// A register the callee may overwrite and that is no result is dead across the
// call: nothing reads what a callee happens to leave in r5.
VolatileMask CallBefore(VolatileMask after, const Summary& s) {
  return (after & ~kReturns & ~s.defines) | s.args | (s.pass & after);
}

/// Backward liveness; returns what is live at the function's entry. @p afterCall,
/// if given, receives per event what is live after each call.
VolatileMask SolveLive(const Fn& f, VolatileMask seed, const std::vector<Fn>& all,
                       std::vector<VolatileMask>& in, std::vector<VolatileMask>* afterCall) {
  const size_t n = f.insns.size();
  in.assign(n, 0);
  if (afterCall)
    afterCall->assign(f.evs.size(), 0);
  if (n == 0)
    return seed;

  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = n; i-- > 0;) {
      const Insn& insn = f.insns[i];
      VolatileMask live = 0;
      if (insn.fallthrough)
        live = i + 1 < n ? in[i + 1] : seed;
      for (uint32_t e = insn.evEnd; e-- > insn.evBegin;) {
        const Event& ev = f.evs[e];
        switch (ev.type) {
          case Event::Mentions:
            live = (live & ~ev.kill) | ev.gen;
            break;
          case Event::Goto: {
            VolatileMask t = in[ev.target];
            live = ev.cond ? (live | t) : t;
            break;
          }
          case Event::Return:
            live = ev.cond ? (live | seed) : seed;
            break;
          case Event::Call: {
            VolatileMask after = live;
            if (afterCall)
              (*afterCall)[e] = after;
            VolatileMask before = CallBefore(after, SummaryOf(all, ev.target));
            live = ev.cond ? (after | before) : before;
            break;
          }
          case Event::Trap:
            live |= kTrapReads;
            break;
        }
      }
      if (live != in[i]) {
        in[i] = live;
        changed = true;
      }
    }
  }
  return in[0];
}

std::string EmittedName(const FunctionNode* fn, const EmitContext& ctx) {
  if (fn->base() == ctx.entryPoint)
    return "xstart";
  if (!fn->name().empty())
    return fn->name();
  return fmt::format("sub_{:08X}", fn->base());
}

bool IsSaveRestoreHelperName(std::string_view name) {
  return name.starts_with("__save") || name.starts_with("__rest");
}

/// Reads one function's instruction texts into events. False (with the reason
/// logged) if the text holds something the analysis does not follow.
bool ParseFunction(Fn& f, const InsnTextSink& sink, std::vector<std::string>& calleeNames,
                   std::string_view name) {
  // Where each label stands. A label is emitted once, at the first instruction
  // emitted for its address.
  std::unordered_map<uint32_t, uint32_t> labelIndex;
  for (uint32_t i = 0; i < sink.insns.size(); ++i) {
    if (sink.insns[i].labelled)
      labelIndex.emplace(sink.insns[i].address, i);
  }

  f.insns.resize(sink.insns.size());
  for (uint32_t i = 0; i < sink.insns.size(); ++i) {
    Insn& insn = f.insns[i];
    insn.evBegin = static_cast<uint32_t>(f.evs.size());

    int depth = 0;
    bool open = false;      // a Mentions event is being filled
    VolatileMask seen = 0;  // mentioned in the open event
    bool endsHere = false;  // the last statement was an unconditional goto/return

    auto lines = SplitLines(sink.insns[i].text);
    for (uint32_t li = 0; li < lines.size(); ++li) {
      VolatileLine line = ClassifyVolatileLine(lines[li]);
      if (line.kind == VolatileLineKind::Comment)
        continue;
      std::string_view code = TrimRight(TrimLeft(lines[li]));
      if (size_t c = code.find("//"); c != std::string_view::npos)
        code = TrimRight(code.substr(0, c));
      if (code.empty())
        continue;

      if (line.kind == VolatileLineKind::Unsupported) {
        REXCODEGEN_WARN("volatile_as_local: {} keeps its registers in PPCContext: \"{}\"", name,
                        code);
        return false;
      }

      const bool cond = line.tabs > 1 || depth > 0 || line.guarded;

      int writeBit = line.kind == VolatileLineKind::Plain ? KillTarget(line.statement) : -1;
      int killBit = cond ? -1 : writeBit;
      if (writeBit >= 0)
        f.ownWrites |= VolatileMask{1} << writeBit;

      VolatileMask mentioned = MentionsOf(code);
      if (mentioned) {
        if (!open) {
          f.evs.push_back(Event{});
          open = true;
          seen = 0;
        }
        Event& m = f.evs.back();
        VolatileMask fresh = mentioned & ~seen;
        if (killBit >= 0 && (fresh & (VolatileMask{1} << killBit))) {
          m.kill |= VolatileMask{1} << killBit;
          fresh &= ~(VolatileMask{1} << killBit);
        }
        m.gen |= fresh;
        seen |= mentioned;
        f.referenced |= mentioned;
      }

      endsHere = false;
      if (line.kind != VolatileLineKind::Plain) {
        Event ev;
        ev.cond = cond;
        ev.line = li;
        ev.kind = line.kind;
        switch (line.kind) {
          case VolatileLineKind::Goto: {
            auto it = labelIndex.find(line.target);
            if (it == labelIndex.end()) {
              REXCODEGEN_WARN("volatile_as_local: {} jumps to loc_{:X}, which it does not hold",
                              name, line.target);
              return false;
            }
            ev.type = Event::Goto;
            ev.target = static_cast<int32_t>(it->second);
            endsHere = !cond;
            break;
          }
          case VolatileLineKind::Return:
            ev.type = Event::Return;
            endsHere = !cond;
            break;
          case VolatileLineKind::Call:
            ev.type = Event::Call;
            ev.target = static_cast<int32_t>(calleeNames.size());
            calleeNames.emplace_back(line.callee);
            break;
          case VolatileLineKind::IndirectCall:
            ev.type = Event::Call;
            ev.target = kAbiCallee;
            break;
          case VolatileLineKind::Trap:
            ev.type = Event::Trap;
            break;
          default:
            break;
        }
        f.evs.push_back(ev);
        open = false;
      }

      depth += BraceDelta(code);
    }

    insn.evEnd = static_cast<uint32_t>(f.evs.size());
    insn.fallthrough = !endsHere;
  }
  return true;
}

}  // namespace

//=============================================================================
// Lines
//=============================================================================

VolatileLine ClassifyVolatileLine(std::string_view raw) {
  VolatileLine line;
  while (line.tabs < raw.size() && raw[line.tabs] == '\t')
    ++line.tabs;
  std::string_view rest = TrimRight(raw.substr(line.tabs));
  if (rest.starts_with("//")) {
    line.kind = VolatileLineKind::Comment;
    return line;
  }
  if (size_t c = rest.find("//"); c != std::string_view::npos)
    rest = TrimRight(rest.substr(0, c));

  std::string_view stmt = rest;
  if (rest.starts_with("if (")) {
    int depth = 0;
    size_t close = std::string_view::npos;
    for (size_t i = 3; i < rest.size(); ++i) {
      if (rest[i] == '(')
        ++depth;
      else if (rest[i] == ')' && --depth == 0) {
        close = i;
        break;
      }
    }
    if (close != std::string_view::npos) {
      std::string_view tail = TrimLeft(rest.substr(close + 1));
      if (!tail.empty() && tail.front() != '{') {
        line.guarded = true;
        line.condition = rest.substr(4, close - 4);
        stmt = tail;
      }
    }
  }
  line.statement = stmt;

  static constexpr std::string_view kCallTail = "(ctx, base);";
  if (stmt == "return;") {
    line.kind = VolatileLineKind::Return;
  } else if (stmt.starts_with("goto loc_") && stmt.ends_with(";")) {
    line.kind = VolatileLineKind::Goto;
    uint32_t target = 0;
    for (char c : stmt.substr(9, stmt.size() - 10)) {
      int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                  : (c >= 'A' && c <= 'F')                    ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f')                    ? c - 'a' + 10
                                                              : -1;
      if (digit < 0) {
        line.kind = VolatileLineKind::Unsupported;
        return line;
      }
      target = target * 16 + static_cast<uint32_t>(digit);
    }
    line.target = target;
  } else if (stmt.starts_with("REX_CALL_INDIRECT_FUNC(") &&
             (stmt.ends_with(");") || stmt.ends_with("); }"))) {
    line.kind = VolatileLineKind::IndirectCall;
  } else if (stmt.starts_with("ppc_trap(ctx, base,") && stmt.ends_with(");")) {
    line.kind = VolatileLineKind::Trap;
  } else if (stmt.ends_with(kCallTail) && stmt.size() > kCallTail.size() &&
             std::all_of(stmt.begin(), stmt.end() - kCallTail.size(), IsIdent)) {
    line.kind = VolatileLineKind::Call;
    line.callee = stmt.substr(0, stmt.size() - kCallTail.size());
  } else if (ContainsWord(rest, "goto") || ContainsWord(rest, "return") ||
             UsesWholeContext(rest) || ContainsWord(rest, "env") ||
             rest.find("REX_CALL_INDIRECT_FUNC") != std::string_view::npos ||
             rest.find("ppc_setjmp") != std::string_view::npos ||
             rest.find("ppc_longjmp") != std::string_view::npos) {
    line.kind = VolatileLineKind::Unsupported;
  }

  // A guarded call is not a shape the emitter rewrites.
  if (line.guarded &&
      (line.kind == VolatileLineKind::Call || line.kind == VolatileLineKind::IndirectCall))
    line.kind = VolatileLineKind::Unsupported;
  return line;
}

//=============================================================================
// Emission
//=============================================================================

std::string VolatileStores(VolatileMask mask, size_t tabs) {
  std::string out;
  for (int bit = 0; bit < kBits; ++bit) {
    if (mask & (VolatileMask{1} << bit)) {
      out.append(tabs, '\t');
      auto name = Name(bit);
      out += fmt::format("ctx.{} = {};\n", name, name);
    }
  }
  return out;
}

std::string VolatileLoads(VolatileMask mask, size_t tabs) {
  std::string out;
  for (int bit = 0; bit < kBits; ++bit) {
    if (mask & (VolatileMask{1} << bit)) {
      out.append(tabs, '\t');
      auto name = Name(bit);
      out += fmt::format("{} = ctx.{};\n", name, name);
    }
  }
  return out;
}

void ApplyVolatileActions(std::string& body, size_t start,
                          const std::vector<VolatileAction>& actions) {
  std::string text = body.substr(start);
  auto lines = SplitLines(text);
  std::string out;
  out.reserve(text.size() + actions.size() * 64);

  size_t next = 0;
  for (uint32_t li = 0; li < lines.size(); ++li) {
    std::string_view raw = lines[li];
    if (next >= actions.size() || actions[next].line != li) {
      out += raw;
      out += '\n';
      continue;
    }
    const VolatileAction& action = actions[next++];
    VolatileLine line = ClassifyVolatileLine(raw);
    if (line.kind != action.kind) {
      throw std::runtime_error(
          fmt::format("volatile_as_local: line \"{}\" is not what the analysis read", raw));
    }

    const std::string tabs(line.tabs, '\t');
    switch (line.kind) {
      case VolatileLineKind::Call:
      case VolatileLineKind::IndirectCall:
        out += VolatileStores(action.store, line.tabs);
        out += raw;
        out += '\n';
        out += VolatileLoads(action.load, line.tabs);
        break;
      case VolatileLineKind::Trap:
      case VolatileLineKind::Return:
        if (!line.guarded) {
          out += VolatileStores(action.store, line.tabs);
          out += raw;
          out += '\n';
        } else if (action.store == 0) {
          out += raw;
          out += '\n';
        } else {
          out += fmt::format("{}if ({}) {{\n", tabs, line.condition);
          out += VolatileStores(action.store, line.tabs + 1);
          out += fmt::format("{}\t{}\n", tabs, line.statement);
          out += fmt::format("{}}}\n", tabs);
        }
        break;
      default:
        throw std::runtime_error(
            fmt::format("volatile_as_local: nothing to do at line \"{}\"", raw));
    }
  }
  if (next != actions.size())
    throw std::runtime_error("volatile_as_local: an instruction lost a line between passes");

  body.resize(start);
  body += out;
}

//=============================================================================
// The analysis
//=============================================================================

VolatileAnalysis AnalyzeVolatiles(const std::vector<const FunctionNode*>& functions,
                                  const EmitContext& emitCtx) {
  VolatileAnalysis result;
  const auto& cfg = emitCtx.config;

  std::vector<Fn> all(functions.size());
  std::unordered_map<std::string, int32_t> byName;
  // The callee names of every function's Call events, resolved once all are read.
  std::vector<std::vector<std::string>> calleeNames(functions.size());

  EmitContext passCtx = emitCtx;
  passCtx.referenced = nullptr;
  passCtx.volatiles = nullptr;

  size_t kept = 0;
  for (size_t i = 0; i < functions.size(); ++i) {
    const FunctionNode* node = functions[i];
    Fn& f = all[i];
    f.node = node;
    std::string name = EmittedName(node, emitCtx);
    byName.emplace(name, static_cast<int32_t>(i));
    if (!node->name().empty())
      byName.emplace(node->name(), static_cast<int32_t>(i));
    f.opaque = cfg.hookedNames.contains(name) || cfg.hookedNames.contains(node->name());

    if (node->blocks().empty() || node->sharesRegisters() || IsSaveRestoreHelperName(name))
      continue;
    if (cfg.generateExceptionHandlers && node->hasExceptionInfo())
      continue;

    InsnTextSink sink;
    passCtx.insnSink = &sink;
    (void)node->emitCpp(passCtx);
    f.enabled = ParseFunction(f, sink, calleeNames[i], name);
    if (!f.enabled) {
      f.insns.clear();
      f.evs.clear();
      ++kept;
    }
  }
  passCtx.insnSink = nullptr;

  // Callee names to function indices, and who calls whom.
  for (size_t i = 0; i < all.size(); ++i) {
    Fn& f = all[i];
    for (Event& ev : f.evs) {
      if (ev.type != Event::Call || ev.target < 0)
        continue;
      const std::string& callee = calleeNames[i][ev.target];
      auto it = byName.find(callee);
      // An import is the ABI's callee; any other name without text is unread.
      ev.target = it != byName.end()             ? it->second
                  : callee.starts_with("__imp__") ? kAbiCallee
                                                  : kUnreadCallee;
      if (ev.target >= 0)
        all[ev.target].callers.push_back(static_cast<uint32_t>(i));
    }
  }

  // What a call of each function may leave changed, to a fixed point; the
  // liveness below takes it as settled.
  std::deque<uint32_t> queue;
  std::vector<bool> queued(all.size(), false);
  for (uint32_t i = 0; i < all.size(); ++i) {
    all[i].mayDefine = all[i].ownWrites;
    if (all[i].enabled) {
      queue.push_back(i);
      queued[i] = true;
    }
  }
  while (!queue.empty()) {
    uint32_t i = queue.front();
    queue.pop_front();
    queued[i] = false;
    Fn& f = all[i];
    VolatileMask defines = f.mayDefine;
    for (const Event& ev : f.evs) {
      if (ev.type == Event::Call)
        defines |= SummaryOf(all, ev.target).defines;
    }
    if (defines == f.mayDefine)
      continue;
    f.mayDefine = defines;
    for (uint32_t caller : f.callers) {
      if (all[caller].enabled && !queued[caller]) {
        queue.push_back(caller);
        queued[caller] = true;
      }
    }
  }

  // The registers live at each function's entry, to a fixed point.
  for (uint32_t i = 0; i < all.size(); ++i) {
    if (all[i].enabled) {
      queue.push_back(i);
      queued[i] = true;
    }
  }
  std::vector<VolatileMask> scratch;
  size_t solves = 0;
  while (!queue.empty()) {
    uint32_t i = queue.front();
    queue.pop_front();
    queued[i] = false;
    Fn& f = all[i];
    VolatileMask args = f.liveArgs | SolveLive(f, 0, all, scratch, nullptr);
    VolatileMask results = f.liveResults | SolveLive(f, kReturns, all, scratch, nullptr);
    ++solves;
    if (args == f.liveArgs && results == f.liveResults)
      continue;
    f.liveArgs = args;
    f.liveResults = results;
    if (f.opaque)
      continue;
    for (uint32_t caller : f.callers) {
      if (all[caller].enabled && !queued[caller]) {
        queue.push_back(caller);
        queued[caller] = true;
      }
    }
  }

  // What each function does at its entry, calls and returns.
  size_t localized = 0, calls = 0, stores = 0, loads = 0;
  std::vector<VolatileMask> liveIn, afterCall, defined;
  for (Fn& f : all) {
    VolatileFunctionInfo info;
    if (!f.enabled) {
      result.functions.emplace(f.node, std::move(info));
      continue;
    }
    ++localized;
    info.enabled = true;
    info.referenced = f.referenced;

    const size_t n = f.insns.size();
    VolatileMask entryLive = SolveLive(f, kReturns, all, liveIn, &afterCall);
    info.entryLoad = entryLive & f.referenced;
    info.fallsOffEnd = n == 0 || f.insns[n - 1].fallthrough;

    // Forward: the registers that may have been mentioned since the last call
    // (or since entry, where the ones read from PPCContext count).
    defined.assign(n, 0);
    if (n)
      defined[0] = entryLive;
    auto walk = [&](bool emit) {
      bool changed = false;
      for (size_t i = 0; i < n; ++i) {
        const Insn& insn = f.insns[i];
        VolatileMask m = defined[i];
        bool dead = false;
        std::vector<VolatileAction>* out = nullptr;
        for (uint32_t e = insn.evBegin; e < insn.evEnd && !dead; ++e) {
          const Event& ev = f.evs[e];
          VolatileAction action;
          action.line = ev.line;
          action.kind = ev.kind;
          switch (ev.type) {
            case Event::Mentions:
              m |= ev.gen | ev.kill;
              break;
            case Event::Goto:
              if ((defined[ev.target] | m) != defined[ev.target]) {
                defined[ev.target] |= m;
                changed = true;
              }
              dead = !ev.cond;
              break;
            case Event::Return:
              action.store = kReturns & f.referenced;
              dead = !ev.cond;
              break;
            case Event::Call: {
              VolatileMask after = afterCall[e];
              Summary s = SummaryOf(all, ev.target);
              action.store = m & (s.args | (s.pass & after)) & f.referenced;
              action.load = kReturns & after & f.referenced;
              // The caller's copy of a register the callee may have overwritten
              // is no longer the one to hand on; the others stay.
              VolatileMask reloaded = kReturns & after;
              m = ev.cond ? (m | reloaded) : ((m & ~s.defines) | reloaded);
              if (emit) {
                ++calls;
                stores += static_cast<size_t>(std::popcount(action.store));
                loads += static_cast<size_t>(std::popcount(action.load));
              }
              break;
            }
            case Event::Trap:
              action.store = m & kTrapReads & f.referenced;
              break;
          }
          if (emit && (action.store || action.load)) {
            if (!out)
              out = &info.actions[static_cast<uint32_t>(i)];
            out->push_back(action);
          }
        }
        if (!dead && insn.fallthrough && i + 1 < n && (defined[i + 1] | m) != defined[i + 1]) {
          defined[i + 1] |= m;
          changed = true;
        }
      }
      return changed;
    };
    while (walk(false)) {
    }
    walk(true);

    result.functions.emplace(f.node, std::move(info));
  }

  REXCODEGEN_INFO(
      "volatile_as_local: {} functions keep r3-r10, f1-f13 and v0-v13 in locals ({} stay on "
      "PPCContext, {} are replaced by hooks); {} calls with {:.2f} registers written before and "
      "{:.2f} read after each; {} solves",
      localized, kept,
      std::count_if(all.begin(), all.end(), [](const Fn& f) { return f.opaque; }), calls,
      calls ? double(stores) / double(calls) : 0.0, calls ? double(loads) / double(calls) : 0.0,
      solves);

  return result;
}

}  // namespace rex::codegen
