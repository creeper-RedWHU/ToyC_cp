#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/Sema.h"
#include "ir/IRGen.h"
#include "opt/Passes.h"
#include "codegen/CodeGen.h"

#include <iostream>
#include <sstream>
#include <string>

using namespace toyc;

int main(int argc, char** argv) {
    bool opt = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-opt") opt = true;
        // unknown flags are ignored per the spec
    }

    std::ostringstream ss;
    ss << std::cin.rdbuf();
    std::string src = ss.str();

    try {
        Lexer lexer(std::move(src));
        auto tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        Module mod = parser.parseModule();

        int globalCount = 0;
        Sema sema;
        sema.analyze(mod, globalCount);

        IRGen irgen;
        IRModule ir = irgen.generate(mod, globalCount);

        optimizeIR(ir, opt);

        std::string asmText = generateAsm(ir, opt);
        std::cout << asmText;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
