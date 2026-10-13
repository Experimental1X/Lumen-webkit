#pragma once

// Theme colors and anti-aliased (GDI+) drawing helpers for the browser chrome.
// Header-only; everything here is inline.

#include <windows.h>

// GDI+ uses min/max, which NOMINMAX removes, so provide them.
#include <algorithm>
#include <cmath>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

namespace lumen::gfx {

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

struct Theme {
    bool dark { false };
    COLORREF page { RGB(255, 255, 255) };
    COLORREF toolbar { RGB(246, 246, 248) };
    COLORREF tabBar { RGB(236, 236, 240) };
    COLORREF tabActive { RGB(255, 255, 255) };
    COLORREF tabHover { RGB(226, 226, 231) };
    COLORREF tabBorder { RGB(214, 214, 220) };
    COLORREF separator { RGB(212, 212, 218) };
    COLORREF glass { RGB(255, 255, 255) };
    COLORREF glassBorder { RGB(222, 222, 228) };
    COLORREF glassHover { RGB(236, 236, 240) };
    COLORREF pill { RGB(233, 233, 238) };
    COLORREF pillHover { RGB(226, 226, 232) };
    COLORREF pillFocused { RGB(255, 255, 255) };
    COLORREF text { RGB(28, 28, 30) };
    COLORREF textSecondary { RGB(110, 110, 118) };
    COLORREF textDisabled { RGB(178, 178, 186) };
    COLORREF accent { RGB(0, 122, 255) };
};

inline Theme makeTheme(bool dark)
{
    Theme t;
    t.dark = dark;
    if (!dark)
        return t;

    t.page = RGB(30, 30, 32);
    t.toolbar = RGB(44, 44, 47);
    t.tabBar = RGB(34, 34, 37);
    t.tabActive = RGB(66, 66, 70);
    t.tabHover = RGB(50, 50, 54);
    t.tabBorder = RGB(84, 84, 90);
    t.separator = RGB(22, 22, 24);
    t.glass = RGB(66, 66, 70);
    t.glassBorder = RGB(86, 86, 92);
    t.glassHover = RGB(82, 82, 88);
    t.pill = RGB(60, 60, 64);
    t.pillHover = RGB(68, 68, 72);
    t.pillFocused = RGB(76, 76, 80);
    t.text = RGB(242, 242, 247);
    t.textSecondary = RGB(160, 160, 168);
    t.textDisabled = RGB(100, 100, 106);
    t.accent = RGB(10, 132, 255);
    return t;
}

inline bool systemUsesDarkMode()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size
    );
    return status == ERROR_SUCCESS && value == 0;
}



// ---------------------------------------------------------------------------
// GDI+ drawing helpers (anti-aliased shapes and SF-Symbols-style glyphs)
// ---------------------------------------------------------------------------

