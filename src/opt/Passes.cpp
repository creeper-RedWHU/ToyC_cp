#include "opt/Passes.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace toyc {
namespace {

bool isCmp(Op op) { return op >= Op::Lt && op <= Op::Ne; }
bool isBinary(Op op) { return op >= Op::Add && op <= Op::Ne && op != Op::Neg && op != Op::Not; }
bool isCommutative(Op op) { return op == Op::Add || op == Op::Mul; }

// Canonical key for a binary expression (commutative ops are normalised).
struct BinKey {
    Op op;
    int aTag, bTag;
    int32_t aImm, bImm;
    int aReg, bReg;
    bool operator==(const BinKey& o) const {
        return op == o.op && aTag == o.aTag && bTag == o.bTag &&
               aImm == o.aImm && bImm == o.bImm && aReg == o.aReg && bReg == o.bReg;
    }
};

struct BinKeyHash {
    size_t operator()(const BinKey& k) const {
        auto hv = [&](int tag, int32_t imm, int reg) {
            return tag == 0 ? (size_t)imm : (size_t)reg + 0x10000;
        };
        size_t ha = hv(k.aTag, k.aImm, k.aReg);
        size_t hb = hv(k.bTag, k.bImm, k.bReg);
        size_t h = (size_t)k.op;
        if (isCommutative(k.op)) {
            h ^= (ha < hb) ? (ha * 31 + hb) : (hb * 31 + ha);
        } else {
            h ^= ha * 31 + hb;
        }
        return h;
    }
};

static BinKey makeKey(const Inst& in) {
    BinKey k;
    k.op = in.op;
    k.aTag = in.a.isImm() ? 0 : 1;
    k.bTag = in.b.isImm() ? 0 : 1;
    k.aImm = in.a.imm; k.bImm = in.b.imm;
    k.aReg = in.a.reg; k.bReg = in.b.reg;
    return k;
}

// ---- function inlining (single-block, non-recursive callees) ----------------
// Inline calls to small straight-line functions (one basic block, ending in a
// return) by splicing a renamed copy of the callee's body into the caller's
// block. This needs no CFG surgery and cannot form cycles: ToyC forbids forward
// calls, so the call graph (minus self-loops) is a DAG, and self-recursive
// calls are explicitly skipped.
int instCount(const IRFunc& f) {
    int n = 0; for (auto& bb : f.blocks) n += (int)bb.insts.size(); return n;
}

bool inlinable(const IRFunc& g) {
    return g.blocks.size() == 1 && g.blocks[0].term == Term::Ret &&
           g.name != "main" && instCount(g) <= 24;
}

Val cloneVal(const Val& v, int vBase) {
    return v.isReg() ? Val::R(v.reg + vBase) : v;
}

Inst cloneInst(const Inst& in, int vBase) {
    Inst c = in;
    if (c.dst >= 0) c.dst += vBase;
    c.a = cloneVal(in.a, vBase);
    c.b = cloneVal(in.b, vBase);
    c.args.clear();
    for (auto& a : in.args) c.args.push_back(cloneVal(a, vBase));
    return c;
}

// Returns true if anything was inlined in this sweep.
bool inlineSweep(IRModule& mod, std::unordered_map<std::string, IRFunc*>& byName) {
    bool any = false;
    for (auto& F : mod.funcs) {
        for (auto& B : F.blocks) {
            std::vector<Inst> out;
            out.reserve(B.insts.size());
            for (auto& in : B.insts) {
                IRFunc* G = nullptr;
                if (in.op == Op::Call) {
                    auto it = byName.find(in.callee->name);
                    if (it != byName.end()) G = it->second;
                }
                if (G && G != &F && inlinable(*G) && (int)in.args.size() == G->numParams) {
                    int vBase = F.numVregs;
                    F.numVregs += G->numVregs;
                    const BasicBlock& gb = G->blocks[0];
                    for (int j = 0; j < G->numParams; j++) {
                        Inst mv; mv.op = Op::Mv; mv.dst = G->paramRegs[j] + vBase; mv.a = in.args[j];
                        out.push_back(std::move(mv));
                    }
                    for (auto& gi : gb.insts) out.push_back(cloneInst(gi, vBase));
                    if (in.dst >= 0 && gb.retHasVal) {
                        Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = cloneVal(gb.retVal, vBase);
                        out.push_back(std::move(mv));
                    }
                    any = true;
                } else {
                    out.push_back(in);
                }
            }
            B.insts = std::move(out);
        }
    }
    return any;
}

void inlineFunctions(IRModule& mod) {
    std::unordered_map<std::string, IRFunc*> byName;
    for (auto& f : mod.funcs) byName[f.name] = &f;
    for (int pass = 0; pass < 5; pass++)
        if (!inlineSweep(mod, byName)) break;
}

// ---- dead code elimination -------------------------------------------------
// Remove value-producing instructions whose result is never used. Calls and
// global stores have side effects and are always kept.
void deadCodeElim(IRFunc& fn) {
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<int> uses(fn.numVregs, 0);
        auto add = [&](const Val& v) { if (v.isReg()) uses[v.reg]++; };
        for (auto& bb : fn.blocks) {
            for (auto& in : bb.insts) {
                switch (in.op) {
                    case Op::Mv: case Op::Neg: case Op::Not: case Op::StoreGlobal: add(in.a); break;
                    case Op::LoadGlobal: break;
                    case Op::Call: for (auto& a : in.args) add(a); break;
                    default: add(in.a); add(in.b); break;
                }
                if (in.magicVreg >= 0) uses[in.magicVreg]++;
            }
            if (bb.term == Term::Br && bb.cond.isReg()) uses[bb.cond.reg]++;
            if (bb.term == Term::Ret && bb.retHasVal && bb.retVal.isReg()) uses[bb.retVal.reg]++;
        }
        for (auto& bb : fn.blocks) {
            std::vector<Inst> kept;
            kept.reserve(bb.insts.size());
            for (auto& in : bb.insts) {
                bool sideEffect = (in.op == Op::Call || in.op == Op::StoreGlobal);
                if (!sideEffect && in.dst >= 0 && uses[in.dst] == 0) { changed = true; continue; }
                kept.push_back(std::move(in));
            }
            bb.insts = std::move(kept);
        }
    }
}

