/*
 * Win11 Fluent 风格绘制层
 *
 * 【为什么自绘而不是用 WinUI 3】
 * 用户明确要求"绿色版"（单个 exe、免安装、不写注册表）。
 * WinUI 3 需要 .NET SDK + Windows App SDK 运行时，与单文件绿色版直接冲突。
 * 因此用 Win32 + GDI+ 自绘 Fluent 外观：圆角、Win11 配色、亚克力观感，
 * 全部静态链接进一个 exe。
 *
 * 字体取 Win11 的 Segoe UI Variable，并按可用性逐级回退，
 * 避免在不支持的机器上出现方框字。
 */
#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>

namespace ui {

// ==================================================================
// Win11 深色主题标准色
// 参考 Win11 设计规范里的 Layer / Stroke / Text / Accent 分层
// ==================================================================
namespace color {

inline Gdiplus::Color rgb(BYTE r, BYTE g, BYTE b, BYTE a = 255) {
    return Gdiplus::Color(a, r, g, b);
}

// 比纯黑柔和，Win11 深色底
inline Gdiplus::Color windowBg()    { return rgb(32, 32, 32); }
// 卡片（Layer）
inline Gdiplus::Color layer()       { return rgb(43, 43, 43); }
inline Gdiplus::Color layerHover()  { return rgb(52, 52, 52); }
inline Gdiplus::Color layerAlt()    { return rgb(38, 38, 38); }
// 描边（Stroke）
inline Gdiplus::Color stroke()      { return rgb(60, 60, 60); }
inline Gdiplus::Color strokeSoft()  { return rgb(50, 50, 50); }
// 文字
inline Gdiplus::Color text()        { return rgb(255, 255, 255); }
inline Gdiplus::Color textSecond()  { return rgb(170, 170, 170); }
inline Gdiplus::Color textTertiary(){ return rgb(130, 130, 130); }
// 强调色（Win11 默认蓝）
inline Gdiplus::Color accent()      { return rgb(115, 155, 255); }
inline Gdiplus::Color accentHover() { return rgb(135, 172, 255); }
inline Gdiplus::Color accentDim()   { return rgb(70, 95, 160); }
// 状态色
inline Gdiplus::Color ok()          { return rgb(108, 203, 95); }
inline Gdiplus::Color warn()        { return rgb(252, 225, 0); }
inline Gdiplus::Color danger()      { return rgb(255, 99, 99); }
inline Gdiplus::Color idle()        { return rgb(120, 120, 120); }
// 电平条
inline Gdiplus::Color meterLow()    { return rgb(108, 203, 95); }
inline Gdiplus::Color meterMid()    { return rgb(252, 225, 0); }
inline Gdiplus::Color meterHigh()   { return rgb(255, 99, 99); }

} // namespace color

// ==================================================================
// 字体
// ==================================================================
/**
 * 建立字体族。按 Win11 → 旧版 Windows → 中文字体逐级回退，
 * 保证任何机器上都能正常显示中英文。
 */
Gdiplus::FontFamily* fontFamilyRegular();
Gdiplus::FontFamily* fontFamilySemibold();

/** 取字体（size 为像素高度） */
Gdiplus::Font* fontTitle();
Gdiplus::Font* fontBody();
Gdiplus::Font* fontBodyStrong();
Gdiplus::Font* fontCaption();
Gdiplus::Font* fontMono();

/** 释放全部缓存（退出时调用） */
void releaseFonts();

// ==================================================================
// 绘制
// ==================================================================
/** 圆角矩形；stroke 传 nullptr 表示不描边 */
void roundedRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                 const Gdiplus::Color& fill,
                 const Gdiplus::Color* stroke, float strokeW = 1.0f);

/** 圆形 */
void circle(Gdiplus::Graphics& g, float cx, float cy, float radius,
            const Gdiplus::Color& fill);

/**
 * 文字。align: 0=左 1=居中 2=右；垂直居中。
 * 内部会按矩形裁剪，避免长设备名溢出卡片。
 */
void text(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::RectF& r,
          Gdiplus::Font* f, const Gdiplus::Color& c, int align = 0);

/** 单行文字的宽度 */
float textWidth(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::Font* f);

/** 电平条（Win11 风格细圆角条，按响度分段变色） */
void levelBar(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float pct01, bool muted);

/** 音量滑块；hover/drag 影响滑块大小（Win11 的悬停放大效果） */
void slider(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float value01,
            bool hover, bool dragging);

/** 开关（本界面用于静音状态） */
void toggle(Gdiplus::Graphics& g, const Gdiplus::RectF& r, bool on, bool hover);

/** 按钮（描边风格） */
void outlineButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                   const std::wstring& label, Gdiplus::Font* f,
                   bool hover, bool on);

// ==================================================================
// 工具
// ==================================================================
std::wstring toWide(const std::string& s);

/** 取 DPI 缩放后的尺寸 */
int scale(int v);
void setDpi(int dpi);

} // namespace ui
