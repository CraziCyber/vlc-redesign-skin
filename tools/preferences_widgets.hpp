#pragma once

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace preferences_widgets {

struct Colors {
    COLORREF panel, track, edge, text, heading, dim, button, hover, knob, fill, accent;
};
inline constexpr Colors dark{
    RGB(15,15,15), RGB(4,4,4), RGB(38,38,38), RGB(143,143,143), RGB(212,212,212),
    RGB(111,111,111), RGB(27,27,27), RGB(43,43,43), RGB(63,63,63), RGB(55,31,6), RGB(211,121,24)};
inline constexpr Colors light{
    RGB(194,193,193), RGB(222,222,222), RGB(154,154,154), RGB(54,54,54), RGB(20,20,20),
    RGB(82,82,82), RGB(186,186,186), RGB(170,170,170), RGB(139,139,139), RGB(216,186,154), RGB(138,67,0)};

inline void fill(HDC dc, RECT area, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void round_rect(HDC dc, RECT area, int radius, COLORREF color) {
    SetDCBrushColor(dc, color);
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, area.left, area.top, area.right, area.bottom, 2 * radius, 2 * radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}

inline std::wstring wstring(std::string_view text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

inline std::string utf8(const wchar_t *text) {
    const int length = static_cast<int>(std::wcslen(text));
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count)
        WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), count, nullptr, nullptr);
    return result;
}

inline std::wstring window_text(HWND window) {
    std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
    const int count = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(count);
    return text;
}

inline void set_unfocused_text(HWND window, const std::wstring &text) {
    if (GetFocus() != window && window_text(window) != text)
        SetWindowTextW(window, text.c_str());
}

inline std::wstring number_text(double number, int precision, std::wstring_view suffix, bool fixed = false) {
    wchar_t buffer[64];
    std::swprintf(buffer, std::size(buffer), L"%.*f", precision, number);
    std::wstring text(buffer);
    if (!fixed && precision > 0 && text.find(L'.') != std::wstring::npos) {
        while (text.back() == L'0')
            text.pop_back();
        if (text.back() == L'.')
            text.pop_back();
    }
    return text.append(suffix);
}

inline std::wstring_view trim(std::wstring_view text) {
    const auto start = text.find_first_not_of(L" \t");
    if (start == std::wstring_view::npos)
        return {};
    return text.substr(start, text.find_last_not_of(L" \t") - start + 1);
}

inline std::optional<double> parse_number(std::wstring text, std::wstring_view suffix) {
    std::replace(text.begin(), text.end(), L',', L'.');
    wchar_t *end = nullptr;
    const double result = std::wcstod(text.c_str(), &end);
    const auto unit = trim(end);
    if (end == text.c_str() || !std::isfinite(result) || (!unit.empty() && unit != trim(suffix)))
        return std::nullopt;
    return result;
}

inline HFONT make_font(int height, int weight = FW_NORMAL) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

inline void text(HDC dc, const wchar_t *label, RECT box, HFONT font, COLORREF color, UINT align = DT_LEFT) {
    const auto previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, label, -1, &box, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | align);
    SelectObject(dc, previous);
}

inline void draw_toggle(HDC dc, RECT box, bool on, const Colors &colors, HFONT font) {
    round_rect(dc, box, 7, colors.track);
    RECT left{box.left + 3, box.top + 3, box.left + 31, box.bottom - 3};
    RECT right{box.left + 34, box.top + 3, box.right - 3, box.bottom - 3};
    round_rect(dc, left, 4, on ? colors.fill : colors.knob);
    round_rect(dc, right, 4, on ? RGB(211,121,24) : colors.button);
    text(dc, on ? L"ON" : L"OFF", on ? left : right, font, on ? colors.accent : colors.text, DT_CENTER);
}

struct SliderTicks {
    int divisions{};
    COLORREF unfilled{}, filled{};
};

