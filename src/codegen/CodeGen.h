#pragma once
#include "ir/IR.h"
#include <string>

namespace toyc {

// Emits RV32IM assembly for an IR module to a string.
// `opt` selects the optimizing path (register allocation etc.); when false a
// simple, always-correct stack-slot-per-vreg strategy is used.
std::string generateAsm(const IRModule& mod, bool opt);

} // namespace toyc
