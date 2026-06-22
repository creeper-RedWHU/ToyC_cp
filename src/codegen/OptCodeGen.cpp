#include "codegen/CodeGen.h"
#include "codegen/Regs.h"
#include "opt/RegAlloc.h"
#include <algorithm>
#include <sstream>
#include <vector>

namespace toyc {
namespace {

bool fits12(long x) { return x >= -2048 && x <= 2047; }
int align16(int x) { return (x + 15) & ~15; }
bool isCmp(Op op) { return op >= Op::Lt && op <= Op::Ne; }

Op invertCmp(Op op) {
    switch (op) {
        case Op::Lt: return Op::Ge; case Op::Ge: return Op::Lt;
        case Op::Gt: return Op::Le; case Op::Le: return Op::Gt;
        case Op::Eq: return Op::Ne; case Op::Ne: return Op::Eq;
        default: return op;
    }
}

class OptCodeGen {
public:
    OptCodeGen(const IRModule& m, bool opt) : mod_(m), opt_(opt) {}

    std::string run() {
        emitData();
        out_ << "  .text\n";
        for (auto& f : mod_.funcs) emitFunc(f);
        return out_.str();
    }

private:
    const IRModule& mod_;
    bool opt_;
    std::ostringstream out_;

    // per-function state
    const IRFunc* fn_ = nullptr;
    Allocation A_;
    int outArgArea_ = 0, spillBase_ = 0, calleeBase_ = 0, raOff_ = 0, frameSize_ = 0;
    std::vector<int> useCount_;

    int spillOff(int v) const { return spillBase_ + A_.spillSlot[v] * 4; }

    // ---- raw memory access (sp-relative), a7 as address scratch for big offs --
    void loadOff(int reg, int off) {
        if (fits12(off)) out_ << "  lw " << regName(reg) << ", " << off << "(sp)\n";
        else { out_ << "  li a7, " << off << "\n  add a7, sp, a7\n  lw " << regName(reg) << ", 0(a7)\n"; }
    }
    void storeOff(int reg, int off) {
        if (fits12(off)) out_ << "  sw " << regName(reg) << ", " << off << "(sp)\n";
        else { out_ << "  li a7, " << off << "\n  add a7, sp, a7\n  sw " << regName(reg) << ", 0(a7)\n"; }
    }
    void loadSpill(int reg, int v) { loadOff(reg, spillOff(v)); }
    void storeSpill(int reg, int v) { storeOff(reg, spillOff(v)); }

    // get a value into some register, possibly `scratch`; returns the register
    int toReg(const Val& v, int scratch) {
        if (v.isImm()) {
            if (v.imm == 0) return X_ZERO;
            out_ << "  li " << regName(scratch) << ", " << v.imm << "\n";
            return scratch;
        }
        int p = A_.reg[v.reg];
        if (p >= 0) return p;
        loadSpill(scratch, v.reg);
        return scratch;
    }

    // place a value into a specific register
    void materialize(const Val& v, int r) {
        if (v.isImm()) { out_ << "  li " << regName(r) << ", " << v.imm << "\n"; return; }
        int p = A_.reg[v.reg];
        if (p >= 0) { if (p != r) out_ << "  mv " << regName(r) << ", " << regName(p) << "\n"; }
        else loadSpill(r, v.reg);
    }

    int dstReg(int v, int scratch) { int p = A_.reg[v]; return p >= 0 ? p : scratch; }
    void writeBack(int v, int r) { if (A_.reg[v] < 0) storeSpill(r, v); }

    // ------------------------------------------------------------------------
    void emitData() {
        if (mod_.globals.empty()) return;
        out_ << "  .data\n";
        for (auto& g : mod_.globals) {
            out_ << "  .globl " << g.name << "\n  .p2align 2\n" << g.name << ":\n  .word " << g.init << "\n";
        }
    }

    std::string label(int bb) const { return ".L" + fn_->name + "_" + std::to_string(bb); }

