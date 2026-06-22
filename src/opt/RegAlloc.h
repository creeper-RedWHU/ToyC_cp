#pragma once
#include "ir/IR.h"
#include <vector>

namespace toyc {

// Result of register allocation for one function.
struct Allocation {
    std::vector<int> reg;        // vreg -> x-register number, or -1 if spilled
    std::vector<int> spillSlot;  // vreg -> spill slot index (if spilled), else -1
    int numSpills = 0;
    std::vector<int> usedCallee; // distinct callee-saved x-regs used (to save)
    bool isLeaf = true;          // function makes no calls
};

// Allocate registers for `fn` (graph-colouring with move biasing; values live
// across a call are constrained to callee-saved registers).
Allocation allocateRegisters(const IRFunc& fn);

} // namespace toyc
