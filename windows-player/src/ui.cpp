#include "ui.h"

#include <algorithm>

using namespace Gdiplus;

namespace ui {

// ==================================================================
// DPI
// ==================================================================
static int g_dpi = 96;
void setDpi(int dpi) { if (dpi >= 72 && dpi <= 480) g_dpi = dpi; }
int  scale(int v) { return MulDiv(v, g_dpi, 96); }

// ==================================================================
// 字体
// ==================================================================
static FontFamily* g_famRegular  = nullptr;
static FontFamily* g_famSemibold = nullptr;
static Font* g_fTitle      = nullptr;
static Font* g_fBody       = nullptr;
static Font* g_fBodyStrong = nullptr;
static Font* g_fCaption    = nullptr;
static Font* g_fMono       = nullptr;

/**
 * 逐个尝试字体族，取第一个可用的。
 *
 * Win11 用 Segoe UI Variable；老系统只有 Segoe UI；
 * 中文回退到 Microsoft YaHei UI。这样任何机器上都不会出现方框字。
 */
static FontFamily* pickFamily(const wchar_t* const* names, int n) {
    for (int i = 0; i < n; i++) {
        FontFamily* f = new FontFamily(names[i]);
        if (f->IsAvailable()) return f;
        delete f;
    }
    // 全都不行就用系统默认
    return FontFamily::GenericSansSerif()->Clone();
}

FontFamily* fontFamilyRegular() {
    if (g_famRegular) return g_famRegular;
    static const wchar_t* names[] = {
        L"Segoe UI Variable Text",
        L"Segoe UI Variable Display",
        L"Segoe UI",
        L"Microsoft YaHei UI",
        L"Microsoft YaHei",
    };
    g_famRegular = pickFamily(names, 5);
    return g_famRegular;
}

FontFamily* fontFamilySemibold() {
    if (g_famSemibold) return g_famSemibold;
    static const wchar_t* names[] = {
        L"Segoe UI Variable Display",
        L"Segoe UI Variable Text",
        L"Segoe UI Semibold",
        L"Segoe UI",
        L"Microsoft YaHei UI",
    };
    g_famSemibold = pickFamily(names, 5);
    return g_famSemibold;
}

Font* fontTitle() {
    if (!g_fTitle) g_fTitle = new Font(fontFamilySemibold(), (REAL)scale(19), FontStyleRegular, UnitPixel);
    return g_fTitle;
}
Font* fontBody() {
    if (!g_fBody) g_fBody = new Font(fontFamilyRegular(), (REAL)scale(14), FontStyleRegular, UnitPixel);
    return g_fBody;
}
Font* fontBodyStrong() {
    if (!g_fBodyStrong) g_fBodyStrong = new Font(fontFamilySemibold(), (REAL)scale(14), FontStyleRegular, UnitPixel);
    return g_fBodyStrong;
}
Font* fontCaption() {
    if (!g_fCaption) g_fCaption = new Font(fontFamilyRegular(), (REAL)scale(12), FontStyleRegular, UnitPixel);
    return g_fCaption;
}
Font* fontMono() {
    if (!g_fMono) {
        static const wchar_t* names[] = { L"Cascadia Mono", L"Consolas", L"Courier New" };
        FontFamily* fam = pickFamily(names, 3);
        g_fMono = new Font(fam, (REAL)scale(12), FontStyleRegular, UnitPixel);
    }
    return g_fMono;
}

void releaseFonts() {
    delete g_fTitle;      g_fTitle = nullptr;
    delete g_fBody;       g_fBody = nullptr;
    delete g_fBodyStrong; g_fBodyStrong = nullptr;
    delete g_fCaption;    g_fCaption = nullptr;
    delete g_fMono;       g_fMono = nullptr;
    delete g_famRegular;  g_famRegular = nullptr;
    delete g_famSemibold; g_famSemibold = nullptr;
}

// ==================================================================
// 绘制
// ==================================================================
void roundedRect(Graphics& g, const RectF& r, float radius, const Color& fill,
                 const Color* stroke, float strokeW) {
    if (r.Width <= 0 || r.Height <= 0) return;
    float d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d < 1.0f) d = 1.0f;

    GraphicsPath p;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();

