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
           g.name != "main" && instCount(g) <= 60;
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

// ---- multi-block inlining --------------------------------------------------
// Inline calls to small multi-block callees (if-else / short loops) by stitching
// the callee's CFG into the caller. The caller block holding the call is split
// at the call point: instructions before the call stay (followed by parameter
// Mv's and a jump into the callee entry); instructions after the call move into
// a fresh "continuation" block that inherits the original terminator. The
// callee's blocks are appended with their block ids and vregs renamed; each
// callee Ret becomes `Mv dst = retval` (when the call result is used) followed
// by a jump to the continuation block. Because succ ids equal block indices,
// appending blocks at the tail keeps id == index consistent.
bool selfRecursive(const IRFunc& g) {
    for (auto& bb : g.blocks)
        for (auto& in : bb.insts)
            if (in.op == Op::Call && in.callee && in.callee->name == g.name) return true;
    return false;
}

bool multiInlinable(const IRFunc& g, const IRFunc& f) {
    if (&g == &f) return false;             // never inline directly into itself
    if (g.name == "main") return false;
    if (g.blocks.size() < 2) return false;  // single-block handled by inlineSweep
    if (g.blocks.size() > 8) return false;  // cap CFG growth
    if (instCount(g) > 40) return false;    // cap code bloat
    if (selfRecursive(g)) return false;     // never inline (self-)recursive callees
    for (auto& bb : g.blocks)
        if (bb.term == Term::None) return false;  // must be fully terminated
    return true;
}

// Splice callee G into F at block bi, instruction index ci (the Call).
void doMultiInline(IRFunc& F, int bi, int ci, const IRFunc& G) {
    int nF = (int)F.blocks.size();
    int vBase = F.numVregs;
    F.numVregs += G.numVregs;
    int contId = nF;          // continuation block appended first
    int gBase  = nF + 1;      // callee blocks appended after it

    // Copy call info before mutating B (its storage is about to change).
    Inst call = F.blocks[bi].insts[ci];
    int callDst = call.dst;

    // Build the continuation block from B's post-call tail; it inherits B's term.
    BasicBlock cont;
    cont.id = contId;
    {
        BasicBlock& B = F.blocks[bi];
        for (int k = ci + 1; k < (int)B.insts.size(); k++) cont.insts.push_back(B.insts[k]);
        cont.term = B.term; cont.cond = B.cond;
        cont.succ0 = B.succ0; cont.succ1 = B.succ1;
        cont.retHasVal = B.retHasVal; cont.retVal = B.retVal;

        // Rewrite B: keep pre-call insts, append parameter Mv's, jump to entry.
        B.insts.resize(ci);
        for (int j = 0; j < G.numParams; j++) {
            Inst mv; mv.op = Op::Mv; mv.dst = G.paramRegs[j] + vBase; mv.a = call.args[j];
            B.insts.push_back(std::move(mv));
        }
        B.term = Term::Jmp; B.succ0 = gBase; B.succ1 = -1;
        B.cond = Val::I(0); B.retHasVal = false;
    }

    // Materialise the remapped callee blocks (entry is block 0 -> gBase).
    std::vector<BasicBlock> appended;
    appended.push_back(std::move(cont));
    for (auto& gb : G.blocks) {
        BasicBlock nb;
        nb.id = gBase + gb.id;
        for (auto& gi : gb.insts) nb.insts.push_back(cloneInst(gi, vBase));
        if (gb.term == Term::Ret) {
            if (callDst >= 0 && gb.retHasVal) {
                Inst mv; mv.op = Op::Mv; mv.dst = callDst; mv.a = cloneVal(gb.retVal, vBase);
                nb.insts.push_back(std::move(mv));
            }
            nb.term = Term::Jmp; nb.succ0 = contId; nb.succ1 = -1;
        } else {
            nb.term = gb.term;
            nb.cond = cloneVal(gb.cond, vBase);
            nb.succ0 = gb.succ0 >= 0 ? gBase + gb.succ0 : -1;
            nb.succ1 = gb.succ1 >= 0 ? gBase + gb.succ1 : -1;
        }
        appended.push_back(std::move(nb));
    }
    for (auto& nb : appended) F.blocks.push_back(std::move(nb));
    computeCFG(F);
}

