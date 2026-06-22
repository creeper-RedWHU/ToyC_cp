#pragma once
#include "ir/IR.h"

namespace toyc {

// Run IR-level optimization passes in place. `opt` enables the more aggressive
// transforms; correctness-preserving cleanups run regardless.
void optimizeIR(IRModule& mod, bool opt);

} // namespace toyc
