#pragma once
#include <vector>

namespace toyc {

// RISC-V integer registers by their x-number.
enum : int {
    X_ZERO = 0, X_RA = 1, X_SP = 2, X_GP = 3, X_TP = 4,
    X_T0 = 5, X_T1 = 6, X_T2 = 7,
    X_S0 = 8, X_S1 = 9,
    X_A0 = 10, X_A1 = 11, X_A2 = 12, X_A3 = 13,
    X_A4 = 14, X_A5 = 15, X_A6 = 16, X_A7 = 17,
    X_S2 = 18, X_S3 = 19, X_S4 = 20, X_S5 = 21, X_S6 = 22, X_S7 = 23,
    X_S8 = 24, X_S9 = 25, X_S10 = 26, X_S11 = 27,
    X_T3 = 28, X_T4 = 29, X_T5 = 30, X_T6 = 31,
};

inline const char* regName(int x) {
    static const char* names[32] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    };
    return (x >= 0 && x < 32) ? names[x] : "?";
}

// Caller-saved registers preferred for values that do NOT span a call
// (no save/restore needed). a0-a7 are reserved for argument passing/return and
// are not general-allocatable, but are usable as transient scratch.
inline const std::vector<int>& callerSavedPool() {
    static const std::vector<int> v = { X_T0, X_T1, X_T2, X_T3, X_T4 };
    return v;
}

// Callee-saved registers: required for values that live across a call; any used
// here must be saved/restored by the function prologue/epilogue.
inline const std::vector<int>& calleeSavedPool() {
    static const std::vector<int> v = {
        X_S0, X_S1, X_S2, X_S3, X_S4, X_S5,
        X_S6, X_S7, X_S8, X_S9, X_S10, X_S11,
    };
    return v;
}

inline bool isCalleeSaved(int x) {
    return (x == X_S0 || x == X_S1 || (x >= X_S2 && x <= X_S11));
}

// Scratch registers, never allocatable: used to materialize spilled operands,
// large immediates, and global addresses.
enum : int { SCRATCH0 = X_T5, SCRATCH1 = X_T6 };

} // namespace toyc