inline void fillSolid(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

inline Gdiplus::Color toGdiColor(COLORREF color, BYTE alpha = 255)
{
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

inline Gdiplus::RectF toRectF(const RECT& rect)
{
    return Gdiplus::RectF(
        static_cast<Gdiplus::REAL>(rect.left),
        static_cast<Gdiplus::REAL>(rect.top),
        static_cast<Gdiplus::REAL>(rect.right - rect.left),
        static_cast<Gdiplus::REAL>(rect.bottom - rect.top)
    );
}

inline void addRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, Gdiplus::REAL radius)
{
    if (rect.Width <= 0 || rect.Height <= 0)
        return;

    Gdiplus::REAL diameter = std::min({ radius * 2.0f, rect.Width, rect.Height });
    if (diameter < 1.0f)
        diameter = 1.0f;

    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

inline void fillRoundedRect(Gdiplus::Graphics& g, const RECT& rect, int radius, COLORREF color, BYTE alpha = 255)
{
    Gdiplus::GraphicsPath path;
    addRoundedRect(path, toRectF(rect), static_cast<Gdiplus::REAL>(radius));
    Gdiplus::SolidBrush brush(toGdiColor(color, alpha));
    g.FillPath(&brush, &path);
}

inline void strokeRoundedRect(Gdiplus::Graphics& g, const RECT& rect, int radius, COLORREF color, Gdiplus::REAL thickness, BYTE alpha = 255)
{
    Gdiplus::RectF bounds = toRectF(rect);
    bounds.X += thickness / 2.0f;
    bounds.Y += thickness / 2.0f;
    bounds.Width -= thickness;
    bounds.Height -= thickness;

    Gdiplus::GraphicsPath path;
    addRoundedRect(path, bounds, static_cast<Gdiplus::REAL>(radius) - thickness / 2.0f);
    Gdiplus::Pen pen(toGdiColor(color, alpha), thickness);
    g.DrawPath(&pen, &path);
}

inline void configureIconPen(Gdiplus::Pen& pen)
{
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
}

enum class TrafficLight { Close, Minimize, Maximize };

inline void drawTrafficLight(Gdiplus::Graphics& g, const RECT& rect, COLORREF color)
{
    Gdiplus::SolidBrush brush(toGdiColor(color));
    g.FillEllipse(&brush, toRectF(rect));
    Gdiplus::Pen edge(Gdiplus::Color(48, 0, 0, 0), 1.0f);
    Gdiplus::RectF bounds = toRectF(rect);
    bounds.X += 0.5f;
    bounds.Y += 0.5f;
    bounds.Width -= 1.0f;
    bounds.Height -= 1.0f;
    g.DrawEllipse(&edge, bounds);
}

inline void drawTrafficLightGlyph(Gdiplus::Graphics& g, const RECT& rect, TrafficLight kind, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    Gdiplus::Pen pen(Gdiplus::Color(190, 60, 30, 20), 1.3f * k);
    configureIconPen(pen);

    if (kind == TrafficLight::Close) {
        g.DrawLine(&pen, cx - 2.4f * k, cy - 2.4f * k, cx + 2.4f * k, cy + 2.4f * k);
        g.DrawLine(&pen, cx + 2.4f * k, cy - 2.4f * k, cx - 2.4f * k, cy + 2.4f * k);
    } else if (kind == TrafficLight::Minimize) {
        g.DrawLine(&pen, cx - 2.8f * k, cy, cx + 2.8f * k, cy);
    } else {
        g.DrawLine(&pen, cx - 2.8f * k, cy, cx + 2.8f * k, cy);
        g.DrawLine(&pen, cx, cy - 2.8f * k, cx, cy + 2.8f * k);
    }
}

inline void drawChevronIcon(Gdiplus::Graphics& g, const RECT& rect, bool forward, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float dir = forward ? 1.0f : -1.0f;

    Gdiplus::Pen pen(toGdiColor(color), 2.0f * k);
    configureIconPen(pen);
    Gdiplus::PointF points[3] = {
        Gdiplus::PointF(cx - dir * 3.0f * k, cy - 7.0f * k),
        Gdiplus::PointF(cx + dir * 3.0f * k, cy),
        Gdiplus::PointF(cx - dir * 3.0f * k, cy + 7.0f * k)
    };
    g.DrawLines(&pen, points, 3);
}

inline void drawSidebarIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.7f * k);
    configureIconPen(pen);
    Gdiplus::GraphicsPath path;
    addRoundedRect(path, Gdiplus::RectF(cx - 9.0f * k, cy - 7.0f * k, 18.0f * k, 14.0f * k), 3.5f * k);
    g.DrawPath(&pen, &path);
    g.DrawLine(&pen, cx - 3.0f * k, cy - 7.0f * k, cx - 3.0f * k, cy + 7.0f * k);
}

inline void drawShareIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.7f * k);
    configureIconPen(pen);

    Gdiplus::PointF box[6] = {
        Gdiplus::PointF(cx - 3.0f * k, cy - 1.0f * k),
        Gdiplus::PointF(cx - 6.0f * k, cy - 1.0f * k),
        Gdiplus::PointF(cx - 6.0f * k, cy + 8.0f * k),
        Gdiplus::PointF(cx + 6.0f * k, cy + 8.0f * k),
        Gdiplus::PointF(cx + 6.0f * k, cy - 1.0f * k),
        Gdiplus::PointF(cx + 3.0f * k, cy - 1.0f * k)
    };
    g.DrawLines(&pen, box, 6);
    g.DrawLine(&pen, cx, cy - 9.0f * k, cx, cy + 3.0f * k);

    Gdiplus::PointF head[3] = {
        Gdiplus::PointF(cx - 3.5f * k, cy - 5.5f * k),
        Gdiplus::PointF(cx, cy - 9.0f * k),
        Gdiplus::PointF(cx + 3.5f * k, cy - 5.5f * k)
    };
    g.DrawLines(&pen, head, 3);
}

inline void drawPlusIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.8f * k);
    configureIconPen(pen);
    g.DrawLine(&pen, cx - 7.0f * k, cy, cx + 7.0f * k, cy);
    g.DrawLine(&pen, cx, cy - 7.0f * k, cx, cy + 7.0f * k);
}