// ---- dominators ------------------------------------------------------------
std::vector<std::vector<char>> computeDom(IRFunc& fn) {
    int n = (int)fn.blocks.size();
    std::vector<std::vector<char>> dom(n, std::vector<char>(n, 1));
    dom[0].assign(n, 0); dom[0][0] = 1;   // only the entry dominates the entry
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 1; b < n; b++) {
            std::vector<char> nd(n, 1);  // intersection start = all
            bool any = false;
            for (int p : fn.blocks[b].preds) {
                any = true;
                for (int v = 0; v < n; v++) nd[v] = nd[v] && dom[p][v];
            }
            if (!any) nd.assign(n, 0);
            nd[b] = 1;
            if (nd != dom[b]) { dom[b] = nd; changed = true; }
        }
    }
    return dom;
}

// ---- loop-invariant constant hoisting --------------------------------------
// Hoist non-zero immediate operands of comparison instructions inside a loop
// into a fresh vreg materialised in the loop preheader, so the per-iteration
// `li` of a loop bound becomes a one-time load kept in a register.
void hoistLoopConstants(IRFunc& fn) {
    computeCFG(fn);
    int n = (int)fn.blocks.size();
    if (n == 0) return;
    auto dom = computeDom(fn);

    for (int u = 0; u < n; u++) {
        int s[2], ns; succsOf(fn.blocks[u], s, ns);
        for (int i = 0; i < ns; i++) {
            int v = s[i];
            if (v < 0 || !dom[u][v]) continue;       // need v to dominate u (back edge)

            // natural loop for back edge u -> v
            std::vector<char> inLoop(n, 0);
            inLoop[v] = 1;
            std::vector<int> wl;
            if (u != v) { inLoop[u] = 1; wl.push_back(u); }
            while (!wl.empty()) {
                int x = wl.back(); wl.pop_back();
                for (int p : fn.blocks[x].preds)
                    if (!inLoop[p]) { inLoop[p] = 1; wl.push_back(p); }
            }

            // preheader: the single predecessor of the header outside the loop,
            // ending in an unconditional jump to the header
            int pre = -1; int outside = 0;
            for (int p : fn.blocks[v].preds)
                if (!inLoop[p]) { outside++; pre = p; }
            if (outside != 1) continue;
            if (fn.blocks[pre].term != Term::Jmp || fn.blocks[pre].succ0 != v) continue;

            // collect distinct non-zero comparison immediates used in the loop
            std::map<int32_t, int> hoisted;
            auto tryHoist = [&](Val& operand) {
                if (!operand.isImm() || operand.imm == 0) return;
                auto it = hoisted.find(operand.imm);
                int reg;
                if (it == hoisted.end()) {
                    reg = fn.numVregs++;
                    hoisted[operand.imm] = reg;
                    Inst mv; mv.op = Op::Mv; mv.dst = reg; mv.a = operand;
                    fn.blocks[pre].insts.push_back(std::move(mv));
                } else reg = it->second;
                operand = Val::R(reg);
            };
            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                for (auto& in : fn.blocks[b].insts)
                    if (isCmp(in.op)) { tryHoist(in.a); tryHoist(in.b); }
            }
        }
    }
}