    void computeUseCounts() {
        useCount_.assign(fn_->numVregs, 0);
        std::vector<int> u;
        for (auto& bb : fn_->blocks) {
            for (auto& in : bb.insts) {
                u.clear();
                auto add = [&](const Val& v) { if (v.isReg()) useCount_[v.reg]++; };
                switch (in.op) {
                    case Op::Mv: case Op::Neg: case Op::Not: case Op::StoreGlobal: add(in.a); break;
                    case Op::LoadGlobal: break;
                    case Op::Call: for (auto& a : in.args) add(a); break;
                    default: add(in.a); add(in.b); break;
                }
            }
            if (bb.term == Term::Br && bb.cond.isReg()) useCount_[bb.cond.reg]++;
            if (bb.term == Term::Ret && bb.retHasVal && bb.retVal.isReg()) useCount_[bb.retVal.reg]++;
        }
    }

    void emitFunc(const IRFunc& f) {
        fn_ = &f;
        A_ = allocateRegisters(f);
        computeUseCounts();

        int maxStackArgs = 0;
        for (auto& bb : f.blocks)
            for (auto& in : bb.insts)
                if (in.op == Op::Call)
                    maxStackArgs = std::max(maxStackArgs, (int)in.args.size() - 8);

        outArgArea_ = maxStackArgs * 4;
        spillBase_ = outArgArea_;
        calleeBase_ = spillBase_ + A_.numSpills * 4;
        int nCallee = (int)A_.usedCallee.size();
        raOff_ = calleeBase_ + nCallee * 4;
        int raw = raOff_ + (A_.isLeaf ? 0 : 4);
        frameSize_ = align16(raw);

        out_ << "  .globl " << f.name << "\n  .p2align 1\n" << f.name << ":\n";

        // prologue
        if (frameSize_ > 0) {
            if (fits12(-frameSize_)) out_ << "  addi sp, sp, " << -frameSize_ << "\n";
            else out_ << "  li t6, " << frameSize_ << "\n  sub sp, sp, t6\n";
        }
        if (!A_.isLeaf) storeOff(X_RA, raOff_);
        for (int i = 0; i < nCallee; i++) storeOff(A_.usedCallee[i], calleeBase_ + i * 4);

        // move incoming parameters into their assigned locations
        for (int i = 0; i < f.numParams; i++) {
            int v = f.paramRegs[i];
            int src = (i < 8) ? (X_A0 + i) : -1;
            if (i < 8) {
                int p = A_.reg[v];
                if (p >= 0) { if (p != src) out_ << "  mv " << regName(p) << ", " << regName(src) << "\n"; }
                else storeSpill(src, v);
            } else {
                int incoming = frameSize_ + (i - 8) * 4;
                int p = A_.reg[v];
                if (p >= 0) loadOff(p, incoming);
                else { loadOff(SCRATCH0, incoming); storeSpill(SCRATCH0, v); }
            }
        }

        // reachable blocks in index order
        std::vector<char> reach(f.blocks.size(), 0);
        std::vector<int> stack = {0};
        reach[0] = 1;
        while (!stack.empty()) {
            int b = stack.back(); stack.pop_back();
            int s[2], n; succsOf(f.blocks[b], s, n);
            for (int i = 0; i < n; i++) if (s[i] >= 0 && !reach[s[i]]) { reach[s[i]] = 1; stack.push_back(s[i]); }
        }
        std::vector<int> order;
        for (int b = 0; b < (int)f.blocks.size(); b++) if (reach[b]) order.push_back(b);

        for (size_t idx = 0; idx < order.size(); idx++) {
            int b = order[idx];
            int next = (idx + 1 < order.size()) ? order[idx + 1] : -1;
            emitBlock(f.blocks[b], next);
        }
        out_ << "\n";
    }

    void emitBlock(const BasicBlock& bb, int next) {
        out_ << label(bb.id) << ":\n";

        // detect a fusible comparison feeding the terminating Br
        const Inst* fused = nullptr;
        if (bb.term == Term::Br && !bb.insts.empty()) {
            const Inst& last = bb.insts.back();
            if (isCmp(last.op) && last.dst == bb.cond.reg && useCount_[last.dst] == 1)
                fused = &last;
        }
        for (size_t i = 0; i < bb.insts.size(); i++) {
            if (fused && i == bb.insts.size() - 1) break;
            emitInst(bb.insts[i]);
        }
        emitTerm(bb, next, fused);
    }

