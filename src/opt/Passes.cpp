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

// ---- algebraic simplification ------------------------------------------------
// Simplify common algebraic identities at the IR level:
//   x + 0 = x,  x - 0 = x,  0 - x = -x, x - x = 0
//   x * 0 = 0,  x * 1 = x,  x * -1 = -x
//   x / 1 = x,  x / -1 = -x
//   x % 1 = 0,  x % -1 = 0
//   Neg(Neg(x)) = x
//   Ge/Le/Eq(x, x) = 1,  Lt/Gt/Ne(x, x) = 0
void algebraicSimplify(IRFunc& fn) {
    for (auto& bb : fn.blocks) {
        std::unordered_map<int, Op> defOp;
        std::unordered_map<int, Val> defA;

        for (auto& in : bb.insts) {
            bool sameReg = in.a.isReg() && in.b.isReg() && in.a.reg == in.b.reg;

            switch (in.op) {
                case Op::Add:
                    if (in.b.isImm() && in.b.imm == 0) {
                        in.op = Op::Mv; in.b = Val::I(0);
                    } else if (in.a.isImm() && in.a.imm == 0) {
                        in.op = Op::Mv; in.a = in.b; in.b = Val::I(0);
                    }
                    break;
                case Op::Sub:
                    if (in.b.isImm() && in.b.imm == 0) {
                        in.op = Op::Mv; in.b = Val::I(0);
                    } else if (in.a.isImm() && in.a.imm == 0) {
                        in.op = Op::Neg; in.a = in.b; in.b = Val::I(0);
                    } else if (sameReg) {
                        in.op = Op::Mv; in.a = Val::I(0); in.b = Val::I(0);
                    }
                    break;
                case Op::Mul:
                    if ((in.a.isImm() && in.a.imm == 0) || (in.b.isImm() && in.b.imm == 0)) {
                        in.op = Op::Mv; in.a = Val::I(0); in.b = Val::I(0);
                    } else if (in.b.isImm() && in.b.imm == 1) {
                        in.op = Op::Mv; in.b = Val::I(0);
                    } else if (in.a.isImm() && in.a.imm == 1) {
                        in.op = Op::Mv; in.a = in.b; in.b = Val::I(0);
                    } else if (in.b.isImm() && in.b.imm == -1) {
                        in.op = Op::Neg; in.b = Val::I(0);
                    } else if (in.a.isImm() && in.a.imm == -1) {
                        in.op = Op::Neg; in.a = in.b; in.b = Val::I(0);
                    }
                    break;
                case Op::Div:
                    if (in.b.isImm() && in.b.imm == 1) {
                        in.op = Op::Mv; in.b = Val::I(0);
                    } else if (in.b.isImm() && in.b.imm == -1) {
                        in.op = Op::Neg; in.b = Val::I(0);
                    }
                    break;
                case Op::Mod:
                    if (in.b.isImm() && (in.b.imm == 1 || in.b.imm == -1)) {
                        in.op = Op::Mv; in.a = Val::I(0); in.b = Val::I(0);
                    }
                    break;
                case Op::Lt: case Op::Gt:
                    if (sameReg) { in.op = Op::Mv; in.a = Val::I(0); in.b = Val::I(0); }
                    break;
                case Op::Ge: case Op::Le: case Op::Eq:
                    if (sameReg) { in.op = Op::Mv; in.a = Val::I(1); in.b = Val::I(0); }
                    break;
                case Op::Ne:
                    if (sameReg) { in.op = Op::Mv; in.a = Val::I(0); in.b = Val::I(0); }
                    break;
                case Op::Neg:
                    if (in.a.isReg()) {
                        auto it = defOp.find(in.a.reg);
                        if (it != defOp.end() && it->second == Op::Neg) {
                            in.op = Op::Mv; in.a = defA[in.a.reg]; in.b = Val::I(0);
                        }
                    }
                    break;
                default: break;
            }

            if (in.dst >= 0) {
                defOp[in.dst] = in.op;
                defA[in.dst] = in.a;
            }
        }
    }
}

// ---- copy propagation -------------------------------------------------------
// Forward-substitute copies: if `rd = mv rs` (both virtual regs), subsequent
// uses of `rd` in the same block are replaced with `rs`. Handles chains
// (r3=mv r2; r2=mv r1 -> r3->r1) and invalidates when a copy source is
// redefined.
void copyPropagation(IRFunc& fn) {
    for (auto& bb : fn.blocks) {
        std::unordered_map<int, int> copyOf;

        auto resolve = [&](int r) -> int {
            auto it = copyOf.find(r);
            if (it == copyOf.end()) return r;
            while (it != copyOf.end()) { r = it->second; it = copyOf.find(r); }
            return r;
        };

        for (auto& in : bb.insts) {
            auto rep = [&](Val& v) {
                if (!v.isReg()) return;
                int r = resolve(v.reg);
                if (r != v.reg) v.reg = r;
            };
            switch (in.op) {
                case Op::Mv: case Op::Neg: case Op::Not: case Op::StoreGlobal:
                    rep(in.a); break;
                case Op::LoadGlobal: break;
                case Op::Call: for (auto& a : in.args) rep(a); break;
                default: rep(in.a); rep(in.b); break;
            }

            if (in.op == Op::Mv && in.a.isReg()) {
                copyOf[in.dst] = resolve(in.a.reg);
            } else if (in.dst >= 0) {
                copyOf.erase(in.dst);
                std::vector<int> stale;
                for (auto& [k, v] : copyOf)
                    if (v == in.dst) stale.push_back(k);
                for (int k : stale) copyOf.erase(k);
            }
        }

        if (bb.term == Term::Br && bb.cond.isReg())
            bb.cond.reg = resolve(bb.cond.reg);
        if (bb.term == Term::Ret && bb.retHasVal && bb.retVal.isReg())
            bb.retVal.reg = resolve(bb.retVal.reg);
    }
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
        for (auto& [key, reg] : avail)
            if ((key.aTag == 1 && key.aReg == d) || (key.bTag == 1 && key.bReg == d))
                toRemove.push_back(key);
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
// Local patterns: Mv x,x -> remove; Neg(Neg(x)) -> x; Not(Not(x)) -> x.
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

} // namespace

void optimizeIR(IRModule& mod, bool opt) {
    if (opt) inlineFunctions(mod);
    for (auto& fn : mod.funcs) {
        algebraicSimplify(fn);
        copyPropagation(fn);
        deadCodeElim(fn);
        if (opt) {
            cse(fn);
            peephole(fn);
            licm(fn);
            hoistLoopConstants(fn);
        }
        deadCodeElim(fn);
    }
}

} // namespace toyc