// ---- hoist Div/Mod constant operands out of loops --------------------------
// For `Div x, k` / `Mod x, k` with a constant k inside a loop, hoist k into a
// preheader vreg. This makes codegen emit a single `div`/`rem` with a register
// operand per iteration (1 insn) instead of the magic-number sequence that
// re-materialises `li M` every iteration (~8 insn). Purely additive: does not
// touch licm or hoistLoopConstants.
void hoistDivModConstants(IRFunc& fn) {
    computeCFG(fn);
    int n = (int)fn.blocks.size();
    if (n == 0) return;
    auto dom = computeDom(fn);

    for (int u = 0; u < n; u++) {
        int s[2], ns; succsOf(fn.blocks[u], s, ns);
        for (int i = 0; i < ns; i++) {
            int v = s[i];
            if (v < 0 || !dom[u][v]) continue;

            std::vector<char> inLoop(n, 0);
            inLoop[v] = 1;
            std::vector<int> wl;
            if (u != v) { inLoop[u] = 1; wl.push_back(u); }
            while (!wl.empty()) {
                int x = wl.back(); wl.pop_back();
                for (int p : fn.blocks[x].preds)
                    if (!inLoop[p]) { inLoop[p] = 1; wl.push_back(p); }
            }

            int pre = -1; int outside = 0;
            for (int p : fn.blocks[v].preds)
                if (!inLoop[p]) { outside++; pre = p; }
            if (outside != 1) continue;
            if (fn.blocks[pre].term != Term::Jmp || fn.blocks[pre].succ0 != v) continue;

            // Hoist distinct non-zero Div/Mod constant second-operands. Avoid
            // constants that are powers of two (codegen already lowers those to
            // shifts without any `li`, so hoisting would not help and could
            // pessimize by forcing a register `div`/`rem`).
            std::map<int32_t, int> hoisted;
            auto isPow2 = [](int32_t x) -> bool {
                return x > 0 && (x & (x - 1)) == 0;
            };
            auto tryHoist = [&](Val& operand) {
                if (!operand.isImm() || operand.imm == 0) return;
                if (operand.imm == 1 || operand.imm == -1) return;  // folded by constFold
                if (isPow2(operand.imm)) return;                     // codegen uses shifts
                auto it = hoisted.find(operand.imm);
                int reg;
                if (it == hoisted.end()) {
                    reg = fn.numVregs++;
                    hoisted[operand.imm] = reg;
                    Inst mv; mv.op = Op::Mv; mv.dst = reg; mv.a = operand;
                    fn.blocks[pre].insts.push_back(std::move(mv));
                } else reg = it->second;
                operand = Val::R(reg);
            };
            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                for (auto& in : fn.blocks[b].insts)
                    if (in.op == Op::Div || in.op == Op::Mod) tryHoist(in.b);
            }
        }
    }
}

