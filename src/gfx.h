#pragma once

// Theme colors and anti-aliased (GDI+) drawing helpers for the browser chrome.
// Header-only; everything here is inline.

#include <windows.h>

// GDI+ uses min/max, which NOMINMAX removes, so provide them.
#include <algorithm>
#include <cmath>
// drawLabelGdiPlus and measureLabelGdiPlus take std::wstring. main.cpp includes
// this header before any of <string>, <cwchar> or the browser headers, so the
// dependency has to be declared here rather than inherited.
#include <string>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <objidl.h>
#include <gdiplus.h>
// gdiplus.h already pulls gdiplusenums.h in, but it is named explicitly here
// because the helpers below name enumerators from it (StringAlignment,
// StringTrimming). If a future SDK ever stops including it transitively, the
// failure lands in this header rather than in a caller.
#include <gdiplusenums.h>

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

// The GDI+ twin of fillSolid, for panels that mix rounded-rect fills with GDI
// text (see Browser::drawLabel). Use this INSTEAD of fillSolid whenever a
// Gdiplus::Graphics is already open on the same DC.
//
// Mixing GDI and GDI+ on one DC is not supported - GDI+ takes the DC over
// when a Graphics is constructed from an HDC and restores DC state of its own
// choosing when it flushes. The observable result here was that DrawTextW
// silently drew nothing at all: the sidebar's three segment labels came out as
// an empty pill, with not one anti-aliased glyph pixel anywhere in the control,
// while "No Bookmarks" a few lines above - same helper, same DC, same frame,
// same colour for two of the three - rendered perfectly. The only thing
// between the two was a raw fillSolid call.
//
// paintChrome never hit this because every one of its fillSolid calls happens
// BEFORE the Graphics is constructed. paintSidebar and paintDownloadsPanel both
// construct theirs first and fill afterwards, which is why the broken labels in
// each are exactly the ones drawn after the last fillSolid: the sidebar's three
// segment labels and its "Clear History", the panel's "Downloads" and "Clear".
//
// Declared after toRectF/toGdiColor because it uses both.
inline void fillSolidGdiPlus(Gdiplus::Graphics& g, const RECT& rect, COLORREF color)
{
    Gdiplus::SolidBrush brush(toGdiColor(color));
    g.FillRectangle(&brush, toRectF(rect));
}

// ---------------------------------------------------------------------------
// GDI+ text, for text that has to live under a Graphics transform.
//
// drawLabel uses GDI's DrawTextW, which draws in DC space and therefore
// IGNORES any Gdiplus::Transform on the same Graphics. That is fine for static
// chrome, but the tab overview animates each card with a scale and a rise, and
// a GDI-drawn title sitting inside a scaled card does not scale with it - the
// label stays put at full size while the picture around it grows.
//
// So the overview's card titles use these instead. The font is reconstructed
// from the same HFONT drawLabel uses, so the two paths stay visually identical
// at rest; only the coordinate space differs.
// ---------------------------------------------------------------------------

inline Gdiplus::Font fontFromHFont(HFONT font)
{
    LOGFONTW lf { };
    if (!GetObjectW(font, sizeof(lf), &lf))
        return Gdiplus::Font(L"Segoe UI", 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    Gdiplus::FontStyle style = lf.lfWeight >= FW_BOLD
        ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
    // Gdiplus::FontStyle is a plain unscoped enum, so there is no operator|= for
    // two enumerators - C2678. Round-trip through int, which is what the
    // built-in |= on the promoted type would have done anyway.
    if (lf.lfItalic)
        style = static_cast<Gdiplus::FontStyle>(
            static_cast<int>(style) | static_cast<int>(Gdiplus::FontStyleItalic));

    // lfHeight is a negative character height and a positive cell height;
    // GDI+ always wants a positive em size in pixels.
    const int emSize = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
    return Gdiplus::Font(lf.lfFaceName, static_cast<Gdiplus::REAL>(emSize),
        style, Gdiplus::UnitPixel);
}

// Draws left-aligned, vertically centred, on one line, ellipsised if it does
// not fit - i.e. the DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
// DT_END_ELLIPSIS case that drawLabel handles.
//
// The alpha is per-card, so the overview's cascade can fade a title in with the
// rest of the card it belongs to. Text on a Graphics with a transform cannot be
// faded any other way: DrawString is the only call here that does not take a
// second Graphics to draw through, so the colour matrix trick used for the
// thumbnail bitmap is not available.
inline void drawLabelGdiPlus(Gdiplus::Graphics& g, HFONT font,
    const std::wstring& text, const Gdiplus::RectF& rect, COLORREF color,
    BYTE alpha = 255)
{
    Gdiplus::Font f = fontFromHFont(font);
    Gdiplus::SolidBrush brush(toGdiColor(color, alpha));
    Gdiplus::StringFormat format;
    // Same three setters drawSiteBadge already uses at the bottom of this file -
    // that call is proven to compile, so there is no new GDI+ surface here.
    format.SetAlignment(Gdiplus::StringAlignmentNear);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), -1, &f, rect, &format, &brush);
}