    void emitEpilogue() {
        int nCallee = (int)A_.usedCallee.size();
        for (int i = 0; i < nCallee; i++) loadOff(A_.usedCallee[i], calleeBase_ + i * 4);
        if (!A_.isLeaf) loadOff(X_RA, raOff_);
        if (frameSize_ > 0) {
            if (fits12(frameSize_)) out_ << "  addi sp, sp, " << frameSize_ << "\n";
            else out_ << "  li t6, " << frameSize_ << "\n  add sp, sp, t6\n";
        }
        out_ << "  ret\n";
    }

    // branch taken iff (op)[XOR negate] holds, comparing valA, valB
    void emitBranchCmp(Op op, const Val& a, const Val& b, const std::string& lbl, bool negate) {
        int ra = toReg(a, SCRATCH0);
        int rb = toReg(b, SCRATCH1);
        Op e = negate ? invertCmp(op) : op;
        switch (e) {
            case Op::Lt: out_ << "  blt " << regName(ra) << ", " << regName(rb) << ", " << lbl << "\n"; break;
            case Op::Ge: out_ << "  bge " << regName(ra) << ", " << regName(rb) << ", " << lbl << "\n"; break;
            case Op::Gt: out_ << "  blt " << regName(rb) << ", " << regName(ra) << ", " << lbl << "\n"; break;
            case Op::Le: out_ << "  bge " << regName(rb) << ", " << regName(ra) << ", " << lbl << "\n"; break;
            case Op::Eq: out_ << "  beq " << regName(ra) << ", " << regName(rb) << ", " << lbl << "\n"; break;
            case Op::Ne: out_ << "  bne " << regName(ra) << ", " << regName(rb) << ", " << lbl << "\n"; break;
            default: break;
        }
    }

    void emitTerm(const BasicBlock& bb, int next, const Inst* fused) {
        switch (bb.term) {
            case Term::Jmp:
                if (bb.succ0 != next) out_ << "  j " << label(bb.succ0) << "\n";
                break;
            case Term::Br: {
                int T = bb.succ0, F = bb.succ1;
                if (fused) {
                    if (F == next) emitBranchCmp(fused->op, fused->a, fused->b, label(T), false);
                    else if (T == next) emitBranchCmp(fused->op, fused->a, fused->b, label(F), true);
                    else { emitBranchCmp(fused->op, fused->a, fused->b, label(T), false); out_ << "  j " << label(F) << "\n"; }
                } else {
                    int rc = toReg(bb.cond, SCRATCH0);
                    if (F == next) out_ << "  bnez " << regName(rc) << ", " << label(T) << "\n";
                    else if (T == next) out_ << "  beqz " << regName(rc) << ", " << label(F) << "\n";
                    else { out_ << "  bnez " << regName(rc) << ", " << label(T) << "\n  j " << label(F) << "\n"; }
                }
                break;
            }
            case Term::Ret:
                if (bb.retHasVal) materialize(bb.retVal, X_A0);
                emitEpilogue();
                break;
            case Term::None:
                emitEpilogue();
                break;
        }
    }

    void emitInst(const Inst& in) {
        switch (in.op) {
            case Op::Mv: {
                if (A_.reg[in.dst] >= 0) materialize(in.a, A_.reg[in.dst]);
                else { materialize(in.a, SCRATCH0); storeSpill(SCRATCH0, in.dst); }
                break;
            }
            case Op::LoadGlobal: {
                int rd = dstReg(in.dst, SCRATCH0);
                out_ << "  lui " << regName(rd) << ", %hi(" << mod_.globals[in.gid].name << ")\n";
                out_ << "  lw " << regName(rd) << ", %lo(" << mod_.globals[in.gid].name << ")(" << regName(rd) << ")\n";
                writeBack(in.dst, rd);
                break;
            }
            case Op::StoreGlobal: {
                int ra = toReg(in.a, SCRATCH0);
                out_ << "  lui " << regName(SCRATCH1) << ", %hi(" << mod_.globals[in.gid].name << ")\n";
                out_ << "  sw " << regName(ra) << ", %lo(" << mod_.globals[in.gid].name << ")(" << regName(SCRATCH1) << ")\n";
                break;
            }
            case Op::Neg: {
                int ra = toReg(in.a, SCRATCH0); int rd = dstReg(in.dst, SCRATCH0);
                out_ << "  neg " << regName(rd) << ", " << regName(ra) << "\n";
                writeBack(in.dst, rd);
                break;
            }
            case Op::Not: {
                int ra = toReg(in.a, SCRATCH0); int rd = dstReg(in.dst, SCRATCH0);
                out_ << "  seqz " << regName(rd) << ", " << regName(ra) << "\n";
                writeBack(in.dst, rd);
                break;
            }
            case Op::Call: emitCall(in); break;
            default: emitBinary(in); break;
        }
    }

