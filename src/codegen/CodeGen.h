#pragma once
#include "ir/IR.h"
#include <string>

namespace toyc {

// Emits RV32IM assembly for an IR module to a string.
//
// By default the optimizing back end (graph-colouring register allocation,
// immediate operands, compare/branch fusion, fall-through layout) is used for
// both functional and performance runs. Setting the environment variable
// TOYC_NAIVE forces the simple stack-slot-per-vreg path (kept for differential
// testing). `opt` enables the more aggressive IR-level transforms.
std::string generateAsm(const IRModule& mod, bool opt);

// Back-end entry points.
std::string generateAsmNaive(const IRModule& mod);
std::string generateAsmOpt(const IRModule& mod, bool opt);

} // namespace toyc
