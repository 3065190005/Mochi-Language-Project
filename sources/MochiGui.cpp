#include "MochiGui.h"

namespace Mochi
{
    // ============================================================
    //   GUI管理
    // ============================================================
    std::queue<GUITask> g_guiTaskQueue;
    std::mutex g_guiTaskMutex;

     // SDL 全局变量（主线程访问）
    SDL_Window* g_window = nullptr;
    SDL_Renderer* g_renderer = nullptr;
    bool g_guiRunning = true;

    TTF_Font* g_font = nullptr;   // 全局字体

    void pushGUITask(std::function<void()> task) {
        std::lock_guard lock(g_guiTaskMutex);
        g_guiTaskQueue.push({ std::move(task) });
    }

    // ===================== 亮色扁平主题 =====================
    SDL_Color themeBg = { 240, 240, 245, 255 };             // 浅灰背景
    SDL_Color themeSurface = { 255, 255, 255, 255 };        // 白色控件背景
    SDL_Color themePrimary = { 70,  130, 220, 255 };        // 主色调（蓝）
    SDL_Color themePrimaryHover = { 100, 160, 240, 255 };   // 悬停亮蓝
    SDL_Color themeText = { 30,  30,  35,  255 };           // 深色文字（用于按钮等深色背景上的字）
    SDL_Color themeTextDark = { 30,  30,  35,  255 };       // 深色文字（用于浅色背景）
    SDL_Color themeDanger = { 230, 80,  80,  255 };         // 危险操作（红）
    SDL_Color themeDangerHover = { 255, 110, 110, 255 };
    SDL_Color themeSuccess = { 60,  200, 100, 255 };        // 成功/进度
    SDL_Color themeBorder = { 180, 180, 185, 255 };         // 浅灰边框

    int themeRadius = 0;                                    // 圆角半径

    // 配色
    SDL_Color g_btnNormal = { 100, 150, 255, 255 };   // 浅蓝
    SDL_Color g_btnHover = { 120, 180, 255, 255 };   // 悬停亮蓝
    SDL_Color g_btnText = { 255, 255, 255, 255 };   // 白色文字
    SDL_Color g_bgColor = { 45, 45, 45, 255 };      // 深灰背景
    int g_btnRadius = 0;   // 按钮圆角半径（会用两条线模拟，简化为直角加边框）
    // 因为 SDL 无原生圆角，可用略小的矩形再填充，造成圆角视觉效果（这里简化，先做直角+粗边框）

    // 用于悬停检测
    int g_hoveredWidgetId = -1;


    // 将字符索引转换为字节偏移（针对一行文本）
    int charIndexToByte(const std::string& line, int charIdx) {
        int bytePos = 0;
        int count = 0;
        while (bytePos < (int)line.size() && count < charIdx) {
            unsigned char c = line[bytePos];
            if (c < 0x80) bytePos += 1;
            else if ((c & 0xE0) == 0xC0) bytePos += 2;
            else if ((c & 0xF0) == 0xE0) bytePos += 3;
            else if ((c & 0xF8) == 0xF0) bytePos += 4;
            else bytePos += 1; // 非法字节跳过
            ++count;
        }
        return bytePos;
    }

    // 获取字符串的字符长度（码点数）
    int utf8Length(const std::string& str) {
        int len = 0;
        size_t i = 0;
        while (i < str.size()) {
            unsigned char c = str[i];
            if (c < 0x80) i += 1;
            else if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i += 1; // 非法字节
            ++len;
        }
        return len;
    }

    // 根据字符索引删除一个字符，返回删除的字节数
    int eraseCharAt(std::string& line, int charIdx) {
        int bytePos = charIndexToByte(line, charIdx);
        if (bytePos >= (int)line.size()) return 0;
        unsigned char c = line[bytePos];
        int bytes = 1;
        if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        line.erase(bytePos, bytes);
        return bytes;
    }

    // 绘制圆角矩形（SDL2_gfx）
    void drawRoundedRect(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color, bool filled) {
        if (filled) {
            roundedBoxRGBA(renderer, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, radius, color.r, color.g, color.b, color.a);
        }
        else {
            roundedRectangleRGBA(renderer, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, radius, color.r, color.g, color.b, color.a);
        }
    }