inline void drawTabOverviewIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.6f * k);
    configureIconPen(pen);

    // Front square.
    Gdiplus::GraphicsPath front;
    addRoundedRect(front, Gdiplus::RectF(cx - 2.0f * k, cy - 2.0f * k, 9.0f * k, 9.0f * k), 2.2f * k);
    g.DrawPath(&pen, &front);

    // Visible "L" of the square behind it.
    Gdiplus::PointF back[7] = {
        Gdiplus::PointF(cx + 2.0f * k, cy - 2.5f * k),
        Gdiplus::PointF(cx + 2.0f * k, cy - 5.0f * k),
        Gdiplus::PointF(cx + 0.0f * k, cy - 7.0f * k),
        Gdiplus::PointF(cx - 5.0f * k, cy - 7.0f * k),
        Gdiplus::PointF(cx - 7.0f * k, cy - 5.0f * k),
        Gdiplus::PointF(cx - 7.0f * k, cy + 0.0f * k),
        Gdiplus::PointF(cx - 5.0f * k, cy + 2.0f * k)
    };
    g.DrawLines(&pen, back, 7);
    g.DrawLine(&pen, cx - 5.0f * k, cy + 2.0f * k, cx - 2.5f * k, cy + 2.0f * k);
}

inline void drawReloadIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float r = 6.5f * k;

    Gdiplus::Pen pen(toGdiColor(color), 1.7f * k);
    configureIconPen(pen);
    g.DrawArc(&pen, Gdiplus::RectF(cx - r, cy - r, r * 2, r * 2), -35.0f, 290.0f);

    // Arrow head at the end of the arc (255 degrees), pointing along the sweep.
    const float px = cx - 0.2588f * r;
    const float py = cy - 0.9659f * r;
    const float tipX = px + 0.9659f * 2.5f * k;
    const float tipY = py - 0.2588f * 2.5f * k;
    const float baseX = tipX - 0.9659f * 4.5f * k;
    const float baseY = tipY + 0.2588f * 4.5f * k;
    Gdiplus::PointF head[3] = {
        Gdiplus::PointF(baseX + 0.2588f * 3.5f * k, baseY + 0.9659f * 3.5f * k),
        Gdiplus::PointF(tipX, tipY),
        Gdiplus::PointF(baseX - 0.2588f * 3.5f * k, baseY - 0.9659f * 3.5f * k)
    };
    g.DrawLines(&pen, head, 3);
}

inline void drawStopIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.8f * k);
    configureIconPen(pen);
    g.DrawLine(&pen, cx - 4.5f * k, cy - 4.5f * k, cx + 4.5f * k, cy + 4.5f * k);
    g.DrawLine(&pen, cx + 4.5f * k, cy - 4.5f * k, cx - 4.5f * k, cy + 4.5f * k);
}

inline void drawCloseIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.5f * k);
    configureIconPen(pen);
    g.DrawLine(&pen, cx - 3.0f * k, cy - 3.0f * k, cx + 3.0f * k, cy + 3.0f * k);
    g.DrawLine(&pen, cx + 3.0f * k, cy - 3.0f * k, cx - 3.0f * k, cy + 3.0f * k);
}

inline void drawLockIcon(Gdiplus::Graphics& g, float cx, float cy, COLORREF color, float k)
{
    Gdiplus::Pen pen(toGdiColor(color), 1.3f * k);
    configureIconPen(pen);
    g.DrawArc(&pen, Gdiplus::RectF(cx - 2.8f * k, cy - 6.0f * k, 5.6f * k, 6.0f * k), 180.0f, 180.0f);
    g.DrawLine(&pen, cx - 2.8f * k, cy - 3.0f * k, cx - 2.8f * k, cy - 0.5f * k);
    g.DrawLine(&pen, cx + 2.8f * k, cy - 3.0f * k, cx + 2.8f * k, cy - 0.5f * k);

    Gdiplus::GraphicsPath body;
    addRoundedRect(body, Gdiplus::RectF(cx - 4.2f * k, cy - 1.0f * k, 8.4f * k, 6.5f * k), 1.5f * k);
    Gdiplus::SolidBrush brush(toGdiColor(color));
    g.FillPath(&brush, &body);
}

inline void drawPageMenuIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 15.0f * k, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(toGdiColor(color));
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    g.DrawString(L"aA", -1, &font, toRectF(rect), &format, &brush);
}

inline COLORREF colorFromHue(int hue)
{
    // HSL(hue, 52%, 46%) to RGB, used for site badges.
    const float s = 0.52f, l = 0.46f;
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float h = static_cast<float>(((hue % 360) + 360) % 360) / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const float m = l - c / 2.0f;
    return RGB(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((b + m) * 255));
}

// Placeholder "favicon": a rounded square with the site's first letter.
inline void drawSiteBadge(Gdiplus::Graphics& g, const RECT& rect, wchar_t letter, COLORREF color, float k)
{
    fillRoundedRect(g, rect, static_cast<int>(3.5f * k), color);

    wchar_t text[2] = { letter, 0 };
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(rect.bottom - rect.top) * 0.62f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(text, 1, &font, toRectF(rect), &format, &brush);
}