// Width the text would take with no limit, so the caller can compare it against
// its available box.
//
// There is deliberately no StringFormat here, and that is forced rather than
// stylistic. DrawString takes one - see drawLabelGdiPlus - but this SDK's
// Graphics::MeasureString has no overload that accepts a StringFormat next to a
// PointF, so the five-argument call that reads the most naturally is C2665. The
// two functions do not have to offer the same overload set.
//
// The form used below is the one the compiler itself printed when it rejected
// the other, so it is known to exist rather than merely documented. The PointF
// variant is chosen over the layout-rect variant because with an origin there
// is no layout width to wrap against, making this the natural single-line width
// - which is the only number the caller wants. The ellipsis itself is drawLabel-
// GdiPlus's job, not this one's.
inline Gdiplus::SizeF measureLabelGdiPlus(Gdiplus::Graphics& g, HFONT font,
    const std::wstring& text)
{
    Gdiplus::Font f = fontFromHFont(font);
    Gdiplus::RectF bounds;
    g.MeasureString(text.c_str(), -1, &f, Gdiplus::PointF(0.0f, 0.0f), &bounds);
    return bounds.GetSize();
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

// Two filled bars, the pause glyph on the downloads row.
inline void drawPauseIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::SolidBrush brush(toGdiColor(color));
    const float w = 1.7f * k;
    const float h = 5.5f * k;
    const float gap = 1.9f * k;
    const Gdiplus::RectF left(cx - gap / 2.0f - w, cy - h / 2.0f, w, h);
    const Gdiplus::RectF right(cx + gap / 2.0f, cy - h / 2.0f, w, h);
    g.FillRectangle(&brush, left);
    g.FillRectangle(&brush, right);
}

// The matching triangle. Filled rather than outlined so it reads at the same
// 13px the pause bars do.
inline void drawPlayIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float s = 4.8f * k;

    const Gdiplus::PointF points[3] = {
        Gdiplus::PointF(cx - s * 0.42f, cy),
        Gdiplus::PointF(cx + s * 0.58f, cy - s),
        Gdiplus::PointF(cx + s * 0.58f, cy + s),
    };

    Gdiplus::SolidBrush brush(toGdiColor(color));
    Gdiplus::GraphicsPath path;
    path.AddPolygon(points, 3);
    g.FillPath(&brush, &path);
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
// The alpha covers the letter as well as the tile - the tab overview fades both
// together, and a hard-coded white glyph left at full strength while the tile
// under it dissolves reads as a pop-in.
inline void drawSiteBadge(Gdiplus::Graphics& g, const RECT& rect, wchar_t letter, COLORREF color, float k, BYTE alpha = 255)
{
    fillRoundedRect(g, rect, static_cast<int>(3.5f * k), color, alpha);

    wchar_t text[2] = { letter, 0 };
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(rect.bottom - rect.top) * 0.62f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, 255, 255, 255));
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

