#ifndef MOCHIGUI
#define MOCHIGUI

#pragma once
#include "MochiInclude.h"
#include "MochiRuntime.h"

namespace Mochi
{
    // ============================================================
    //   GUI管理
    // ============================================================

    struct GUITask {
        std::function<void()> action;
    };

    extern std::queue<GUITask> g_guiTaskQueue;
    extern std::mutex g_guiTaskMutex;

    void pushGUITask(std::function<void()> task);

    // SDL 全局变量（主线程访问）
    extern SDL_Window* g_window;
    extern SDL_Renderer* g_renderer;
    extern bool g_guiRunning;

    extern TTF_Font* g_font;   // 全局字体

    //// ===================== 扁平化主题 =====================
    //SDL_Color themeBg = { 30,  30,  35,  255 };  // 深色背景
    //SDL_Color themeSurface = { 45,  45,  50,  255 };  // 控件背景
    //SDL_Color themePrimary = { 80,  140, 255, 255 };  // 主色调
    //SDL_Color themePrimaryHover = { 110, 165, 255, 255 };  // 悬停
    //SDL_Color themeText = { 230, 230, 235, 255 };  // 浅色文字
    //SDL_Color themeTextDark = { 30,  30,  35,  255 };  // 深色文字
    //SDL_Color themeDanger = { 255, 90,  90,  255 };  // 危险操作
    //SDL_Color themeDangerHover = { 255, 120, 120, 255 };
    //SDL_Color themeSuccess = { 80,  200, 120, 255 };  // 成功/进度
    //SDL_Color themeBorder = { 80,  80,  85,  255 };  // 边框
    //int themeRadius = 6;   // 圆角半径

    // ===================== 亮色扁平主题 =====================
    extern SDL_Color themeBg;           // 浅灰背景
    extern SDL_Color themeSurface;      // 白色控件背景
    extern SDL_Color themePrimary;      // 主色调（蓝）
    extern SDL_Color themePrimaryHover; // 悬停亮蓝
    extern SDL_Color themeText;         // 深色文字（用于按钮等深色背景上的字）
    extern  SDL_Color themeTextDark;    // 深色文字（用于浅色背景）
    extern SDL_Color themeDanger;       // 危险操作（红）
    extern SDL_Color themeDangerHover;
    extern SDL_Color themeSuccess;      // 成功/进度
    extern SDL_Color themeBorder;;      // 浅灰边框

    extern int themeRadius;             // 圆角半径

    // 将字符索引转换为字节偏移（针对一行文本）
    int charIndexToByte(const std::string& line, int charIdx);
    // 获取字符串的字符长度（码点数）
    int utf8Length(const std::string& str);
    // 根据字符索引删除一个字符，返回删除的字节数
    int eraseCharAt(std::string& line, int charIdx);

    // 绘制圆角矩形（SDL2_gfx）
    void drawRoundedRect(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color, bool filled = true);

    void initGUIFont();

    SDL_Texture* renderText(const std::string& text, SDL_Color color);
    void drawRoundedRect(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color);
    void drawRoundedBorder(SDL_Renderer* renderer, SDL_Rect rect, int radius, int width, SDL_Color color);

    // 配色
     extern SDL_Color g_btnNormal;   // 浅蓝
     extern SDL_Color g_btnHover;   // 悬停亮蓝
     extern  SDL_Color g_btnText;   // 白色文字
     extern  SDL_Color g_bgColor;      // 深灰背景
     extern  int g_btnRadius;   // 按钮圆角半径（会用两条线模拟，简化为直角加边框）
    // 因为 SDL 无原生圆角，可用略小的矩形再填充，造成圆角视觉效果（这里简化，先做直角+粗边框）

    // 用于悬停检测
    extern  int g_hoveredWidgetId;

    // 控件存储（按唯一ID索引）
    struct Widget {
        std::string type;
        SDL_Rect rect;
        std::string text;
        std::shared_ptr<MochiFunction> callback;
        bool checked = false;
        double value = 0.0;
        double min = 0.0, max = 1.0, step = 0.01;
        bool focused = false;
        std::shared_ptr<MochiFunction> onChange;
        bool password = false;
        bool enabled = true;
        bool visible = true;
        std::vector<std::string> lines;   // 多行文本
        int scrollY = 0;                  // 垂直滚动偏移（行号）
        int cursorLine = 0, cursorCol = 0; // 光标位置
        bool wordWrap = false;   // 是否自动换行

        std::vector<int> children;   // 子控件 id（用于布局）
        int spacing = 6;
        int padding = 8;
        std::string layoutType;      // "h" 或 "v"
        int layerId = -1;            // 所属层 id（布局容器需要）

        struct {
            SDL_Color bgNormal = { 100, 150, 255, 255 };
            SDL_Color bgHover = { 120, 180, 255, 255 };
            SDL_Color bgDisabled = { 80, 80, 80, 255 };
            SDL_Color textColor = { 255, 255, 255, 255 };
            SDL_Color borderColor = { 255, 255, 255, 100 };
            int borderRadius = 8;
            int borderWidth = 1;
        } style;
    };

    int getCharColFromX(const std::string& line, TTF_Font* font, int targetX);
    void relayout(int layoutId);

    extern std::unordered_map<int, Widget> g_widgets;
    extern int g_nextWidgetId;

}

#endif