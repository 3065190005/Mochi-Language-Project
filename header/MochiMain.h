#ifndef MOCHIMAIN
#define MOCHIMAIN

#include "MochiInclude.h"
#include "MochiLexer.h"
#include "MochiParser.h"

namespace Mochi
{
    // ============================================================
    // 运行入口
    // ============================================================
    void run(const std::string& source);

    void runFile(const std::string& path);

    //// ============================================================
    //// REPL
    //// ============================================================

    void runPrompt();
}

#endif