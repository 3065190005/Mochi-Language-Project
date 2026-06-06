#include "MochiInclude.h"
#include "MochiLexer.h"
#include "MochiRuntime.h"

#ifndef MOCHIUTILS
#define MOCHIUTILS

namespace Mochi
{
    // ============================================================
    // 0. ¸¨Öúº¯Êý
    // ============================================================
    bool isTruthy(const Value& v);


    std::string stringify(const Value& v);

}


#endif