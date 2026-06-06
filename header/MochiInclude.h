#pragma once
#ifndef MOCHIINCLUDE__
#define MOCHIINCLUDE__


#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <variant>
#include <stdexcept>
#include <any>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include <random>
#include <optional>
#include <cstdio>
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#include <iomanip>
#include <cctype>
#include <algorithm>
#include <numeric>
#include <regex>


// gui管理

#include "SDL.h"
#include "SDL_ttf.h"
#include "SDL2_gfxPrimitives.h"
#include <unordered_map>

// FFI
#ifdef _WIN32
#define MOCHI_API __declspec(dllexport)
#else
#define MOCHI_API __attribute__((visibility("default")))
#endif

#ifdef _WIN32
    // 前向声明 Windows API 类型和函数，避免包含 windows.h 导致宏/类型污染
extern "C" {
    typedef void* HMODULE;
    typedef unsigned long DWORD;
    typedef int BOOL;
    typedef void* FARPROC;
    __declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char* lpLibFileName);
    __declspec(dllimport) BOOL __stdcall FreeLibrary(HMODULE hLibModule);
    __declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE hModule, const char* lpProcName);
}

// 手动声明，避免引入 windows.h 造成宏污染
extern "C" {
    int __stdcall SetConsoleOutputCP(unsigned int);
    int __stdcall SetConsoleCP(unsigned int);
}
#define CP_UTF8 65001

#else
#include <dlfcn.h>
#endif

#endif // !MOCHIINCLUDE__