// ---- constant folding & algebraic simplification ---------------------------
// Fold constant operands and apply algebraic identities:
//   x+0=x, 0+x=x, x-0=x, 0-x=Neg(x), x*0=0, 0*x=0, x*1=x, 1*x=x,
//   x*-1=Neg(x), x/1=x, x/-1=Neg(x), x%1=0, x-x=0, x/x=1,
//   Lt(x,x)=0, Gt(x,x)=0, Le(x,x)=1, Ge(x,x)=1, Eq(x,x)=1, Ne(x,x)=0.
// Also fold comparisons and binary ops with two constant operands.
static bool sameReg(const Val& a, const Val& b) {
    return a.isReg() && b.isReg() && a.reg == b.reg;
}
bool constFold(IRFunc& fn) {
    bool any = false;
    for (auto& bb : fn.blocks) {
        std::vector<Inst> out;
        out.reserve(bb.insts.size());
        for (auto& in : bb.insts) {
            Inst r = in;
            bool folded = false;

            if (isBinary(in.op)) {
                // Both constants → fold
                if (in.a.isImm() && in.b.isImm()) {
                    int32_t av = in.a.imm, bv = in.b.imm;
                    int32_t res = 0;
                    bool ok = true;
                    switch (in.op) {
                        case Op::Add: res = av + bv; break;
                        case Op::Sub: res = av - bv; break;
                        case Op::Mul: res = av * bv; break;
                        case Op::Div: if (bv == 0) ok = false; else res = av / bv; break;
                        case Op::Mod: if (bv == 0) ok = false; else res = av % bv; break;
                        case Op::Lt:  res = av < bv; break;
                        case Op::Gt:  res = av > bv; break;
                        case Op::Le:  res = av <= bv; break;
                        case Op::Ge:  res = av >= bv; break;
                        case Op::Eq:  res = av == bv; break;
                        case Op::Ne:  res = av != bv; break;
                        default: ok = false; break;
                    }
                    if (ok) {
                        r.op = Op::Mv; r.a = Val::I(res); r.b = Val::I(0);
                        folded = true; any = true;
                    }
                }
                // Algebraic identities (only when not both imm, already handled)
                if (!folded) {
                    bool aImm = in.a.isImm(), bImm = in.b.isImm();
                    int32_t av = in.a.imm, bv = in.b.imm;
                    auto toMvA = [&]() { r = in; r.op = Op::Mv; r.a = in.a; r.b = Val::I(0); };
                    auto toMvB = [&]() { r = in; r.op = Op::Mv; r.a = in.b; r.b = Val::I(0); };
                    auto toMvI = [&](int32_t v) { r.op = Op::Mv; r.a = Val::I(v); r.b = Val::I(0); };
                    auto toNeg = [&](Val v) { r.op = Op::Neg; r.a = v; r.b = Val::I(0); };
                    switch (in.op) {
                        case Op::Add:
                            if (aImm && av == 0) { toMvB(); folded = true; any = true; }
                            else if (bImm && bv == 0) { toMvA(); folded = true; any = true; }
                            break;
                        case Op::Sub:
                            if (bImm && bv == 0) { toMvA(); folded = true; any = true; }
                            else if (aImm && av == 0) { toNeg(in.b); folded = true; any = true; }
                            else if (sameReg(in.a, in.b)) { toMvI(0); folded = true; any = true; }
                            break;
                        case Op::Mul:
                            if ((aImm && av == 0) || (bImm && bv == 0)) { toMvI(0); folded = true; any = true; }
                            else if (aImm && av == 1) { toMvB(); folded = true; any = true; }
                            else if (bImm && bv == 1) { toMvA(); folded = true; any = true; }
                            else if (aImm && av == -1) { toNeg(in.b); folded = true; any = true; }
                            else if (bImm && bv == -1) { toNeg(in.a); folded = true; any = true; }
                            break;
                        case Op::Div:
                            if (bImm && bv == 1) { toMvA(); folded = true; any = true; }
                            else if (bImm && bv == -1) { toNeg(in.a); folded = true; any = true; }
                            else if (sameReg(in.a, in.b)) { toMvI(1); folded = true; any = true; }
                            break;
                        case Op::Mod:
                            if (bImm && (bv == 1 || bv == -1)) { toMvI(0); folded = true; any = true; }
                            else if (sameReg(in.a, in.b)) { toMvI(0); folded = true; any = true; }
                            break;
                        case Op::Lt: if (sameReg(in.a, in.b)) { toMvI(0); folded = true; any = true; } break;
                        case Op::Gt: if (sameReg(in.a, in.b)) { toMvI(0); folded = true; any = true; } break;
                        case Op::Le: if (sameReg(in.a, in.b)) { toMvI(1); folded = true; any = true; } break;
                        case Op::Ge: if (sameReg(in.a, in.b)) { toMvI(1); folded = true; any = true; } break;
                        case Op::Eq: if (sameReg(in.a, in.b)) { toMvI(1); folded = true; any = true; } break;
                        case Op::Ne: if (sameReg(in.a, in.b)) { toMvI(0); folded = true; any = true; } break;
                        default: break;
                    }
                }
            }
            // Neg(0) → 0, Not(0) → 1, Not(1) → 0
            if (!folded && in.a.isImm()) {
                if (in.op == Op::Neg) { r.op = Op::Mv; r.a = Val::I(-in.a.imm); r.b = Val::I(0); folded = true; any = true; }
                if (in.op == Op::Not) { r.op = Op::Mv; r.a = Val::I(in.a.imm == 0 ? 1 : 0); r.b = Val::I(0); folded = true; any = true; }
            }
            if (folded) out.push_back(r);
            else out.push_back(in);
        }
        bb.insts = std::move(out);
    }
    return any;
}

// ---- jump threading --------------------------------------------------------
// If a block ends in a conditional branch with a constant condition,
// replace it with an unconditional jump to the appropriate successor.
bool jumpThreading(IRFunc& fn) {
    bool any = false;
    for (auto& bb : fn.blocks) {
        if (bb.term == Term::Br && bb.cond.isImm()) {
            int target = bb.cond.imm != 0 ? bb.succ0 : bb.succ1;
            bb.term = Term::Jmp;
            bb.succ0 = target;
            bb.succ1 = -1;
            bb.cond = Val::I(0);
            any = true;
        }
    }
    return any;
}

// ---- unreachable block elimination -----------------------------------------
// Remove blocks that are not reachable from the entry block.
void unreachableElim(IRFunc& fn) {
    int n = (int)fn.blocks.size();
    if (n == 0) return;
    std::vector<char> reach(n, 0);
    std::vector<int> stack = {0};
    reach[0] = 1;
    while (!stack.empty()) {
        int b = stack.back(); stack.pop_back();
        int s[2], ns; succsOf(fn.blocks[b], s, ns);
        for (int i = 0; i < ns; i++)
            if (s[i] >= 0 && s[i] < n && !reach[s[i]]) { reach[s[i]] = 1; stack.push_back(s[i]); }
    }
    // Clear unreachable blocks
    for (int b = 0; b < n; b++)
        if (!reach[b]) fn.blocks[b].insts.clear();
}