// Icon for the Start Page tab: a 2x2 grid of small rounded squares.
inline void drawStartPageIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float size = 4.6f * k;
    const float gap = 1.8f * k;
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            Gdiplus::GraphicsPath path;
            const float x = cx - size - gap / 2.0f + static_cast<float>(column) * (size + gap);
            const float y = cy - size - gap / 2.0f + static_cast<float>(row) * (size + gap);
            addRoundedRect(path, Gdiplus::RectF(x, y, size, size), 1.3f * k);
            Gdiplus::SolidBrush brush(toGdiColor(color));
            g.FillPath(&brush, &path);
        }
    }
}

// Downloads button: a downward arrow on a tray, with a coloured progress arc
// wrapping it when a download is in flight. Minimal placeholder so the
// toolbar builds; replace with the project's fuller version if it has one.
inline void drawDownloadsIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF baseColor, COLORREF accentColor, bool hasActive, double progress, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float trayWidth = 10.0f * k;
    const float trayHeight = 2.4f * k;
    const float topY = cy - 1.5f * k;
    const float arrowSize = 3.6f * k;

    Gdiplus::Pen pen(toGdiColor(baseColor), std::max(1.4f, 1.6f * k));
    g.DrawLine(&pen, cx - trayWidth, topY, cx + trayWidth, topY);

    Gdiplus::GraphicsPath arrow;
    arrow.AddLine(cx - arrowSize, topY + 0.5f * k, cx, topY + 0.5f * k + arrowSize);
    arrow.AddLine(cx, topY + 0.5f * k + arrowSize, cx + arrowSize, topY + 0.5f * k);
    Gdiplus::SolidBrush brush(toGdiColor(baseColor));
    Gdiplus::GraphicsPath closed;
    closed.AddLine(cx - arrowSize, topY + 0.5f * k, cx - arrowSize + 0.1f * k, topY + 0.5f * k);
    closed.AddLine(cx - arrowSize, topY + 0.5f * k, cx, topY + arrowSize + 0.5f * k);
    closed.AddLine(cx, topY + arrowSize + 0.5f * k, cx + arrowSize, topY + 0.5f * k);
    closed.AddLine(cx + arrowSize, topY + 0.5f * k, cx + arrowSize - 0.1f * k, topY + 0.5f * k);
    closed.CloseFigure();
    g.FillPath(&brush, &closed);

    if (hasActive) {
        const float radius = 7.6f * k;
        Gdiplus::RectF arcRect(cx - radius, cy - radius, radius * 2, radius * 2);
        Gdiplus::GraphicsPath progressPath;
        progressPath.AddArc(arcRect, -90.0f, static_cast<Gdiplus::REAL>(-360.0 * std::clamp(progress, 0.0, 1.0)));
        Gdiplus::Pen accentPen(toGdiColor(accentColor), std::max(1.4f, 1.8f * k));
        g.DrawPath(&accentPen, &progressPath);
    }
}

// A small chevron drawn vertically. `up` flips the orientation.
inline void drawVerticalChevron(Gdiplus::Graphics& g, const RECT& rect, bool up, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float half = 4.0f * k;
    Gdiplus::GraphicsPath path;
    if (up) {
        path.AddLine(cx - half, cy + half * 0.4f, cx, cy - half * 0.4f);
        path.AddLine(cx, cy - half * 0.4f, cx + half, cy + half * 0.4f);
    } else {
        path.AddLine(cx - half, cy - half * 0.4f, cx, cy + half * 0.4f);
        path.AddLine(cx, cy + half * 0.4f, cx + half, cy - half * 0.4f);
    }
    Gdiplus::Pen pen(toGdiColor(color), std::max(1.4f, 1.6f * k));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&pen, &path);
}

// Magnifying-glass icon: open at the find bar's "Find on Page" affordance.
inline void drawMagnifierIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f - 1.0f * k;
    const float cy = (rect.top + rect.bottom) / 2.0f - 1.0f * k;
    const float radius = 4.6f * k;
    Gdiplus::GraphicsPath loop;
    loop.AddEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
    Gdiplus::Pen pen(toGdiColor(color), std::max(1.4f, 1.6f * k));
    g.DrawPath(&pen, &loop);
    Gdiplus::SolidBrush brush(toGdiColor(color));
    Gdiplus::GraphicsPath handle;
    handle.AddLine(cx + radius * 0.7f, cy + radius * 0.7f, cx + radius * 0.7f + 3.5f * k, cy + radius * 0.7f + 3.5f * k);
    pen.SetWidth(std::max(1.6f, 1.8f * k));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx + radius * 0.7f, cy + radius * 0.7f, cx + radius * 0.7f + 3.5f * k, cy + radius * 0.7f + 3.5f * k);
}

} // namespace lumen::gfx