// Settings affordance: an outlined cog with notches around the rim and a
// hole in the middle, drawn in the same visual idiom as the other chrome
// icons.
inline void drawGearIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, COLORREF holeColor, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float outer = 8.2f * k;
    const float inner = 5.6f * k;
    const float hole = 2.6f * k;
    const int teeth = 8;

    Gdiplus::GraphicsPath path;
    for (int i = 0; i < teeth; ++i) {
        const float a0 = (static_cast<float>(i) / teeth) * 360.0f - 90.0f;
        const float a1 = ((static_cast<float>(i) + 0.5f) / teeth) * 360.0f - 90.0f;
        const float a2 = ((static_cast<float>(i) + 1.0f) / teeth) * 360.0f - 90.0f;
        const float r0x = cx + std::cos(a0 * 3.14159265f / 180.0f) * (outer - 1.4f * k);
        const float r0y = cy + std::sin(a0 * 3.14159265f / 180.0f) * (outer - 1.4f * k);
        const float r1x = cx + std::cos(a1 * 3.14159265f / 180.0f) * outer;
        const float r1y = cy + std::sin(a1 * 3.14159265f / 180.0f) * outer;
        const float r2x = cx + std::cos(a2 * 3.14159265f / 180.0f) * (outer - 1.4f * k);
        const float r2y = cy + std::sin(a2 * 3.14159265f / 180.0f) * (outer - 1.4f * k);
        path.AddLine(r0x, r0y, r1x, r1y);
        path.AddLine(r1x, r1y, r2x, r2y);
    }
    path.CloseFigure();

    Gdiplus::SolidBrush brush(toGdiColor(color));
    g.FillPath(&brush, &path);

    Gdiplus::Pen pen(toGdiColor(color), std::max(1.4f, 1.6f * k));
    Gdiplus::GraphicsPath ringPath;
    ringPath.AddEllipse(cx - inner, cy - inner, inner * 2.0f, inner * 2.0f);
    g.DrawPath(&pen, &ringPath);

    Gdiplus::SolidBrush holeBrush(toGdiColor(holeColor));
    g.FillEllipse(&holeBrush, cx - hole, cy - hole, hole * 2.0f, hole * 2.0f);
}

// Circled down arrow with a progress ring while downloads are active.
inline void drawDownloadsIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, COLORREF accent, bool active, double progress, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float r = 8.5f * k;
    const Gdiplus::RectF ringBounds(cx - r, cy - r, r * 2.0f, r * 2.0f);

    Gdiplus::Pen ring(toGdiColor(color, active ? 70 : 255), 1.6f * k);
    configureIconPen(ring);
    g.DrawEllipse(&ring, ringBounds);

    if (active && progress > 0.0) {
        Gdiplus::Pen arc(toGdiColor(accent), 2.0f * k);
        configureIconPen(arc);
        g.DrawArc(&arc, ringBounds, -90.0f, static_cast<Gdiplus::REAL>(360.0 * progress));
    }

    Gdiplus::Pen pen(toGdiColor(color), 1.6f * k);
    configureIconPen(pen);
    g.DrawLine(&pen, cx, cy - 3.5f * k, cx, cy + 3.5f * k);
    Gdiplus::PointF head[3] = {
        Gdiplus::PointF(cx - 3.0f * k, cy + 0.6f * k),
        Gdiplus::PointF(cx, cy + 3.8f * k),
        Gdiplus::PointF(cx + 3.0f * k, cy + 0.6f * k)
    };
    g.DrawLines(&pen, head, 3);
}

inline void drawMagnifierIcon(Gdiplus::Graphics& g, const RECT& rect, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.6f * k);
    configureIconPen(pen);
    g.DrawEllipse(&pen, Gdiplus::RectF(cx - 5.0f * k, cy - 5.0f * k, 7.5f * k, 7.5f * k));
    g.DrawLine(&pen, cx + 1.2f * k, cy + 1.2f * k, cx + 5.0f * k, cy + 5.0f * k);
}

inline void drawVerticalChevron(Gdiplus::Graphics& g, const RECT& rect, bool up, COLORREF color, float k)
{
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float dir = up ? 1.0f : -1.0f;

    Gdiplus::Pen pen(toGdiColor(color), 1.9f * k);
    configureIconPen(pen);
    Gdiplus::PointF points[3] = {
        Gdiplus::PointF(cx - 5.0f * k, cy + dir * 2.5f * k),
        Gdiplus::PointF(cx, cy - dir * 2.5f * k),
        Gdiplus::PointF(cx + 5.0f * k, cy + dir * 2.5f * k)
    };
    g.DrawLines(&pen, points, 3);
}

} // namespace lumen::gfx