    void emitBinary(const Inst& in) {
        int dst = in.dst;
        Op op = in.op;

        // immediate fast paths
        if (op == Op::Add) {
            if (in.b.isImm() && fits12(in.b.imm)) { int ra = toReg(in.a, SCRATCH0); int rd = dstReg(dst, SCRATCH0);
                out_ << "  addi " << regName(rd) << ", " << regName(ra) << ", " << in.b.imm << "\n"; writeBack(dst, rd); return; }
            if (in.a.isImm() && fits12(in.a.imm)) { int rb = toReg(in.b, SCRATCH0); int rd = dstReg(dst, SCRATCH0);
                out_ << "  addi " << regName(rd) << ", " << regName(rb) << ", " << in.a.imm << "\n"; writeBack(dst, rd); return; }
        }
        if (op == Op::Sub && in.b.isImm() && fits12(-(long)in.b.imm)) {
            int ra = toReg(in.a, SCRATCH0); int rd = dstReg(dst, SCRATCH0);
            out_ << "  addi " << regName(rd) << ", " << regName(ra) << ", " << (-in.b.imm) << "\n"; writeBack(dst, rd); return;
        }
        if (op == Op::Lt && in.b.isImm() && fits12(in.b.imm)) {
            int ra = toReg(in.a, SCRATCH0); int rd = dstReg(dst, SCRATCH0);
            out_ << "  slti " << regName(rd) << ", " << regName(ra) << ", " << in.b.imm << "\n"; writeBack(dst, rd); return;
        }
        if ((op == Op::Eq || op == Op::Ne) && in.b.isImm() && in.b.imm == 0) {
            int ra = toReg(in.a, SCRATCH0); int rd = dstReg(dst, SCRATCH0);
            out_ << (op == Op::Eq ? "  seqz " : "  snez ") << regName(rd) << ", " << regName(ra) << "\n"; writeBack(dst, rd); return;
        }

        int ra = toReg(in.a, SCRATCH0);
        int rb = toReg(in.b, SCRATCH1);
        int rd = dstReg(dst, SCRATCH0);
        std::string A = regName(ra), B = regName(rb), D = regName(rd);
        switch (op) {
            case Op::Add: out_ << "  add " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Sub: out_ << "  sub " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Mul: out_ << "  mul " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Div: out_ << "  div " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Mod: out_ << "  rem " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Lt:  out_ << "  slt " << D << ", " << A << ", " << B << "\n"; break;
            case Op::Gt:  out_ << "  slt " << D << ", " << B << ", " << A << "\n"; break;
            case Op::Le:  out_ << "  slt " << D << ", " << B << ", " << A << "\n  xori " << D << ", " << D << ", 1\n"; break;
            case Op::Ge:  out_ << "  slt " << D << ", " << A << ", " << B << "\n  xori " << D << ", " << D << ", 1\n"; break;
            case Op::Eq:  out_ << "  sub " << D << ", " << A << ", " << B << "\n  seqz " << D << ", " << D << "\n"; break;
            case Op::Ne:  out_ << "  sub " << D << ", " << A << ", " << B << "\n  snez " << D << ", " << D << "\n"; break;
            default: break;
        }
        writeBack(dst, rd);
    }

    void emitCall(const Inst& in) {
        int n = (int)in.args.size();
        for (int i = n - 1; i >= 8; i--) { materialize(in.args[i], SCRATCH0); storeOff(SCRATCH0, (i - 8) * 4); }
        for (int i = 0; i < n && i < 8; i++) materialize(in.args[i], X_A0 + i);
        out_ << "  call " << in.callee->name << "\n";
        if (in.dst >= 0) {
            int p = A_.reg[in.dst];
            if (p >= 0) { if (p != X_A0) out_ << "  mv " << regName(p) << ", a0\n"; }
            else storeSpill(X_A0, in.dst);
        }
    }
};

} // namespace

std::string generateAsmOpt(const IRModule& mod, bool opt) {
    OptCodeGen g(mod, opt);
    return g.run();
}

} // namespace toyc
