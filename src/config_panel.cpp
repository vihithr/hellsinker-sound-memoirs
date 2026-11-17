#include "config_panel.h"

#include "panel_text.h"

#include <string>

namespace {

constexpr int kPanelL = 144;
constexpr int kPanelT = 249;
constexpr int kPanelR = 495;
constexpr int kPanelB = 475;
constexpr int kStripL = 154;
constexpr int kStripR = 486;
constexpr int kSettingY0 = 288;
constexpr int kSettingPitch = 36;

int Scale(int value, float scale) {
    return static_cast<int>(value * scale + 0.5f);
}

void Box(const config_panel::DrawContext& context, int left, int top,
         int right, int bottom, COLORREF color) {
    RECT rect = {
        Scale(left, context.scale),
        Scale(top, context.scale),
        Scale(right, context.scale),
        Scale(bottom, context.scale)
    };
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(context.dc, &rect, brush);
    DeleteObject(brush);
}

void Line(const config_panel::DrawContext& context, int left, int top,
          int right, int bottom, const std::wstring& value, COLORREF color,
          UINT align = DT_LEFT) {
    DrawPanelText(
        context.dc,
        {Scale(left, context.scale), Scale(top, context.scale),
         Scale(right, context.scale), Scale(bottom, context.scale)},
        value, color, align, context.scale, context.atlas,
        PanelTextMode::AtlasFirst);
}

COLORREF Lerp(COLORREF first, COLORREF second, float amount) {
    if (amount <= 0.0f) return first;
    if (amount >= 1.0f) return second;
    return RGB(
        static_cast<int>(GetRValue(first) +
                         (GetRValue(second) - GetRValue(first)) * amount),
        static_cast<int>(GetGValue(first) +
                         (GetGValue(second) - GetGValue(first)) * amount),
        static_cast<int>(GetBValue(first) +
                         (GetBValue(second) - GetBValue(first)) * amount));
}

const wchar_t* Translate(lib::Lang language, const wchar_t* english,
                         const wchar_t* chinese, const wchar_t* japanese) {
    switch (language) {
        case lib::Lang::ZH: return chinese ? chinese : english;
        case lib::Lang::JA: return japanese ? japanese : english;
        case lib::Lang::EN:
        default: return english;
    }
}

std::wstring Localize(lib::Lang language, const wchar_t* english,
                      const wchar_t* chinese, const wchar_t* japanese) {
    return Translate(language, english, chinese, japanese);
}

std::wstring AsciiWide(const char* value) {
    std::wstring result;
    while (value && *value) {
        result.push_back(static_cast<wchar_t>(*value++));
    }
    return result;
}

} // namespace