// Inline the first multi-block call found in F. Returns true if one was inlined.
bool inlineOneMulti(IRFunc& F, std::unordered_map<std::string, IRFunc*>& byName) {
    for (int bi = 0; bi < (int)F.blocks.size(); bi++) {
        for (int ci = 0; ci < (int)F.blocks[bi].insts.size(); ci++) {
            const Inst& in = F.blocks[bi].insts[ci];
            if (in.op != Op::Call || !in.callee) continue;
            auto it = byName.find(in.callee->name);
            if (it == byName.end()) continue;
            IRFunc* G = it->second;
            if (!multiInlinable(*G, F)) continue;
            if ((int)in.args.size() != G->numParams) continue;
            doMultiInline(F, bi, ci, *G);
            return true;
        }
    }
    return false;
}

void inlineFunctions(IRModule& mod) {
    std::unordered_map<std::string, IRFunc*> byName;
    for (auto& f : mod.funcs) byName[f.name] = &f;
    for (int pass = 0; pass < 5; pass++) {
        bool any = inlineSweep(mod, byName);
        for (auto& F : mod.funcs)
            while (inlineOneMulti(F, byName)) any = true;
        if (!any) break;
    }
}

// ---- global constant-store forwarding --------------------------------------
// Intraprocedural, dominance-respecting forwarding of globals that currently
// hold a known constant. The ToyC IR is NOT in SSA form and globals are mutable
// shared state, so we run a forward dataflow analysis over a 3-valued lattice
// per global:  Top (no info yet) > Const(c) > Bottom (varies / unknown).
//
//   StoreGlobal gid, imm  -> gid = Const(imm)
//   StoreGlobal gid, reg  -> gid = Bottom
//   Call                  -> ALL globals = Bottom (callee may write any)
//   LoadGlobal gid -> dst -> if gid is Const(c), rewrite to `Mv dst, c`
//
// The meet over predecessors is the lattice meet (Const(c) survives only if
// every predecessor agrees on the same c; disagreement -> Bottom). Crucially we
// initialise every block's OUT to Top (optimistic) and the entry IN to Bottom
// (a function may be entered with globals in any state -- always sound), then
// iterate to a fixpoint. Optimistic Top init is what lets a constant store in
// the loop preheader reach loads inside the loop body: a pessimistic empty init
// would let the back-edge's empty state intersect the header down to nothing on
// the first pass. The big win lands after inlining, when a callee's reads sit in
// the same function as the constant stores (p11: `G = k` in main, hot loop reads
// G via the inlined `compute`).
enum class CState { Top, Const, Bottom };
struct Cell {
    CState s = CState::Top;
    int32_t c = 0;
    bool operator==(const Cell& o) const {
        return s == o.s && (s != CState::Const || c == o.c);
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};
using GMap = std::map<int, Cell>;   // absent key == Top

static Cell meetCell(const Cell& a, const Cell& b) {
    if (a.s == CState::Top) return b;
    if (b.s == CState::Top) return a;
    if (a.s == CState::Bottom || b.s == CState::Bottom) return {CState::Bottom, 0};
    // both Const
    if (a.c == b.c) return a;
    return {CState::Bottom, 0};
}

// Meet of predecessors' OUT states. Absent in a map means Top, so a gid present
// in any predecessor must be met against Top (identity) for the others.
static GMap meetPreds(const std::vector<GMap>& out, const std::vector<int>& preds) {
    GMap r;
    for (int p : preds)
        for (auto& [gid, cell] : out[p]) {
            auto it = r.find(gid);
            if (it == r.end()) r[gid] = cell;       // Top ∧ cell = cell
            else it->second = meetCell(it->second, cell);
        }
    return r;
}

static void transferGlobals(const BasicBlock& bb, GMap& st) {
    for (auto& in : bb.insts) {
        if (in.op == Op::Call) {
            for (auto& [gid, cell] : st) cell = {CState::Bottom, 0};
        } else if (in.op == Op::StoreGlobal) {
            if (in.a.isImm()) st[in.gid] = {CState::Const, in.a.imm};
            else st[in.gid] = {CState::Bottom, 0};
        }
    }
}

bool globalConstProp(IRFunc& fn) {
    computeCFG(fn);
    int n = (int)fn.blocks.size();
    if (n == 0) return false;

    // Globals referenced anywhere in this function: at entry they are unknown.
    GMap entryIn;
    for (auto& bb : fn.blocks)
        for (auto& in : bb.insts)
            if (in.op == Op::LoadGlobal || in.op == Op::StoreGlobal)
                entryIn[in.gid] = {CState::Bottom, 0};
    if (entryIn.empty()) return false;

    std::vector<GMap> in(n), out(n);   // OUT starts Top (empty map) = optimistic
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 0; b < n; b++) {
            GMap ni = (b == 0) ? entryIn : meetPreds(out, fn.blocks[b].preds);
            in[b] = ni;
            transferGlobals(fn.blocks[b], ni);
            if (ni != out[b]) { out[b] = std::move(ni); changed = true; }
        }
    }

    bool any = false;
    for (int b = 0; b < n; b++) {
        GMap st = in[b];
        for (auto& inst : fn.blocks[b].insts) {
            if (inst.op == Op::LoadGlobal) {
                auto it = st.find(inst.gid);
                if (it != st.end() && it->second.s == CState::Const) {
                    inst.op = Op::Mv; inst.a = Val::I(it->second.c);
                    inst.gid = -1;
                    any = true;
                }
            } else if (inst.op == Op::Call) {
                for (auto& [gid, cell] : st) cell = {CState::Bottom, 0};
            } else if (inst.op == Op::StoreGlobal) {
                if (inst.a.isImm()) st[inst.gid] = {CState::Const, inst.a.imm};
                else st[inst.gid] = {CState::Bottom, 0};
            }
        }
    }
    return any;
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