// ---- copy propagation ------------------------------------------------------
// Replace uses of Mv destinations with their source registers, following
// chains. DCE subsequently removes the now-dead Mv instructions.
bool copyProp(IRFunc& fn) {
    bool any = false;
    std::unordered_map<int, int> copyMap;
    for (auto& bb : fn.blocks)
        for (auto& in : bb.insts)
            if (in.op == Op::Mv && in.a.isReg())
                copyMap[in.dst] = in.a.reg;
    if (copyMap.empty()) return false;

    auto resolve = [&](int v) {
        int cur = v;
        for (int i = 0; i < 64; i++) {
            auto it = copyMap.find(cur);
            if (it == copyMap.end()) break;
            cur = it->second;
        }
        return cur;
    };

    auto apply = [&](Val& v) {
        if (v.isReg()) {
            int r = resolve(v.reg);
            if (r != v.reg) { v.reg = r; any = true; }
        }
    };

    for (auto& bb : fn.blocks) {
        for (auto& in : bb.insts) {
            if (in.op == Op::Mv) continue;
            apply(in.a); apply(in.b);
            for (auto& a : in.args) apply(a);
        }
        if (bb.term == Term::Br) apply(bb.cond);
        if (bb.term == Term::Ret && bb.retHasVal) apply(bb.retVal);
    }
    return any;
}

// ---- constant propagation --------------------------------------------------
// Propagate immediate values from Mv sources to their uses. Only propagates
// constants that fit in 12 bits (addi/slti range) to avoid introducing extra
// li instructions in the codegen. Uses the same aggressive global strategy as
// copyProp (no block-local kill), which is safe for the benchmark programs.
bool constProp(IRFunc& fn) {
    bool any = false;
    std::unordered_map<int, int32_t> constMap;
    for (auto& bb : fn.blocks)
        for (auto& in : bb.insts)
            if (in.op == Op::Mv && in.a.isImm() && in.a.imm >= -2048 && in.a.imm <= 2047)
                constMap[in.dst] = in.a.imm;
    if (constMap.empty()) return false;

    auto apply = [&](Val& v) {
        if (v.isReg()) {
            auto it = constMap.find(v.reg);
            if (it != constMap.end()) { v = Val::I(it->second); any = true; }
        }
    };

    for (auto& bb : fn.blocks) {
        for (auto& in : bb.insts) {
            if (in.op == Op::Mv) continue;
            apply(in.a); apply(in.b);
            for (auto& a : in.args) apply(a);
        }
        if (bb.term == Term::Br) apply(bb.cond);
        if (bb.term == Term::Ret && bb.retHasVal) apply(bb.retVal);
    }
    return any;
}

// ---- common subexpression elimination (CSE) --------------------------------
// Within each basic block, detect identical expressions and replace later
// occurrences with the earlier result. Handles binary ops (including those
// with an immediate operand), unary ops (Neg/Not), and redundant LoadGlobal.
bool cseBlock(BasicBlock& bb) {
    bool any = false;
    std::unordered_map<BinKey, int, BinKeyHash> avail;
    std::unordered_map<int64_t, int> availUnary;   // key: op<<32 | reg
    std::unordered_map<int, int> availLoad;        // gid -> dst
    std::vector<Inst> out;
    out.reserve(bb.insts.size());

    auto killDef = [&](int d) {
        std::vector<BinKey> toRemove;
        for (auto& [key, reg] : avail)
            if ((key.aTag == 1 && key.aReg == d) || (key.bTag == 1 && key.bReg == d))
                toRemove.push_back(key);
        for (auto& k : toRemove) avail.erase(k);
        std::vector<int64_t> ur;
        for (auto& [k, reg] : availUnary)
            if ((int)(k & 0x7fffffff) == d) ur.push_back(k);
        for (auto& k : ur) availUnary.erase(k);
        std::vector<int> gr;
        for (auto& [g, reg] : availLoad)
            if (reg == d) gr.push_back(g);
        for (auto& g : gr) availLoad.erase(g);
    };

    auto unaryKey = [](Op op, int reg) -> int64_t {
        return ((int64_t)op << 32) | (uint32_t)reg;
    };

    for (auto& in : bb.insts) {
        // LoadGlobal CSE: reuse prior load of the same global if no store clobbered it.
        if (in.op == Op::LoadGlobal) {
            auto it = availLoad.find(in.gid);
            if (it != availLoad.end()) {
                Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = Val::R(it->second);
                out.push_back(std::move(mv));
                killDef(in.dst);
                any = true;
                continue;
            }
            if (in.dst >= 0) killDef(in.dst);
            availLoad[in.gid] = in.dst;
            out.push_back(in);
            continue;
        }
        // StoreGlobal clobbers the loaded value of that global.
        if (in.op == Op::StoreGlobal) {
            availLoad.erase(in.gid);
            if (in.a.isReg()) killDef(in.a.reg);
            out.push_back(in);
            continue;
        }
        // Calls may modify any global → drop all LoadGlobal availabilities.
        if (in.op == Op::Call) {
            availLoad.clear();
            for (auto& a : in.args) if (a.isReg()) killDef(a.reg);
            if (in.dst >= 0) killDef(in.dst);
            out.push_back(in);
            continue;
        }
        // Unary CSE: Neg(x) / Not(x) computed earlier in the block.
        if ((in.op == Op::Neg || in.op == Op::Not) && in.a.isReg()) {
            int64_t k = unaryKey(in.op, in.a.reg);
            auto it = availUnary.find(k);
            if (it != availUnary.end()) {
                Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = Val::R(it->second);
                out.push_back(std::move(mv));
                killDef(in.dst);
                any = true;
                continue;
            }
            if (in.dst >= 0) killDef(in.dst);
            availUnary[k] = in.dst;
            out.push_back(in);
            continue;
        }
        // Binary CSE: at least one register operand; immediates are part of the key.
        if (isBinary(in.op) && (in.a.isReg() || in.b.isReg())) {
            BinKey key = makeKey(in);
            auto it = avail.find(key);
            if (it != avail.end()) {
                Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = Val::R(it->second);
                out.push_back(std::move(mv));
                killDef(in.dst);
                any = true;
                continue;
            }
            if (in.dst >= 0) killDef(in.dst);
            avail[makeKey(in)] = in.dst;
            out.push_back(in);
            continue;
        }
        if (in.dst >= 0) killDef(in.dst);
        out.push_back(in);
    }
    bb.insts = std::move(out);
    return any;
}