    void initGUIFont() {
        if (g_font) return;
        TTF_Init();
        // 优先尝试支持中文的现代字体
        g_font = TTF_OpenFont("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 18);
        if (!g_font) g_font = TTF_OpenFont("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 18);
        if (!g_font) g_font = TTF_OpenFont("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 18);
        if (!g_font) g_font = TTF_OpenFont("C:\\Windows\\Fonts\\msyh.ttc", 18);   // 微软雅黑
        if (!g_font) g_font = TTF_OpenFont("C:\\Windows\\Fonts\\simhei.ttf", 18); // 黑体
        if (!g_font) g_font = TTF_OpenFont("/System/Library/Fonts/PingFang.ttc", 18);
        if (!g_font) g_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18);
        if (!g_font) {
            std::cerr << "Warning: No suitable font found, text will be invisible." << std::endl;
        }
    }

    SDL_Texture* renderText(const std::string& text, SDL_Color color) {
        if (!g_font || !g_renderer) return nullptr;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font, text.c_str(), color);
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(g_renderer, surf);
        SDL_FreeSurface(surf);
        return tex;
    }

    void drawRoundedRect(SDL_Renderer* renderer, SDL_Rect rect, int radius, SDL_Color color) {
        // 绘制填充圆角矩形（简单实现：用多个矩形和圆形拼接）
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);

        // 中心矩形
        SDL_Rect inner = { rect.x + radius, rect.y, rect.w - 2 * radius, rect.h };
        SDL_RenderFillRect(renderer, &inner);
        // 上下长条
        SDL_Rect top = { rect.x + radius, rect.y, rect.w - 2 * radius, radius };
        SDL_RenderFillRect(renderer, &top);
        SDL_Rect bottom = { rect.x + radius, rect.y + rect.h - radius, rect.w - 2 * radius, radius };
        SDL_RenderFillRect(renderer, &bottom);

        // 四个角（用圆形逼近）
        auto fillCirclePart = [&](int cx, int cy, int r) {
            for (int dy = -r; dy <= 0; dy++) {
                int dx = (int)(sqrt(r * r - dy * dy) + 0.5);
                SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
            }
            };
        fillCirclePart(rect.x + radius, rect.y + radius, radius);                // 左上
        fillCirclePart(rect.x + rect.w - radius - 1, rect.y + radius, radius);   // 右上
        // 下半圆（翻转 Y）
        auto fillCirclePartBottom = [&](int cx, int cy, int r) {
            for (int dy = 0; dy <= r; dy++) {
                int dx = (int)(sqrt(r * r - dy * dy) + 0.5);
                SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
            }
            };
        fillCirclePartBottom(rect.x + radius, rect.y + rect.h - radius - 1, radius);                // 左下
        fillCirclePartBottom(rect.x + rect.w - radius - 1, rect.y + rect.h - radius - 1, radius);   // 右下
    }

    void drawRoundedBorder(SDL_Renderer* renderer, SDL_Rect rect, int radius, int width, SDL_Color color) {
        // 简单边框：先画一个稍大的圆角矩形，再画一个略小的同色背景，形成边框效果
        // 更精确的边框需要复杂路径，这里用两层叠加模拟
        SDL_Rect outer = rect;
        SDL_Rect inner = { rect.x + width, rect.y + width, rect.w - 2 * width, rect.h - 2 * width };
        // 先画整个背景（含边框区域）
        SDL_Color bg = { 0,0,0,0 }; // 透明，需要先获取背景色？这里假设父背景已绘制。
        // 简化：直接画实色边框圆角矩形
        // 实际使用中，我们会在渲染按钮时用更简单的方法
    }

    int getCharColFromX(const std::string& line, TTF_Font* font, int targetX) {
        if (!font || line.empty() || targetX <= 0) return 0;
        int maxCol = utf8Length(line);
        int low = 0, high = maxCol;
        while (low < high) {
            int mid = (low + high + 1) / 2;
            int bytePos = charIndexToByte(line, mid);
            std::string prefix = line.substr(0, bytePos);
            int w, h;
            TTF_SizeUTF8(font, prefix.c_str(), &w, &h);
            if (w <= targetX) {
                low = mid;
            }
            else {
                high = mid - 1;
            }
        }
        // 如果点击位置超过了当前字符中间，光标移到下一个字符
        if (low < maxCol) {
            int bytePos = charIndexToByte(line, low);
            std::string prefix = line.substr(0, bytePos);
            int prefixW, h;
            TTF_SizeUTF8(font, prefix.c_str(), &prefixW, &h);
            // 获取当前字符宽度
            int nextBytePos = charIndexToByte(line, low + 1);
            std::string curChar = line.substr(bytePos, nextBytePos - bytePos);
            int charW;
            TTF_SizeUTF8(font, curChar.c_str(), &charW, &h);
            if (targetX > prefixW + charW / 2) {
                low++;
            }
        }
        return low;
    }

    // 布局重新计算函数（全局）
    void relayout(int layoutId) {
        if (!g_widgets.count(layoutId)) return;
        auto& layout = g_widgets[layoutId];
        if (layout.layoutType.empty()) return;

        int x = layout.rect.x + layout.padding;
        int y = layout.rect.y + layout.padding;
        for (int childId : layout.children) {
            if (!g_widgets.count(childId)) continue;
            auto& child = g_widgets[childId];
            child.rect.x = x;
            child.rect.y = y;
            if (layout.layoutType == "h") {
                x += child.rect.w + layout.spacing;
            }
            if (layout.layoutType == "v") {
                y += child.rect.h + layout.spacing;
            }
        }
    }

    std::unordered_map<int, Widget> g_widgets;
    int g_nextWidgetId = 1;
}