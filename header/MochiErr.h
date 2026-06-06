#include "MochiInclude.h"
#include "MochiRuntime.h"
#ifndef MOCHIERR
#define MOCHIERR

namespace Mochi
{
    // ============================================================
// 7. “Ï≥£¿‡
// ============================================================
    struct ReturnException : std::exception {
        Value value;
        explicit ReturnException(Value v) : value(std::move(v)) {}
    };

    struct ThrowException : std::exception {
        Value value;
        explicit ThrowException(Value v) : value(std::move(v)) {}
    };

    struct BreakException : std::exception {};
    struct ContinueException : std::exception {};

}

#endif