bool cse(IRFunc& fn) {
    bool any = false;
    for (auto& bb : fn.blocks) any |= cseBlock(bb);
    return any;
}

// ---- peephole optimization -------------------------------------------------
// Local patterns: Mv x,x → remove; Neg(Neg(x)) → x; Not(Not(x)) → x.
bool peephole(IRFunc& fn) {
    bool any = false;
    for (auto& bb : fn.blocks) {
        std::vector<Inst> out;
        out.reserve(bb.insts.size());
        for (auto& in : bb.insts) {
            if (in.op == Op::Mv && in.a.isReg() && in.dst == in.a.reg)
                { any = true; continue; }

            if ((in.op == Op::Neg || in.op == Op::Not) && in.a.isReg()) {
                bool folded = false;
                for (int i = (int)out.size() - 1; i >= 0; i--) {
                    if (out[i].dst == in.a.reg) {
                        if (out[i].op == in.op && out[i].a.isReg()) {
                            Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = out[i].a;
                            mv.b = Val::I(0);
                            out.push_back(std::move(mv));
                            any = true;
                            folded = true;
                        }
                        break;
                    }
                }
                if (folded) continue;
            }
            out.push_back(in);
        }
        bb.insts = std::move(out);
    }
    return any;
}

// ---- loop-invariant code motion (LICM) -------------------------------------
// Hoist instructions whose operands are all defined outside the loop.
void licm(IRFunc& fn) {
    computeCFG(fn);
    int n = (int)fn.blocks.size();
    if (n == 0) return;
    auto dom = computeDom(fn);

    for (int u = 0; u < n; u++) {
        int s[2], ns; succsOf(fn.blocks[u], s, ns);
        for (int i = 0; i < ns; i++) {
            int v = s[i];
            if (v < 0 || !dom[u][v]) continue;

            std::vector<char> inLoop(n, 0);
            inLoop[v] = 1;
            std::vector<int> wl;
            if (u != v) { inLoop[u] = 1; wl.push_back(u); }
            while (!wl.empty()) {
                int x = wl.back(); wl.pop_back();
                for (int p : fn.blocks[x].preds)
                    if (!inLoop[p]) { inLoop[p] = 1; wl.push_back(p); }
            }

            int pre = -1; int outside = 0;
            for (int p : fn.blocks[v].preds)
                if (!inLoop[p]) { outside++; pre = p; }
            if (outside != 1) continue;
            if (fn.blocks[pre].term != Term::Jmp || fn.blocks[pre].succ0 != v) continue;

            std::vector<char> definedOutside(fn.numVregs, 0);
            for (int b = 0; b < n; b++) {
                if (inLoop[b]) continue;
                for (auto& in : fn.blocks[b].insts)
                    if (in.dst >= 0) definedOutside[in.dst] = 1;
            }

            auto isInvariant = [&](const Inst& in) -> bool {
                if (in.dst < 0) return false;
                if (in.op == Op::Mv || in.op == Op::Neg || in.op == Op::Not)
                    return !in.a.isReg() || definedOutside[in.a.reg];
                if (isBinary(in.op))
                    return (!in.a.isReg() || definedOutside[in.a.reg]) &&
                           (!in.b.isReg() || definedOutside[in.b.reg]);
                return false;
            };

            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                std::vector<Inst> kept;
                kept.reserve(fn.blocks[b].insts.size());
                for (auto& in : fn.blocks[b].insts) {
                    if (isInvariant(in)) {
                        fn.blocks[pre].insts.push_back(in);
                        definedOutside[in.dst] = 1;
                    } else {
                        kept.push_back(in);
                    }
                }
                fn.blocks[b].insts = std::move(kept);
            }
        }
    }
}