// ---- constant folding & algebraic simplification ---------------------------
// Fold constant operands and apply algebraic identities:
//   x+0=x, 0+x=x, x-0=x, x*0=0, 0*x=0, x*1=x, 1*x=x, x/1=x, x%1=0, x-x=0
// Also fold comparisons with known constants.
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
                    switch (in.op) {
                        case Op::Add: res = av + bv; break;
                        case Op::Sub: res = av - bv; break;
                        case Op::Mul: res = av * bv; break;
                        case Op::Div: if (bv != 0) res = av / bv; else break;
                        case Op::Mod: if (bv != 0) res = av % bv; else break;
                        case Op::Lt:  res = av < bv; break;
                        case Op::Gt:  res = av > bv; break;
                        case Op::Le:  res = av <= bv; break;
                        case Op::Ge:  res = av >= bv; break;
                        case Op::Eq:  res = av == bv; break;
                        case Op::Ne:  res = av != bv; break;
                        default: break;
                    }
                    if (in.op != Op::Div && in.op != Op::Mod || bv != 0) {
                        r.op = Op::Mv; r.a = Val::I(res); r.b = Val::I(0);
                        folded = true; any = true;
                    }
                }
                // Algebraic identities (only when not both imm, already handled)
                if (!folded) {
                    bool aImm = in.a.isImm(), bImm = in.b.isImm();
                    int32_t av = in.a.imm, bv = in.b.imm;
                    switch (in.op) {
                        case Op::Add:
                            if (aImm && av == 0) { r = in; r.op = Op::Mv; r.a = in.b; folded = true; any = true; }
                            else if (bImm && bv == 0) { r = in; r.op = Op::Mv; r.a = in.a; folded = true; any = true; }
                            break;
                        case Op::Sub:
                            if (bImm && bv == 0) { r = in; r.op = Op::Mv; r.a = in.a; folded = true; any = true; }
                            if (!aImm && !bImm && in.a.tag == in.b.tag && in.a.reg == in.b.reg) {
                                r.op = Op::Mv; r.a = Val::I(0); folded = true; any = true;
                            }
                            break;
                        case Op::Mul:
                            if ((aImm && av == 0) || (bImm && bv == 0)) { r.op = Op::Mv; r.a = Val::I(0); folded = true; any = true; }
                            else if (aImm && av == 1) { r.op = Op::Mv; r.a = in.b; folded = true; any = true; }
                            else if (bImm && bv == 1) { r.op = Op::Mv; r.a = in.a; folded = true; any = true; }
                            break;
                        case Op::Div:
                            if (bImm && bv == 1) { r.op = Op::Mv; r.a = in.a; folded = true; any = true; }
                            break;
                        case Op::Mod:
                            if (bImm && bv == 1) { r.op = Op::Mv; r.a = Val::I(0); folded = true; any = true; }
                            break;
                        case Op::Eq:
                            if (!aImm && !bImm && in.a.tag == in.b.tag && in.a.reg == in.b.reg) {
                                r.op = Op::Mv; r.a = Val::I(1); folded = true; any = true;
                            }
                            break;
                        case Op::Ne:
                            if (!aImm && !bImm && in.a.tag == in.b.tag && in.a.reg == in.b.reg) {
                                r.op = Op::Mv; r.a = Val::I(0); folded = true; any = true;
                            }
                            break;
                        default: break;
                    }
                }
            }
            // Neg(0) → 0, Not(0) → 1, Not(1) → 0
            if (!folded && in.a.isImm()) {
                if (in.op == Op::Neg) { r.op = Op::Mv; r.a = Val::I(-in.a.imm); folded = true; any = true; }
                if (in.op == Op::Not) { r.op = Op::Mv; r.a = Val::I(in.a.imm == 0 ? 1 : 0); folded = true; any = true; }
            }
            if (folded) out.push_back(r);
            else out.push_back(in);
        }
        bb.insts = std::move(out);
    }
    return any;
}

