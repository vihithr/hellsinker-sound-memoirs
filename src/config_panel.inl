// System configuration panel. This file is included at the former
// settingsPaint() location in ui.cpp and deliberately relies on its scope.
void settingsPaint(HDC dc) {
    constexpr int panelL = 144, panelT = 249, panelR = 495, panelB = 475;
    constexpr int stripL = 154, stripR = 486;

    const auto& theme=Th();
    const COLORREF configInk=theme.panel, frameInk=theme.frame;
    const COLORREF accent=theme.bar, helpInk=theme.ink;
    const COLORREF stripBase=LerpCol(theme.panel,theme.strip,.16f);
    const COLORREF stripEdge=LerpCol(theme.panel,theme.frame,.45f);
    const COLORREF stripShade=theme.seam, buttonInk=theme.outer;

    auto localized = [](const wchar_t* en, const wchar_t* zh,
                        const wchar_t* ja) {
        return std::wstring(tr(en, zh, ja));
    };
    auto asciiWide = [](const char* value) {
        std::wstring result;
        while (*value) result.push_back(static_cast<wchar_t>(*value++));
        return result;
    };

    panelBox(dc, panelL, panelT, panelR, panelB, configInk);
    panelBox(dc, panelL, panelT, panelR, panelT + 1, frameInk);
    panelBox(dc, panelL, panelT + 1, panelL + 1, panelB, frameInk);
    panelBox(dc, panelR - 1, panelT + 1, panelR, panelB, frameInk);
    panelBox(dc, panelL, panelB - 1, panelR, panelB, frameInk);

    lineText(dc, 154, 249, 285, 271,
             localized(L"SETTINGS", L"系统设置", L"設定"),
             helpInk);
    panelBox(dc,154,268,355,269,stripEdge);

    const std::wstring labels[] = {
        localized(L"OUTPUT LEVEL", L"输出音量", L"出力音量"),
        localized(L"PALETTE", L"画面配色", L"配色"),
        localized(L"LANGUAGE", L"界面语言", L"言語"),
        localized(L"LIB FONT", L"曲库字体", L"ライブラリ書体")
    };
    std::wstring values[kSettingCount];
    values[0] = std::to_wstring(g_app.settings->volume) + L"%";
    values[1] = asciiWide(Th().name);
    values[2] = lib::language() == lib::Lang::ZH ? L"简体中文" :
                lib::language() == lib::Lang::JA ? L"日本語" : L"ENGLISH";
    values[3] = g_app.settings->libFont == 1 ? L"PIXEL" : L"ORIGINAL";

    for (int row = 0; row < kSettingCount; ++row) {
        const int y = kSettingY0 + kSettingPitch * row;
        const int top = y + 8, bottom = y + 38;
        const bool active = row == settingRow;
        const COLORREF thisStrip = active ?
            LerpCol(stripBase, theme.stripHot, 0.15f) : stripBase;

        if (active) {
            panelBox(dc, stripL, top - 3, stripR, top - 1, theme.frame);
            panelBox(dc, stripL, bottom, stripR, bottom + 2, theme.frame);
        }
        panelBox(dc, stripL, top, stripR, bottom, thisStrip);
        panelBox(dc, stripL, top, stripR, top + 1, stripEdge);
        panelBox(dc, stripL, bottom - 1, stripR, bottom, stripShade);

        // A tiny cap and short red rule read as an instrument label without
        // adding another competing border around the row.
        panelBox(dc, 156, y + 15, 158, y + 29, accent);
        lineText(dc, 161, y + 10, 260, y + 29, labels[row], theme.ink);
        panelBox(dc, 161, y + 31, 248, y + 33, accent);

        const COLORREF buttonEdge = active ? theme.frame :
            LerpCol(frameInk, Th().frame, 0.18f);
        panelBox(dc, 269, y + 12, 294, y + 33, buttonInk);
        panelBox(dc, 269, y + 12, 294, y + 13, buttonEdge);
        panelBox(dc, 269, y + 32, 294, y + 33, stripShade);
        panelBox(dc, 462, y + 12, 485, y + 33, buttonInk);
        panelBox(dc, 462, y + 12, 485, y + 13, buttonEdge);
        panelBox(dc, 462, y + 32, 485, y + 33, stripShade);
        for(int side=0;side<2;++side) {
            int x=side?473:281, mid=y+22;
            if(row==0) {
                panelBox(dc,x-4,mid-1,x+5,mid+1,theme.ink);
                if(side) panelBox(dc,x,mid-4,x+2,mid+5,theme.ink);
            } else {
                for(int d=-3;d<=3;++d) {
                    int px=x+(side?1:-1)*(2-abs(d));
                    panelBox(dc,px,mid+d,px+2,mid+d+1,theme.ink);
                }
            }
        }

        lineText(dc, 303, y + 9, 454, y + 30, values[row],
                 theme.digit,
                 DT_RIGHT);

        if (row == 0) {
            const int volume = g_app.settings->volume;
            for (int seg = 0; seg < 20; ++seg) {
                const int sx = 304 + seg * 7;
                panelBox(dc, sx, y + 31, sx + 4, y + 33,
                         seg * 5 < volume ? Th().bar :
                         LerpCol(thisStrip, configInk, 0.38f));
            }
        } else if (row == 1) {
            const COLORREF swatches[] = {
                Th().frame, Th().digit, Th().bar, Th().selEdge
            };
            for (int swatch = 0; swatch < 4; ++swatch)
                panelBox(dc, 418 + swatch * 9, y + 30,
                         425 + swatch * 9, y + 33, swatches[swatch]);
                }
    }

    const int helpT = 442, helpB = 475;
    panelBox(dc, 154, helpT, 486, helpB, theme.outer);
    panelBox(dc, 154, helpT, 486, helpT + 1, frameInk);
    panelBox(dc, 157, helpT + 5, 160, helpT + 29, theme.frame);

    const wchar_t* help = settingRow == 0 ?
        tr(L"Master output level", L"主音量（最终输出）", L"主音量（最終出力）") :
        settingRow == 1 ?
        tr(L"Interface and signal palette", L"界面与信号配色", L"画面と信号の配色") :
        settingRow == 2 ?
        tr(L"Interface and help language", L"界面与帮助语言", L"表示とヘルプの言語") :
        tr(L"Library track title typeface", L"曲库曲目名的显示字体", L"音楽庫の曲名書体");
    const wchar_t* defaultValue = settingRow == 0 ? L"80%" :
        settingRow == 1 ? L"ORIGINAL" :
        settingRow == 2 ? L"SYSTEM" : L"PIXEL";
    const std::wstring defaultLine =
        localized(L"Default", L"默认值", L"初期値") + L": " + defaultValue;

    lineText(dc, 167, 444, 481, 460, help, helpInk);
    lineText(dc, 167, 458, 481, 474, defaultLine, theme.digit);
}