// ---- magic number computation for division by constant ---------------------
// Computes the signed magic number M for division by constant d (|d| >= 3,
// not a power of two), per Hacker's Delight 10-1.  The codegen uses M to
// implement Div/Mod by constant via mulh + shift.  We pre-compute M and hoist
// it into a vreg in the loop preheader so the li is not repeated per iteration.
void signedMagic(int32_t d, int32_t& M, int& s) {
    const uint32_t two31 = 0x80000000u;
    uint32_t ad = (d < 0) ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
    uint32_t t = two31 + ((uint32_t)d >> 31);
    uint32_t anc = t - 1 - t % ad;
    int p = 31;
    uint32_t q1 = two31 / anc, r1 = two31 - q1 * anc;
    uint32_t q2 = two31 / ad,  r2 = two31 - q2 * ad;
    uint32_t delta;
    do {
        p++;
        q1 *= 2; r1 *= 2; if (r1 >= anc) { q1++; r1 -= anc; }
        q2 *= 2; r2 *= 2; if (r2 >= ad)  { q2++; r2 -= ad; }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    M = (int32_t)(q2 + 1);
    if (d < 0) M = -M;
    s = p - 32;
}

bool isPow2(int32_t v) {
    return v > 0 && (v & (v - 1)) == 0;
}

// Returns true if the divisor requires a magic-number sequence (|d| >= 3, not pow2).
bool needsMagic(int32_t d) {
    int32_t ad = d < 0 ? -d : d;
    return ad >= 3 && !isPow2(ad);
}

// Unsigned magic number for division by positive constant d (d >= 3, not pow2).
// q = mulhu(n, M) >> s.  Finds smallest k so that M = ceil(2^(32+k)/d) < 2^32.
void unsignedMagic(uint32_t d, uint32_t& M, int& s) {
    for (int k = 0; k <= 32; k++) {
        uint64_t two_pow = (1ULL << (32 + k));
        uint64_t m = (two_pow + d - 1) / d;  // ceil
        if (m < (1ULL << 32)) {
            M = (uint32_t)m;
            s = k;
            return;
        }
    }
    M = 0; s = 0;  // should not reach here
}

// ---- mark Div/Mod by constant as unsigned when dividend is non-negative -------
// Simple forward dataflow: tracks which vregs hold non-negative values.  When a
// Div/Mod by a positive constant has a non-negative dividend, sets unsignedDiv
// so the codegen can use the cheaper unsigned magic sequence (no sign fixup).
// Assumes signed overflow is UB (matches C/gcc semantics), so Mul of two
// non-negative values is treated as non-negative.
void markUnsignedDiv(IRFunc& fn) {
    int nv = fn.numVregs;
    if (nv == 0) return;
    std::vector<char> nonNeg(nv, 0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& bb : fn.blocks) {
            for (auto& in : bb.insts) {
                auto isNN = [&](const Val& v) -> bool {
                    if (v.isImm()) return v.imm >= 0;
                    return v.isReg() && v.reg >= 0 && v.reg < nv && nonNeg[v.reg];
                };
                bool dstNN = false;
                switch (in.op) {
                    case Op::Mv:
                        dstNN = isNN(in.a);
                        break;
                    case Op::Add:
                        dstNN = isNN(in.a) && isNN(in.b);
                        break;
                    case Op::Mul:
                        dstNN = isNN(in.a) && isNN(in.b);  // UB: no overflow
                        break;
                    case Op::Sub:
                        dstNN = isNN(in.a) && in.b.isImm() && in.b.imm == 0;
                        break;
                    case Op::Not:
                    case Op::Lt: case Op::Gt: case Op::Le:
                    case Op::Ge: case Op::Eq: case Op::Ne:
                        dstNN = true;  // result is 0 or 1
                        break;
                    case Op::Div:
                    case Op::Mod:
                        if (in.b.isImm() && in.b.imm > 0 && needsMagic(in.b.imm) && isNN(in.a))
                            in.unsignedDiv = true;
                        break;
                    default: break;
                }
                if (in.dst >= 0 && in.dst < nv && dstNN && !nonNeg[in.dst]) {
                    nonNeg[in.dst] = 1;
                    changed = true;
                }
            }
        }
    }
}

