#pragma once
#include "ast/AST.h"
#include <cstdint>
#include <string>
#include <vector>

namespace toyc {

// An operand: either an immediate or a virtual register.
struct Val {
    enum Tag { Imm, Reg } tag = Imm;
    int32_t imm = 0;
    int reg = -1;
    bool isImm() const { return tag == Imm; }
    bool isReg() const { return tag == Reg; }
    static Val I(int32_t v) { Val x; x.tag = Imm; x.imm = v; return x; }
    static Val R(int r) { Val x; x.tag = Reg; x.reg = r; return x; }
};

enum class Op {
    Add, Sub, Mul, Div, Mod,
    Lt, Gt, Le, Ge, Eq, Ne,
    Neg, Not,             // unary: Neg = arithmetic negate, Not = logical (0/1)
    Mv,                   // dst = a
    LoadGlobal,           // dst = globals[gid]
    StoreGlobal,          // globals[gid] = a
    Call,                 // dst = callee(args...)   (dst == -1 when void)
};

struct Inst {
    Op op;
    int dst = -1;
    Val a, b;
    int gid = -1;                 // LoadGlobal / StoreGlobal
    FuncDef* callee = nullptr;    // Call
    std::vector<Val> args;        // Call
    int magicVreg = -1;           // Codegen hint: pre-computed magic number for Div/Mod by constant
    bool unsignedDiv = false;     // Codegen hint: dividend is non-negative, use unsigned magic division
};

enum class Term { None, Jmp, Br, Ret };

struct BasicBlock {
    int id = -1;
    std::vector<Inst> insts;
    Term term = Term::None;
    Val cond;                 // Br condition
    int succ0 = -1;           // Jmp target / Br true-target
    int succ1 = -1;           // Br false-target
    bool retHasVal = false;   // Ret
    Val retVal;               // Ret value
    std::vector<int> preds;   // computed by computeCFG
};

struct IRFunc {
    std::string name;
    bool returnsVoid = false;
    int numParams = 0;
    std::vector<int> paramRegs;    // vreg holding each incoming parameter
    std::vector<BasicBlock> blocks;
    int numVregs = 0;
};

struct IRModule {
    struct Global { std::string name; int32_t init = 0; };
    std::vector<Global> globals;   // indexed by globalId
    std::vector<IRFunc> funcs;
};

// Successor list of a block (0, 1 or 2 entries; Ret has none).
inline void succsOf(const BasicBlock& bb, int out[2], int& n) {
    n = 0;
    if (bb.term == Term::Jmp) { out[n++] = bb.succ0; }
    else if (bb.term == Term::Br) { out[n++] = bb.succ0; out[n++] = bb.succ1; }
}

// (Re)compute predecessor lists for every block of a function.
void computeCFG(IRFunc& fn);

} // namespace toyc