namespace config_panel {

HitResult HitTest(int logicalX, int logicalY) {
    const int rowTop = kSettingY0;
    const int rowBottom = kSettingY0 + kSettingCount * kSettingPitch + 5;
    if (logicalX < 154 || logicalX >= 486 ||
        logicalY < rowTop || logicalY >= rowBottom) {
        return {-1, HitAction::Select};
    }

    int row = (logicalY - kSettingY0) / kSettingPitch;
    if (row < 0) row = 0;
    if (row >= kSettingCount) row = kSettingCount - 1;

    HitAction action = HitAction::Select;
    if (logicalX >= 268 && logicalX < 296) {
        action = HitAction::Decrease;
    } else if (logicalX >= 461) {
        action = HitAction::Increase;
    }
    return {row, action};
}

void Paint(const DrawContext& context, const State& state) {
    if (!context.dc) return;

    const Palette& palette = context.palette;
    const COLORREF configInk = palette.panel;
    const COLORREF frameInk = palette.frame;
    const COLORREF accent = palette.bar;
    const COLORREF helpInk = palette.ink;
    const COLORREF stripBase =
        Lerp(palette.panel, palette.strip, 0.16f);
    const COLORREF stripEdge =
        Lerp(palette.panel, palette.frame, 0.45f);
    const COLORREF stripShade = palette.seam;
    const COLORREF buttonInk = palette.outer;

    Box(context, kPanelL, kPanelT, kPanelR, kPanelB, configInk);
    Box(context, kPanelL, kPanelT, kPanelR, kPanelT + 1, frameInk);
    Box(context, kPanelL, kPanelT + 1, kPanelL + 1, kPanelB, frameInk);
    Box(context, kPanelR - 1, kPanelT + 1, kPanelR, kPanelB, frameInk);
    Box(context, kPanelL, kPanelB - 1, kPanelR, kPanelB, frameInk);

    Line(context, 154, 249, 285, 271,
         Localize(state.language, L"SETTINGS", L"系统设置", L"設定"),
         helpInk);
    Box(context, 154, 268, 355, 269, stripEdge);

    const std::wstring labels[] = {
        Localize(state.language, L"OUTPUT LEVEL", L"输出音量", L"出力音量"),
        Localize(state.language, L"PALETTE", L"画面配色", L"配色"),
        Localize(state.language, L"LANGUAGE", L"界面语言", L"言語"),
        Localize(state.language, L"LIB FONT", L"曲库字体",
                 L"ライブラリ書体")
    };

    std::wstring values[kSettingCount];
    values[0] = std::to_wstring(state.volume) + L"%";
    values[1] = AsciiWide(state.paletteName);
    values[2] = state.language == lib::Lang::ZH ? L"简体中文" :
                state.language == lib::Lang::JA ? L"日本語" : L"ENGLISH";
    values[3] = state.libraryFont == 1 ? L"PIXEL" : L"ORIGINAL";

    for (int row = 0; row < kSettingCount; ++row) {
        const int y = kSettingY0 + kSettingPitch * row;
        const int top = y + 8;
        const int bottom = y + 38;
        const bool active = row == state.activeRow;
        const COLORREF thisStrip = active
            ? Lerp(stripBase, palette.stripHot, 0.15f)
            : stripBase;

        if (active) {
            Box(context, kStripL, top - 3, kStripR, top - 1, palette.frame);
            Box(context, kStripL, bottom, kStripR, bottom + 2, palette.frame);
        }
        Box(context, kStripL, top, kStripR, bottom, thisStrip);
        Box(context, kStripL, top, kStripR, top + 1, stripEdge);
        Box(context, kStripL, bottom - 1, kStripR, bottom, stripShade);

        // A tiny cap and short red rule read as an instrument label without
        // adding another competing border around the row.
        Box(context, 156, y + 15, 158, y + 29, accent);
        Line(context, 161, y + 10, 260, y + 29, labels[row], palette.ink);
        Box(context, 161, y + 31, 248, y + 33, accent);

        const COLORREF buttonEdge = active
            ? palette.frame
            : Lerp(frameInk, palette.frame, 0.18f);
        Box(context, 269, y + 12, 294, y + 33, buttonInk);
        Box(context, 269, y + 12, 294, y + 13, buttonEdge);
        Box(context, 269, y + 32, 294, y + 33, stripShade);
        Box(context, 462, y + 12, 485, y + 33, buttonInk);
        Box(context, 462, y + 12, 485, y + 13, buttonEdge);
        Box(context, 462, y + 32, 485, y + 33, stripShade);

        for (int side = 0; side < 2; ++side) {
            const int x = side ? 473 : 281;
            const int middle = y + 22;
            if (row == 0) {
                Box(context, x - 4, middle - 1, x + 5, middle + 1,
                    palette.ink);
                if (side) {
                    Box(context, x, middle - 4, x + 2, middle + 5,
                        palette.ink);
                }
            } else {
                for (int distance = -3; distance <= 3; ++distance) {
                    const int magnitude =
                        distance < 0 ? -distance : distance;
                    const int px =
                        x + (side ? 1 : -1) * (2 - magnitude);
                    Box(context, px, middle + distance,
                        px + 2, middle + distance + 1, palette.ink);
                }
            }
        }

        Line(context, 303, y + 9, 454, y + 30, values[row],
             palette.digit, DT_RIGHT);

        if (row == 0) {
            for (int segment = 0; segment < 20; ++segment) {
                const int x = 304 + segment * 7;
                Box(context, x, y + 31, x + 4, y + 33,
                    segment * 5 < state.volume
                        ? palette.bar
                        : Lerp(thisStrip, configInk, 0.38f));
            }
        } else if (row == 1) {
            const COLORREF swatches[] = {
                palette.frame, palette.digit, palette.bar,
                palette.selectedEdge
            };
            for (int swatch = 0; swatch < 4; ++swatch) {
                Box(context, 418 + swatch * 9, y + 30,
                    425 + swatch * 9, y + 33, swatches[swatch]);
            }
        }
    }

    constexpr int helpT = 442;
    constexpr int helpB = 475;
    Box(context, 154, helpT, 486, helpB, palette.outer);
    Box(context, 154, helpT, 486, helpT + 1, frameInk);
    Box(context, 157, helpT + 5, 160, helpT + 29, palette.frame);

    const wchar_t* help =
        state.activeRow == 0
            ? Translate(state.language, L"Master output level",
                        L"主音量（最终输出）", L"主音量（最終出力）")
            : state.activeRow == 1
            ? Translate(state.language, L"Interface and signal palette",
                        L"界面与信号配色", L"画面と信号の配色")
            : state.activeRow == 2
            ? Translate(state.language, L"Interface and help language",
                        L"界面与帮助语言", L"表示とヘルプの言語")
            : Translate(state.language, L"Library track title typeface",
                        L"曲库曲目名的显示字体", L"音楽庫の曲名書体");
    const wchar_t* defaultValue =
        state.activeRow == 0 ? L"80%" :
        state.activeRow == 1 ? L"ORIGINAL" :
        state.activeRow == 2 ? L"SYSTEM" : L"PIXEL";
    const std::wstring defaultLine =
        Localize(state.language, L"Default", L"默认值", L"初期値") +
        L": " + defaultValue;

    Line(context, 167, 444, 481, 460, help, helpInk);
    Line(context, 167, 458, 481, 474, defaultLine, palette.digit);
}

} // namespace config_panel
