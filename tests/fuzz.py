#!/usr/bin/env python3
"""Differential fuzzer for the ToyC compiler.

Generates random *UB-free* ToyC programs (bounded magnitude so no signed
overflow; only non-zero literal divisors) and checks that our compiler's output
run on Spike returns the same exit code as the program compiled natively with
the host C compiler (ToyC being a strict C subset).

Usage:  tests/fuzz.py [N] [--seed S] [--opt]
"""
import os, random, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPILER = os.environ.get("COMPILER", os.path.join(ROOT, "build", "compiler"))
RT = os.path.join(ROOT, "runtime")
RVGCC = os.environ.get("RVGCC", "riscv64-unknown-elf-gcc")
SPIKE = os.environ.get("SPIKE", "spike")
HOSTCC = os.environ.get("HOSTCC", "cc")
RVFLAGS = ["-march=rv32im", "-mabi=ilp32", "-O0", "-nostdlib", "-nostartfiles",
           "-T", os.path.join(RT, "link.ld")]

N = 300
SEED = 12345
OPT = []
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == "--seed": SEED = int(args[i+1]); i += 2
    elif args[i] == "--opt": OPT = ["-opt"]; i += 1
    else: N = int(args[i]); i += 1

VARS = ["v0", "v1", "v2", "v3"]

def gen_expr(depth, use_mul):
    if depth <= 0:
        if random.random() < 0.5:
            return str(random.randint(-9, 9))
        return random.choice(VARS)
    choice = random.random()
    a = lambda: gen_expr(depth - 1, use_mul)
    if choice < 0.30:
        return "(" + a() + random.choice([" + ", " - "]) + a() + ")"
    if choice < 0.45 and use_mul:
        return "(" + a() + " * " + a() + ")"
    if choice < 0.55:
        return "(" + a() + " / " + str(random.randint(1, 9)) + ")"
    if choice < 0.62:
        return "(" + a() + " % " + str(random.randint(1, 9)) + ")"
    if choice < 0.78:
        return "(" + a() + random.choice([" < ", " > ", " <= ", " >= ", " == ", " != "]) + a() + ")"
    if choice < 0.90:
        return "(" + a() + random.choice([" && ", " || "]) + a() + ")"
    if choice < 0.95:
        return "(!" + a() + ")"
    return "(-" + a() + ")"

def gen_program_expr():
    # small variable values keep multiplication results well within int range
    decls = "".join(f"  int {v} = {random.randint(-4, 4)};\n" for v in VARS)
    # depth 3 with leaves in [-9,9] and small vars: |value| < 2^30, no overflow
    use_mul = random.random() < 0.6
    expr = gen_expr(3, use_mul)
    return f"int main() {{\n{decls}  return {expr};\n}}\n"

# --- control-flow generator (bounded so it is provably overflow-free) --------
CTR = ["k0", "k1", "k2"]

def small_arith(d=2):
    if d <= 0 or random.random() < 0.4:
        return random.choice(CTR + [str(random.randint(-9, 9))])
    return "(" + small_arith(d-1) + random.choice([" + ", " - "]) + small_arith(d-1) + ")"

def small_bool(depth):
    c = f"k{depth}" if depth < 3 else "k0"
    return f"({c} {random.choice(['<','>','<=','>=','==','!='])} {random.randint(0,6)})"

def assign_stmt():
    t = random.choice(["a", "b", "s"])
    e = small_arith()
    return f"s = s + ({e});" if t == "s" else f"{t} = ({e});"

def gen_block(depth):
    out = []
    for _ in range(random.randint(1, 3)):
        out.append(gen_stmt(depth))
    return " ".join(out)

def gen_stmt(depth):
    r = random.random()
    if depth < 2 and r < 0.35:
        n = random.randint(2, 6)
        c = f"k{depth}"
        return f"{c} = 0; while ({c} < {n}) {{ {gen_block(depth+1)} {c} = {c} + 1; }}"
    if r < 0.6:
        return f"if ({small_bool(depth)}) {{ {assign_stmt()} }} else {{ {assign_stmt()} }}"
    return assign_stmt()

def gen_program_cf():
    body = " ".join(gen_stmt(0) for _ in range(random.randint(3, 6)))
    return ("int main() {\n  int a = 0; int b = 0; int s = 0;\n"
            "  int k0 = 0; int k1 = 0; int k2 = 0;\n"
            f"  {body}\n  return s + a - b;\n}}\n")

def gen_program():
    return gen_program_cf() if random.random() < 0.5 else gen_program_expr()

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)

def main():
    random.seed(SEED)
    if not os.path.exists(COMPILER):
        print("compiler not built:", COMPILER); sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="toycfuzz")
    fails = 0
    for k in range(N):
        prog = gen_program()
        src = os.path.join(tmp, "p.tc")
        open(src, "w").write(prog)

        nat = os.path.join(tmp, "p.native")
        if run([HOSTCC, "-x", "c", "-w", "-o", nat, src]).returncode != 0:
            continue
        exp = run([nat]).returncode & 0xFF

        cc = run([COMPILER] + OPT, stdin=open(src))
        if cc.returncode != 0:
            print(f"[{k}] COMPILER ERROR\n{prog}\n{cc.stderr.decode()}"); fails += 1; continue
        s = os.path.join(tmp, "p.s"); open(s, "wb").write(cc.stdout)
        elf = os.path.join(tmp, "p.elf")
        ln = run([RVGCC] + RVFLAGS + [os.path.join(RT, "start.S"), s, "-o", elf])
        if ln.returncode != 0:
            print(f"[{k}] LINK ERROR\n{prog}\n{ln.stderr.decode()}"); fails += 1; continue
        got = run([SPIKE, "--isa=rv32im", elf]).returncode & 0xFF
        if got != exp:
            print(f"[{k}] MISMATCH exp={exp} got={got}\n{prog}"); fails += 1
            if fails >= 5:
                print("stopping after 5 mismatches"); break
    print(f"fuzz done: {N} programs, {fails} failures {'(opt)' if OPT else ''}")
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
