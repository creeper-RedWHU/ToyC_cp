#include "opt/RegAlloc.h"
#include "codegen/Regs.h"
#include <algorithm>
#include <set>
#include <unordered_set>

namespace toyc {
namespace {

// Collect the vregs read by an instruction.
void instUses(const Inst& in, std::vector<int>& out) {
    auto addV = [&](const Val& v) { if (v.isReg()) out.push_back(v.reg); };
    switch (in.op) {
        case Op::Mv: case Op::Neg: case Op::Not: case Op::StoreGlobal:
            addV(in.a); break;
        case Op::LoadGlobal: break;
        case Op::Call: for (auto& a : in.args) addV(a); break;
        default: addV(in.a); addV(in.b); break;   // binary
    }
}

int instDef(const Inst& in) { return in.dst; }   // -1 when none

struct LivenessInfo {
    std::vector<std::vector<char>> liveIn, liveOut;
};

} // namespace

Allocation allocateRegisters(const IRFunc& fn) {
    const int nb = (int)fn.blocks.size();
    const int nv = fn.numVregs;

    Allocation A;
    A.reg.assign(nv, -1);
    A.spillSlot.assign(nv, -1);

    // ----- per-block use/def -------------------------------------------------
    auto termUses = [&](const BasicBlock& bb, std::vector<int>& out) {
        if (bb.term == Term::Br && bb.cond.isReg()) out.push_back(bb.cond.reg);
        if (bb.term == Term::Ret && bb.retHasVal && bb.retVal.isReg()) out.push_back(bb.retVal.reg);
    };

    std::vector<std::vector<char>> useB(nb, std::vector<char>(nv, 0));
    std::vector<std::vector<char>> defB(nb, std::vector<char>(nv, 0));
    for (int b = 0; b < nb; b++) {
        const auto& bb = fn.blocks[b];
        std::vector<char> defined(nv, 0);
        auto useV = [&](int v) { if (v >= 0 && !defined[v]) useB[b][v] = 1; };
        std::vector<int> u;
        for (auto& in : bb.insts) {
            u.clear(); instUses(in, u);
            for (int v : u) useV(v);
            int d = instDef(in);
            if (d >= 0) { defB[b][d] = 1; defined[d] = 1; }
        }
        u.clear(); termUses(bb, u);
        for (int v : u) useV(v);
    }

    // ----- iterative liveness ------------------------------------------------
    LivenessInfo L;
    L.liveIn.assign(nb, std::vector<char>(nv, 0));
    L.liveOut.assign(nb, std::vector<char>(nv, 0));
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = nb - 1; b >= 0; b--) {
            const auto& bb = fn.blocks[b];
            std::vector<char> out(nv, 0);
            int s[2], n; succsOf(bb, s, n);
            for (int i = 0; i < n; i++)
                for (int v = 0; v < nv; v++) out[v] |= L.liveIn[s[i]][v];
            // in = use ∪ (out − def)
            std::vector<char> in(nv, 0);
            for (int v = 0; v < nv; v++)
                in[v] = useB[b][v] || (out[v] && !defB[b][v]);
            if (out != L.liveOut[b]) { L.liveOut[b] = out; changed = true; }
            if (in != L.liveIn[b])   { L.liveIn[b] = in;  changed = true; }
        }
    }

    // ----- interference graph + call-crossing + move pairs -------------------
    std::vector<std::unordered_set<int>> adj(nv);
    std::vector<char> callCross(nv, 0);
    std::vector<std::vector<int>> movePartner(nv);
    auto addInterf = [&](int a, int b) {
        if (a == b) return;
        adj[a].insert(b); adj[b].insert(a);
    };

    A.isLeaf = true;
    for (int b = 0; b < nb; b++) {
        const auto& bb = fn.blocks[b];
        std::set<int> live;
        for (int v = 0; v < nv; v++) if (L.liveOut[b][v]) live.insert(v);
        std::vector<int> tu; termUses(bb, tu);
        for (int v : tu) live.insert(v);

        for (int i = (int)bb.insts.size() - 1; i >= 0; i--) {
            const Inst& in = bb.insts[i];
            int d = instDef(in);
            bool isMove = (in.op == Op::Mv && in.a.isReg());
            if (d >= 0) {
                for (int v : live) {
                    if (v == d) continue;
                    if (isMove && v == in.a.reg) continue;   // allow coalescing
                    addInterf(d, v);
                }
            }
            if (in.op == Op::Call) {
                A.isLeaf = false;
                for (int v : live) if (v != d) callCross[v] = 1;
            }
            if (isMove) { movePartner[d].push_back(in.a.reg); movePartner[in.a.reg].push_back(d); }
            if (d >= 0) live.erase(d);
            std::vector<int> u; instUses(in, u);
            for (int v : u) live.insert(v);
        }
    }

    // Parameters are conceptually defined together at function entry, so any two
    // simultaneously-live parameters interfere. They are never the target of an
    // IR instruction, so the def-based rule above misses them; add them here.
    for (size_t i = 0; i < fn.paramRegs.size(); i++) {
        for (size_t j = i + 1; j < fn.paramRegs.size(); j++)
            addInterf(fn.paramRegs[i], fn.paramRegs[j]);
        for (int v = 0; v < nv; v++)
            if (L.liveIn[0][v]) addInterf(fn.paramRegs[i], v);
    }

    // ----- greedy colouring with move bias -----------------------------------
    // Compute total use count per vreg — spill cost heuristic: vregs used more
    // often are more expensive to spill, so they get priority in the order.
    std::vector<int> useTotal(nv, 0);
    for (int b = 0; b < nb; b++)
        for (int v = 0; v < nv; v++)
            if (useB[b][v]) useTotal[v]++;

    std::vector<int> order(nv);
    for (int i = 0; i < nv; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        // Primary: more interference edges → higher priority
        // Secondary: more total uses → higher spill cost → higher priority
        size_t da = adj[a].size(), db = adj[b].size();
        if (da != db) return da > db;
        return useTotal[a] > useTotal[b];
    });

    const auto& caller = callerSavedPool();
    const auto& callee = calleeSavedPool();

    for (int v : order) {
        // build the allowed colour list (preference-ordered)
        std::vector<int> allowed;
        if (callCross[v]) {
            allowed = callee;                      // must survive calls
        } else {
            allowed = caller;
            allowed.insert(allowed.end(), callee.begin(), callee.end());
        }
        std::unordered_set<int> used;
        for (int w : adj[v]) if (A.reg[w] >= 0) used.insert(A.reg[w]);

        int pick = -1;
        // bias: try to share a colour with a move partner
        for (int p : movePartner[v]) {
            if (A.reg[p] >= 0 && !used.count(A.reg[p]) &&
                std::find(allowed.begin(), allowed.end(), A.reg[p]) != allowed.end()) {
                pick = A.reg[p]; break;
            }
        }
        if (pick < 0)
            for (int c : allowed) if (!used.count(c)) { pick = c; break; }

        if (pick < 0) { A.spillSlot[v] = A.numSpills++; }   // spill
        else A.reg[v] = pick;
    }

    std::set<int> usedCallee;
    for (int v = 0; v < nv; v++)
        if (A.reg[v] >= 0 && isCalleeSaved(A.reg[v])) usedCallee.insert(A.reg[v]);
    A.usedCallee.assign(usedCallee.begin(), usedCallee.end());

    return A;
}

} // namespace toyc