    SolidBrush br(fill);
    g.FillPath(&br, &p);
    if (stroke) {
        Pen pen(*stroke, strokeW);
        g.DrawPath(&pen, &p);
    }
}

void circle(Graphics& g, float cx, float cy, float radius, const Color& fill) {
    SolidBrush br(fill);
    g.FillEllipse(&br, cx - radius, cy - radius, radius * 2, radius * 2);
}

void text(Graphics& g, const std::wstring& s, const RectF& r,
          Font* f, const Color& c, int align) {
    if (!f || s.empty()) return;
    StringFormat sf;
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetAlignment(align == 0 ? StringAlignmentNear
                  : align == 1 ? StringAlignmentCenter : StringAlignmentFar);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush br(c);
    g.DrawString(s.c_str(), (INT)s.size(), f, r, &sf, &br);
}

float textWidth(Graphics& g, const std::wstring& s, Font* f) {
    if (!f || s.empty()) return 0;
    RectF box;
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(s.c_str(), (INT)s.size(), f, RectF(0, 0, 4000, 200), &sf, &box);
    return box.Width;
}

void levelBar(Graphics& g, const RectF& r, float pct01, bool muted) {
    // 轨道
    roundedRect(g, r, r.Height / 2, Color(255, 55, 55, 55), nullptr);

    if (muted || pct01 <= 0.001f) return;
    if (pct01 > 1.0f) pct01 = 1.0f;

    float w = r.Width * pct01;
    RectF fill(r.X, r.Y, w, r.Height);

    // 响度越高越偏红，和 Win11 的"音量感"一致
    Color c = color::meterLow();
    if (pct01 > 0.85f)      c = color::meterHigh();
    else if (pct01 > 0.60f) c = color::meterMid();

    roundedRect(g, fill, r.Height / 2, c, nullptr);
}

void slider(Graphics& g, const RectF& r, float v01, bool hover, bool dragging) {
    if (v01 < 0) v01 = 0;
    if (v01 > 1) v01 = 1;

    const float trackH = (float)scale(4);
    const float cy     = r.Y + r.Height / 2;
    RectF track(r.X, cy - trackH / 2, r.Width, trackH);

    roundedRect(g, track, trackH / 2, Color(255, 70, 70, 70), nullptr);

    RectF filled(track.X, track.Y, track.Width * v01, track.Height);
    if (v01 > 0.001f) {
        roundedRect(g, filled, trackH / 2,
                    (hover || dragging) ? color::accentHover() : color::accent(), nullptr);
    }

    // Win11 滑块在悬停/拖动时会变大
    float rad = (float)scale(9);
    if (dragging)     rad = (float)scale(12);
    else if (hover)   rad = (float)scale(11);

    float cx = track.X + track.Width * v01;
    // 外圈：与轨道同色，形成"挖空"观感
    circle(g, cx, cy, rad + scale(3), color::windowBg());
    circle(g, cx, cy, rad, dragging ? color::accentHover() : color::accent());
    // 内点（Win11 滑块中心的实心点）
    circle(g, cx, cy, rad * 0.42f, color::windowBg());
}

void toggle(Graphics& g, const RectF& r, bool on, bool hover) {
    float rad = r.Height / 2;
    Color track = on ? (hover ? color::accentHover() : color::accent())
                     : Color(255, 90, 90, 90);
    roundedRect(g, r, rad, track, nullptr);

    float thumbR = rad - scale(3);
    float cx = on ? (r.GetRight() - rad) : (r.X + rad);
    circle(g, cx, r.Y + rad, thumbR, on ? color::windowBg() : Color(255, 200, 200, 200));
}

void outlineButton(Graphics& g, const RectF& r, const std::wstring& label,
                   Font* f, bool hover, bool on) {
    Color fill = on ? color::accentDim()
                    : (hover ? color::layerHover() : Color(255, 0, 0, 0));
    Color strokeC = on ? color::accent() : color::stroke();
    if (fill.GetA() == 0) {
        roundedRect(g, r, (float)scale(6), Color(255, 0, 0, 0), &strokeC, 1.0f);
    } else {
        roundedRect(g, r, (float)scale(6), fill, &strokeC, 1.0f);
    }
    text(g, label, r, f, on ? color::text() : color::textSecond(), 1);
}

// ==================================================================
std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

} // namespace ui