// ---- strength reduction ----------------------------------------------------
// Replace expensive operations with cheaper equivalents:
//   x*2^k → x<<k, x/2^k → x>>k, x%2^k → x & (2^k-1)
//   x*3 → (x<<1)+x, etc.
bool strengthReduction(IRFunc& fn) {
    bool any = false;
    auto isPow2 = [](int32_t v) -> int {
        if (v <= 0) return -1;
        if ((v & (v - 1)) == 0) { int k = 0; while (v > 1) { v >>= 1; k++; } return k; }
        return -1;
    };
    for (auto& bb : fn.blocks) {
        std::vector<Inst> out;
        out.reserve(bb.insts.size());
        for (auto& in : bb.insts) {
            if (in.op == Op::Mul && in.b.isImm()) {
                int k = isPow2(in.b.imm);
                if (k >= 1) {
                    // x * 2^k → x << k  (use add-based shift since ToyC IR has no shift)
                    // We'll emit it as repeated adds via the codegen strength reduction
                    // For IR level, keep as Mul but mark for codegen
                    out.push_back(in);
                } else {
                    out.push_back(in);
                }
            } else {
                out.push_back(in);
            }
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
// Replace uses of a copy's destination with its source register, *within a
// basic block*. The ToyC IR is NOT in SSA form: a local variable keeps one
// vreg across reassignments (e.g. `r = r + 1` redefines r's vreg), so a copy
// `dst = src` is only valid from the Mv up to the point where either `dst` or
// `src` is next redefined. A global, kill-free map would wrongly propagate a
// stale copy past a redefinition and corrupt the program. We therefore track
// the live copy set per block and invalidate entries on every redefinition.
// DCE subsequently removes any Mv whose destination became dead.
bool copyProp(IRFunc& fn) {
    bool any = false;

    for (auto& bb : fn.blocks) {
        // copy[d] = s  means "vreg d currently holds the same value as vreg s".
        std::unordered_map<int, int> copy;

        // Drop every copy invalidated by a (re)definition of vreg `d`: both
        // copies *of* d and copies whose *source* is d.
        auto kill = [&](int d) {
            if (d < 0) return;
            copy.erase(d);
            for (auto it = copy.begin(); it != copy.end(); ) {
                if (it->second == d) it = copy.erase(it);
                else ++it;
            }
        };

        // Follow a chain of copies to its root (bounded; copies form a DAG
        // within the live set, but guard against cycles anyway).
        auto resolve = [&](int v) {
            int cur = v;
            for (int i = 0; i < 64; i++) {
                auto it = copy.find(cur);
                if (it == copy.end()) break;
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

        for (auto& in : bb.insts) {
            // Rewrite operand uses with the current copy set first.
            if (in.op == Op::Mv) {
                apply(in.a);
            } else {
                apply(in.a); apply(in.b);
                for (auto& a : in.args) apply(a);
            }

            // Then account for this instruction's definition.
            if (in.op == Op::Mv && in.a.isReg() && in.dst >= 0 && in.dst != in.a.reg) {
                kill(in.dst);              // dst is being redefined
                copy[in.dst] = in.a.reg;   // record the fresh copy
            } else {
                kill(in.dst);              // any other def kills copies on dst
            }
        }
        if (bb.term == Term::Br) apply(bb.cond);
        if (bb.term == Term::Ret && bb.retHasVal) apply(bb.retVal);
    }
    return any;
}

// ---- common subexpression elimination (CSE) --------------------------------
// Within each basic block, detect identical binary expressions and replace
// later occurrences with the earlier result.
bool cseBlock(BasicBlock& bb) {
    bool any = false;
    std::unordered_map<BinKey, int, BinKeyHash> avail;
    std::vector<Inst> out;
    out.reserve(bb.insts.size());

    auto killDef = [&](int d) {
        std::vector<BinKey> toRemove;
        for (auto& [key, reg] : avail) {
            if ((key.aTag == 1 && key.aReg == d) || (key.bTag == 1 && key.bReg == d))
                toRemove.push_back(key);
        }
        for (auto& k : toRemove) avail.erase(k);
    };

    for (auto& in : bb.insts) {
        if (isBinary(in.op) && in.a.isReg() && in.b.isReg()) {
            BinKey key = makeKey(in);
            auto it = avail.find(key);
            if (it != avail.end()) {
                Inst mv; mv.op = Op::Mv; mv.dst = in.dst; mv.a = Val::R(it->second);
                out.push_back(std::move(mv));
                killDef(in.dst);
                any = true;
                continue;
            }
        }
        if (in.dst >= 0) killDef(in.dst);
        if (isBinary(in.op) && in.a.isReg() && in.b.isReg())
            avail[makeKey(in)] = in.dst;
        out.push_back(in);
    }
    bb.insts = std::move(out);
    return any;
}

void cse(IRFunc& fn) {
    for (auto& bb : fn.blocks) cseBlock(bb);
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
                            out.push_back(std::move(mv));
                            any = true;
                            folded = true;
                        }
                        break;
                    }
                    if (out[i].dst == in.a.reg) break;
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
// Hoist instructions whose operands are all loop-invariant. Because the ToyC IR
// is NOT in SSA form, a vreg can be (re)assigned inside the loop body (e.g. the
// induction `i = i + 1` or accumulation `s = s + i`). An operand is only truly
// invariant if it is NEVER defined inside the loop -- being "also defined before
// the loop" is not enough, since the in-loop definition makes it vary per
// iteration. We therefore compute the set of vregs defined inside the loop and
// only hoist an instruction when (a) none of its register operands is defined in
// the loop, and (b) its own destination is defined exactly once in the loop (so
// moving it out cannot drop other reaching definitions).
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

            // Count how many times each vreg is defined *inside* the loop.
            std::vector<int> defCountInLoop(fn.numVregs, 0);
            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                for (auto& in : fn.blocks[b].insts)
                    if (in.dst >= 0) defCountInLoop[in.dst]++;
            }
            auto definedInLoop = [&](int reg) { return defCountInLoop[reg] > 0; };

            // An operand is invariant iff it is an immediate or a vreg never
            // (re)defined inside the loop.
            auto operandInvariant = [&](const Val& v2) {
                return !v2.isReg() || !definedInLoop(v2.reg);
            };
            auto isInvariant = [&](const Inst& in) -> bool {
                if (in.dst < 0) return false;
                if (defCountInLoop[in.dst] != 1) return false;  // single def only
                if (in.op == Op::Mv || in.op == Op::Neg || in.op == Op::Not)
                    return operandInvariant(in.a);
                if (isBinary(in.op))
                    return operandInvariant(in.a) && operandInvariant(in.b);
                return false;   // calls, loads/stores: never hoist (side effects)
            };

            // A block's instructions only run every iteration if the block
            // DOMINATES the loop latch `u` (the back-edge source). Blocks that
            // sit inside a conditional arm (if/else) execute only on some
            // iterations; hoisting from them would run the instruction
            // unconditionally and, since the IR is non-SSA, clobber a vreg whose
            // value should have been preserved on the not-taken path. Restrict
            // hoisting to latch-dominating blocks.
            for (int b = 0; b < n; b++) {
                if (!inLoop[b]) continue;
                if (!dom[b][u]) continue;          // must dominate the latch
                std::vector<Inst> kept;
                kept.reserve(fn.blocks[b].insts.size());
                for (auto& in : fn.blocks[b].insts) {
                    if (isInvariant(in)) {
                        // Once hoisted, this vreg is no longer defined in the
                        // loop; later instructions using it stay correct because
                        // the value is now computed once in the preheader.
                        defCountInLoop[in.dst] = 0;
                        fn.blocks[pre].insts.push_back(in);
                    } else {
                        kept.push_back(in);
                    }
                }
                fn.blocks[b].insts = std::move(kept);
            }
        }
    }
}

