#include "codegen/CodeGen.h"
#include <sstream>

namespace toyc {
namespace {

bool fits12(long x) { return x >= -2048 && x <= 2047; }
int align16(int x) { return (x + 15) & ~15; }

// ----------------------------------------------------------------------------
// Baseline code generator: every virtual register lives in a stack slot.
// Within a single IR instruction we use t0/t1/t2 as scratch and t6 to
// materialize large frame offsets. Nothing is kept in registers across
// instructions, so this is trivially correct (and slow — see the -opt path).
// ----------------------------------------------------------------------------
class NaiveCodeGen {
public:
    explicit NaiveCodeGen(const IRModule& m) : mod_(m) {}

    std::string run() {
        emitData();
        out_ << "  .text\n";
        for (auto& f : mod_.funcs) emitFunc(f);
        return out_.str();
    }

private:
    const IRModule& mod_;
    std::ostringstream out_;

    // per-function frame info
    int outArgArea_ = 0;
    int numVregs_ = 0;
    int raOff_ = 0;
    int frameSize_ = 0;

    int slotOff(int v) const { return outArgArea_ + v * 4; }

    void emitData() {
        if (mod_.globals.empty()) return;
        out_ << "  .data\n";
        for (auto& g : mod_.globals) {
            out_ << "  .globl " << g.name << "\n";
            out_ << "  .p2align 2\n";
            out_ << g.name << ":\n";
            out_ << "  .word " << g.init << "\n";
        }
    }

    // memory access with arbitrary sp-relative offset
    void loadOff(const char* reg, int off) {
        if (fits12(off)) out_ << "  lw " << reg << ", " << off << "(sp)\n";
        else { out_ << "  li t6, " << off << "\n  add t6, sp, t6\n  lw " << reg << ", 0(t6)\n"; }
    }
    void storeOff(const char* reg, int off) {
        if (fits12(off)) out_ << "  sw " << reg << ", " << off << "(sp)\n";
        else { out_ << "  li t6, " << off << "\n  add t6, sp, t6\n  sw " << reg << ", 0(t6)\n"; }
    }
    void loadSlot(const char* reg, int v) { loadOff(reg, slotOff(v)); }
    void storeSlot(const char* reg, int v) { storeOff(reg, slotOff(v)); }

    void loadVal(const Val& val, const char* reg) {
        if (val.isImm()) out_ << "  li " << reg << ", " << val.imm << "\n";
        else loadSlot(reg, val.reg);
    }

    void emitFunc(const IRFunc& f) {
        // Frame layout (sp = lowest):
        //   [outgoing stack-arg area][vreg slots][saved ra]
        int maxStackArgs = 0;
        for (auto& bb : f.blocks)
            for (auto& in : bb.insts)
                if (in.op == Op::Call)
                    maxStackArgs = std::max(maxStackArgs, (int)in.args.size() - 8);
        outArgArea_ = maxStackArgs * 4;
        numVregs_ = f.numVregs;
        raOff_ = outArgArea_ + numVregs_ * 4;
        frameSize_ = align16(raOff_ + 4);

        out_ << "  .globl " << f.name << "\n";
        out_ << "  .p2align 1\n";
        out_ << f.name << ":\n";

        // prologue
        if (fits12(-frameSize_)) out_ << "  addi sp, sp, " << -frameSize_ << "\n";
        else out_ << "  li t6, " << frameSize_ << "\n  sub sp, sp, t6\n";
        storeOff("ra", raOff_);

        // move incoming parameters into their slots
        for (int i = 0; i < f.numParams; i++) {
            int v = f.paramRegs[i];
            if (i < 8) {
                std::string ar = "a" + std::to_string(i);
                storeSlot(ar.c_str(), v);
            } else {
                loadOff("t0", frameSize_ + (i - 8) * 4);
                storeSlot("t0", v);
            }
        }

        for (auto& bb : f.blocks) emitBlock(f, bb);
        out_ << "\n";
    }

    std::string label(const IRFunc& f, int bb) const { return ".L" + f.name + "_" + std::to_string(bb); }

    void emitBlock(const IRFunc& f, const BasicBlock& bb) {
        out_ << label(f, bb.id) << ":\n";
        for (auto& in : bb.insts) emitInst(in);
        emitTerm(f, bb);
    }