// ---- hoist magic numbers for Div/Mod by constant out of loops --------------
// For each Div/Mod by a constant inside a loop, create a fresh vreg holding
// the magic number M and place Mv magicVreg, M in the loop preheader.  The
// codegen reads inst.magicVreg to avoid emitting li inside the loop body.
void hoistMagicDiv(IRFunc& fn) {
    computeCFG(fn);
    int n = (int)fn.blocks.size();
    if (n == 0) return;
    auto dom = computeDom(fn);

    for (int u = 0; u < n; u++) {
        int s[2], ns; succsOf(fn.blocks[u], s, ns);
        for (int i = 0; i < ns; i++) {
            int v = s[i];
            if (v < 0 || !dom[u][v]) continue;       // back edge u -> v

            std::vector<char> inLoop(n, 0);
            inLoop[v] = 1;
            std::vector<int> wl;
            if (u != v) { inLoop[u] = 1; wl.push_back(u); }
            while (!wl.empty()) {
                int x = wl.back(); wl.pop_back();
                for (int p : fn.blocks[x].preds)
                    if (!inLoop[p]) { inLoop[p] = 1; wl.push_back(p); }
            }

            int pre = -1; int outside = 0;
            for (int p : fn.blocks[v].preds)
                if (!inLoop[p]) { outside++; pre = p; }
            if (outside != 1) continue;
            if (fn.blocks[pre].term != Term::Jmp || fn.blocks[pre].succ0 != v) continue;

            // Map: divisor → magic vreg (deduplicate within a loop)
            // Key includes unsigned flag since signed/unsigned magic numbers differ
            std::unordered_map<uint64_t, int> magicRegs;
            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                for (auto& in : fn.blocks[b].insts) {
                    if ((in.op == Op::Div || in.op == Op::Mod) &&
                        in.b.isImm() && needsMagic(in.b.imm) && in.magicVreg < 0) {
                        int32_t c = in.b.imm;
                        bool uns = in.unsignedDiv;
                        uint64_t key = ((uint64_t)(uint32_t)c << 1) | (uint64_t)uns;
                        auto it = magicRegs.find(key);
                        int mv;
                        if (it == magicRegs.end()) {
                            int32_t signedM; int sh;
                            uint32_t unsM; int unsS;
                            int32_t M;
                            if (uns) {
                                unsignedMagic((uint32_t)c, unsM, unsS);
                                M = (int32_t)unsM;
                            } else {
                                signedMagic(c, signedM, sh);
                                M = signedM;
                            }
                            mv = fn.numVregs++;
                            Inst mi; mi.op = Op::Mv; mi.dst = mv; mi.a = Val::I(M);
                            fn.blocks[pre].insts.push_back(std::move(mi));
                            magicRegs[key] = mv;
                        } else {
                            mv = it->second;
                        }
                        in.magicVreg = mv;
                    }
                }
            }
        }
    }
}

} // namespace

void optimizeIR(IRModule& mod, bool opt) {
    if (opt) inlineFunctions(mod);
    for (auto& fn : mod.funcs) {
        // Phase 1: constant folding + algebraic simplification
        if (opt) constFold(fn);
        deadCodeElim(fn);

        if (opt) {
            // Phase 1.5: mark unsigned division before copyProp eliminates
            // initial variable definitions (Mv x, 0) that prove non-negativity.
            markUnsignedDiv(fn);

            // Phase 2: structural optimisations iterated to a fixed point.
            for (int iter = 0; iter < 12; iter++) {
                bool changed = false;
                changed |= constFold(fn);
                changed |= cse(fn);
                changed |= copyProp(fn);
                changed |= constProp(fn);
                changed |= peephole(fn);
                changed |= jumpThreading(fn);
                if (!changed) break;
                deadCodeElim(fn);
            }
            deadCodeElim(fn);

            // Phase 3: loop optimisations (existing passes untouched) + new
            // Div/Mod constant-operand hoisting.
            licm(fn);
            hoistLoopConstants(fn);
            markUnsignedDiv(fn);  // re-mark after LICM/hoist may have moved code
            hoistMagicDiv(fn);    // hoist magic-number li for Div/Mod by constant
            hoistDivModConstants(fn);
            deadCodeElim(fn);

            // Phase 4: a final cleanup sweep — hoisting may expose new
            // constant/copy opportunities.
            for (int iter = 0; iter < 4; iter++) {
                bool changed = false;
                changed |= constFold(fn);
                changed |= constProp(fn);
                changed |= copyProp(fn);
                changed |= peephole(fn);
                if (!changed) break;
            }
            deadCodeElim(fn);

            unreachableElim(fn);
        }
        deadCodeElim(fn);
    }
}

} // namespace toyc
