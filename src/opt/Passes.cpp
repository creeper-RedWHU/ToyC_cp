#include "opt/Passes.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace toyc {
namespace {

bool isCmp(Op op) { return op >= Op::Lt && op <= Op::Ne; }

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

} // namespace

void optimizeIR(IRModule& mod, bool opt) {
    if (opt) inlineFunctions(mod);
    for (auto& fn : mod.funcs) {
        deadCodeElim(fn);
        if (opt) hoistLoopConstants(fn);
        deadCodeElim(fn);
    }
}

} // namespace toyc