    void emitEpilogue(const IRFunc& f) {
        loadOff("ra", raOff_);
        if (fits12(frameSize_)) out_ << "  addi sp, sp, " << frameSize_ << "\n";
        else out_ << "  li t6, " << frameSize_ << "\n  add sp, sp, t6\n";
        out_ << "  ret\n";
    }

    void emitTerm(const IRFunc& f, const BasicBlock& bb) {
        switch (bb.term) {
            case Term::Jmp:
                out_ << "  j " << label(f, bb.succ0) << "\n";
                break;
            case Term::Br: {
                loadVal(bb.cond, "t0");
                out_ << "  bnez t0, " << label(f, bb.succ0) << "\n";
                out_ << "  j " << label(f, bb.succ1) << "\n";
                break;
            }
            case Term::Ret:
                if (bb.retHasVal) loadVal(bb.retVal, "a0");
                emitEpilogue(f);
                break;
            case Term::None:
                emitEpilogue(f);   // should not happen; be safe
                break;
        }
    }

    void emitInst(const Inst& in) {
        switch (in.op) {
            case Op::Mv:
                loadVal(in.a, "t0"); storeSlot("t0", in.dst); break;
            case Op::LoadGlobal:
                out_ << "  lui t0, %hi(" << mod_.globals[in.gid].name << ")\n";
                out_ << "  lw t0, %lo(" << mod_.globals[in.gid].name << ")(t0)\n";
                storeSlot("t0", in.dst); break;
            case Op::StoreGlobal:
                loadVal(in.a, "t0");
                out_ << "  lui t1, %hi(" << mod_.globals[in.gid].name << ")\n";
                out_ << "  sw t0, %lo(" << mod_.globals[in.gid].name << ")(t1)\n";
                break;
            case Op::Neg:
                loadVal(in.a, "t0"); out_ << "  neg t0, t0\n"; storeSlot("t0", in.dst); break;
            case Op::Not:
                loadVal(in.a, "t0"); out_ << "  seqz t0, t0\n"; storeSlot("t0", in.dst); break;
            case Op::Call: emitCall(in); break;
            default: emitBinary(in); break;
        }
    }

    void emitBinary(const Inst& in) {
        loadVal(in.a, "t0");
        loadVal(in.b, "t1");
        switch (in.op) {
            case Op::Add: out_ << "  add t0, t0, t1\n"; break;
            case Op::Sub: out_ << "  sub t0, t0, t1\n"; break;
            case Op::Mul: out_ << "  mul t0, t0, t1\n"; break;
            case Op::Div: out_ << "  div t0, t0, t1\n"; break;
            case Op::Mod: out_ << "  rem t0, t0, t1\n"; break;
            case Op::Lt:  out_ << "  slt t0, t0, t1\n"; break;
            case Op::Gt:  out_ << "  slt t0, t1, t0\n"; break;
            case Op::Le:  out_ << "  slt t0, t1, t0\n  xori t0, t0, 1\n"; break;
            case Op::Ge:  out_ << "  slt t0, t0, t1\n  xori t0, t0, 1\n"; break;
            case Op::Eq:  out_ << "  sub t0, t0, t1\n  seqz t0, t0\n"; break;
            case Op::Ne:  out_ << "  sub t0, t0, t1\n  snez t0, t0\n"; break;
            default: break;
        }
        storeSlot("t0", in.dst);
    }

    void emitCall(const Inst& in) {
        int n = (int)in.args.size();
        // stack args first (i >= 8)
        for (int i = n - 1; i >= 8; i--) {
            loadVal(in.args[i], "t0");
            storeOff("t0", (i - 8) * 4);
        }
        // register args a0..a7
        for (int i = 0; i < n && i < 8; i++) {
            std::string ar = "a" + std::to_string(i);
            loadVal(in.args[i], ar.c_str());
        }
        out_ << "  call " << in.callee->name << "\n";
        if (in.dst >= 0) storeSlot("a0", in.dst);
    }
};

} // namespace

std::string generateAsm(const IRModule& mod, bool opt) {
    (void)opt;   // optimizing path added later
    NaiveCodeGen g(mod);
    return g.run();
}

} // namespace toyc