inline LRESULT draw_slider(NMCUSTOMDRAW &drawing, const Colors &colors, bool centered,
                           COLORREF thumb_color = RGB(211,121,24), SliderTicks ticks = {}) {
    if (drawing.dwDrawStage == CDDS_PREPAINT) {
        fill(drawing.hdc, drawing.rc, colors.panel);
        return CDRF_NOTIFYITEMDRAW;
    }
    if (drawing.dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;
    const auto control = drawing.hdr.hwndFrom;
    RECT channel{}, handle{};
    SendMessageW(control, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(control, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&handle));
    const LONG x = (handle.left + handle.right) / 2, y = (handle.top + handle.bottom) / 2;
    const bool enabled = IsWindowEnabled(control);
    if (drawing.dwItemSpec == TBCD_THUMB) {
        round_rect(drawing.hdc, {x - 4, y - 12, x + 5, y + 12}, 4,
                   enabled ? thumb_color : colors.knob);
        return CDRF_SKIPDEFAULT;
    }
    if (drawing.dwItemSpec == TBCD_CHANNEL) {
        round_rect(drawing.hdc, {channel.left, y - 8, channel.right, y + 8}, 7, colors.track);
        const LONG origin = centered ? (channel.left + channel.right) / 2 : channel.left;
        if (enabled && origin != x)
            round_rect(drawing.hdc, {std::min(origin, x), y - 6, std::max(origin, x), y + 6}, 3, colors.fill);
        for (int i = 1; i < ticks.divisions; ++i) {
            const LONG tick = channel.left + (channel.right - channel.left) * i / ticks.divisions;
            const bool filled = enabled && tick > std::min(origin, x) && tick < std::max(origin, x);
            fill(drawing.hdc, {tick - 1, y - 4, tick + 1, y + 4}, filled ? ticks.filled : ticks.unfilled);
        }
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

template<class Draw>
void paint(HWND window, const RECT &area, Draw draw) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    const auto memory = CreateCompatibleDC(dc);
    const auto bitmap = memory ? CreateCompatibleBitmap(dc, area.right, area.bottom) : nullptr;
    if (bitmap) {
        const auto original = SelectObject(memory, bitmap);
        draw(memory);
        BitBlt(dc, 0, 0, area.right, area.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, original);
        DeleteObject(bitmap);
    } else {
        draw(dc);
    }
    if (memory)
        DeleteDC(memory);
    EndPaint(window, &paint);
}

inline bool wheel_over(HWND view, const MSG &message) {
    if (message.message != WM_MOUSEWHEEL || message.hwnd == view)
        return false;
    RECT area{};
    GetWindowRect(view, &area);
    return IsChild(view, message.hwnd) || PtInRect(&area, {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)});
}

class Scroll {
public:
    explicit Scroll(int content_height) : content_height_(content_height) {}

    void set_content_height(int height) { content_height_ = height; }
    int offset() const { return offset_; }
    int limit(RECT area) const { return std::max(0, content_height_ - static_cast<int>(area.bottom)); }

    bool move(int y, RECT area) {
        const int next = std::clamp(y, 0, limit(area));
        if (offset_ == next)
            return false;
        offset_ = next;
        return true;
    }

    bool wheel(int delta, RECT area) {
        wheel_ += delta;
        const bool changed = move(offset_ - 56 * (wheel_ / WHEEL_DELTA), area);
        wheel_ %= WHEEL_DELTA;
        return changed;
    }

    RECT thumb(RECT area) const {
        const int height = std::max(1L, area.bottom - 16);
        const int length = std::min(height, std::max(25, height * static_cast<int>(area.bottom) / content_height_));
        const int y = 8 + (height - length) * offset_ / std::max(1, limit(area));
        return {area.right - 7, y, area.right - 3, y + length};
    }

    bool start_drag(HWND window, RECT area, int x, int y) {
        if (!limit(area) || x < area.right - 12)
            return false;
        const auto bar = thumb(area);
        grab_ = y >= bar.top && y < bar.bottom ? y - bar.top : (bar.bottom - bar.top) / 2;
        SetCapture(window);
        return drag(area, y);
    }

    bool drag(RECT area, int y) {
        if (grab_ < 0)
            return false;
        const auto bar = thumb(area);
        const int span = std::max(1L, area.bottom - 16 - (bar.bottom - bar.top));
        return move((y - 8 - grab_) * limit(area) / span, area);
    }

    void end_drag() {
        if (grab_ >= 0)
            ReleaseCapture();
    }

    void capture_lost() { grab_ = -1; }

    bool show(HWND window, HWND control, RECT area) {
        RECT box{};
        GetWindowRect(control, &box);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&box), 2);
        if (box.top < 5)
            return move(offset_ + box.top - 5, area);
        if (box.bottom > area.bottom - 5)
            return move(offset_ + box.bottom - area.bottom + 5, area);
        return false;
    }

private:
    int content_height_, offset_ = 0, wheel_ = 0, grab_ = -1;
};

}