// ---- self tail-call elimination --------------------------------------------
// Rewrite `return self(args);` into a jump back to the function entry, updating
// the parameter vregs in place — turning self tail recursion into a loop and
// dropping the per-call stack-frame setup/teardown. ToyC forbids mutual
// recursion and forward calls, so only direct self-recursion can occur; we
// match it by name (function names are unique). The entry block (index 0)
// becomes the loop header: jumping to its label re-enters the body after the
// prologue, re-reading the (updated) parameter vregs. Locals are recomputed
// each iteration, which is exactly the call semantics. Because the entry block
// has no predecessor outside the loop, LICM/const-hoist (which require a single
// outside preheader) correctly skip this loop.
void tailCallElim(IRFunc& fn) {
    if (fn.blocks.empty()) return;
    bool any = false;
    for (auto& bb : fn.blocks) {
        if (bb.term != Term::Ret || !bb.retHasVal || bb.insts.empty()) continue;
        Inst& call = bb.insts.back();
        if (call.op != Op::Call || !call.callee || call.callee->name != fn.name) continue;
        if (call.dst < 0 || !bb.retVal.isReg() || bb.retVal.reg != call.dst) continue;
        if ((int)call.args.size() != fn.numParams) continue;

        // 1. Evaluate every argument into a fresh temp first, so swaps like
        //    f(b, a) don't clobber a parameter before it has been read.
        std::vector<Val> args = call.args;     // copy before we drop the Call
        std::vector<int> tmp(fn.numParams);
        bb.insts.pop_back();                   // remove the tail Call
        for (int i = 0; i < fn.numParams; i++) {
            tmp[i] = fn.numVregs++;
            Inst mv; mv.op = Op::Mv; mv.dst = tmp[i]; mv.a = args[i];
            bb.insts.push_back(std::move(mv));
        }
        // 2. Copy the temps back into the parameter vregs.
        for (int i = 0; i < fn.numParams; i++) {
            Inst mv; mv.op = Op::Mv; mv.dst = fn.paramRegs[i]; mv.a = Val::R(tmp[i]);
            bb.insts.push_back(std::move(mv));
        }
        // 3. Jump back to the entry block instead of returning.
        bb.term = Term::Jmp;
        bb.succ0 = 0;
        bb.succ1 = -1;
        bb.retHasVal = false;
        bb.retVal = Val::I(0);
        any = true;
    }
    if (any) computeCFG(fn);
}

} // namespace

void optimizeIR(IRModule& mod, bool opt) {
    if (opt) inlineFunctions(mod);
    for (auto& fn : mod.funcs) {
        if (opt) tailCallElim(fn);
        // Phase 1: constant folding + algebraic simplification
        if (opt) constFold(fn);
        deadCodeElim(fn);

        if (opt) {
            // Phase 2: structural optimisations
            globalConstProp(fn); // fold loads of constant globals to immediates
            constFold(fn);       // fold the freshly materialised constants
            cse(fn);
            copyProp(fn);
            peephole(fn);
            jumpThreading(fn);
            constFold(fn);       // fold again after jump threading
            copyProp(fn);        // propagate new Mvs
            peephole(fn);        // clean up
            globalConstProp(fn); // re-run after CFG simplification exposed more
            constFold(fn);
            deadCodeElim(fn);

            // Phase 3: loop optimisations
            licm(fn);
            hoistLoopConstants(fn);
            deadCodeElim(fn);

            unreachableElim(fn);
        }
        deadCodeElim(fn);
    }
}

} // namespace toyc
