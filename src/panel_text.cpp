#include "panel_text.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kOriginalAtlasResource = 101;
constexpr UINT kFusionPixelFontResource = 201;
const LPCWSTR kRcDataResource = MAKEINTRESOURCEW(10);

constexpr int kAtlasCellW = 8;
constexpr int kAtlasCellH = 11;
constexpr wchar_t kAtlasCells[] =
    L"ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
constexpr wchar_t kEllipsisGlyph = L'.';

int ClampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

int Scaled(int value, float scale) {
    return std::max(1, (int)std::lround((double)value * scale));
}

int Luma(uint32_t pixel) {
    const int b = (int)(pixel & 0xff);
    const int g = (int)((pixel >> 8) & 0xff);
    const int r = (int)((pixel >> 16) & 0xff);
    return (r * 77 + g * 150 + b * 29) >> 8;
}

int AtlasIndex(wchar_t ch) {
    if (ch >= L'a' && ch <= L'z') ch = (wchar_t)(ch - L'a' + L'A');
    const wchar_t* found = std::find(std::begin(kAtlasCells),
                                     std::end(kAtlasCells) - 1, ch);
    if (found != std::end(kAtlasCells) - 1)
        return (int)(found - kAtlasCells);
    return -1; // Missing punctuation uses its real glyph in the companion font.
}

bool IsAtlasCharacter(wchar_t ch) {
    return ch >= L' ' && ch <= L'~' && AtlasIndex(ch) >= 0;
}

struct OriginalAtlas {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;

    OriginalAtlas() {
        bitmap = (HBITMAP)LoadImageW(GetModuleHandleW(nullptr),
                                     MAKEINTRESOURCEW(kOriginalAtlasResource),
                                     IMAGE_BITMAP, 0, 0,
                                     LR_CREATEDIBSECTION);
        if (!bitmap) return;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) {
            DeleteObject(bitmap);
            bitmap = nullptr;
            return;
        }
        SelectObject(dc, bitmap);
    }

    ~OriginalAtlas() {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
    }
};

HDC AtlasForCall(HDC atlas) {
    if (atlas) return atlas;
    static OriginalAtlas original;
    return original.dc;
}

struct FontRuntime {
    HANDLE memoryFont = nullptr;
    std::wstring faceName = L"Microsoft YaHei UI";
    std::unordered_map<int, HFONT> fonts;

    FontRuntime() {
        HRSRC resource = FindResourceW(GetModuleHandleW(nullptr),
                                       MAKEINTRESOURCEW(kFusionPixelFontResource),
                                       kRcDataResource);
        if (!resource) return;
        HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
        if (!loaded) return;
        void* bytes = LockResource(loaded);
        const DWORD bytesSize = SizeofResource(GetModuleHandleW(nullptr), resource);
        if (!bytes || bytesSize == 0) return;

        DWORD installed = 0;
        memoryFont = AddFontMemResourceEx(bytes, bytesSize, nullptr, &installed);
        if (memoryFont && installed != 0)
            faceName = L"Fusion Pixel 12px Mono zh-Hans";
    }

    ~FontRuntime() {
        for (auto& entry : fonts) {
            if (entry.second) DeleteObject(entry.second);
        }
        if (memoryFont) RemoveFontMemResourceEx(memoryFont);
    }

    HFONT GetFont(int pixelHeight) {
        pixelHeight = std::max(1, pixelHeight);
        auto found = fonts.find(pixelHeight);
        if (found != fonts.end()) return found->second;

        LOGFONTW lf = {};
        lf.lfHeight = -pixelHeight;
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = NONANTIALIASED_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpynW(lf.lfFaceName, faceName.c_str(), LF_FACESIZE);

        HFONT font = CreateFontIndirectW(&lf);
        fonts.emplace(pixelHeight, font);
        return font;
    }
};

FontRuntime& Fonts() {
    static FontRuntime runtime;
    return runtime;
}

struct AtlasMask {
    HBITMAP source = nullptr;
    int width = 0;
    int columns = 0;
    std::vector<uint8_t> ink;
};

AtlasMask* GetAtlasMask(HDC atlas) {
    static std::unordered_map<HBITMAP, AtlasMask> cache;
    if (!atlas) return nullptr;

    HBITMAP source = (HBITMAP)GetCurrentObject(atlas, OBJ_BITMAP);
    if (!source) return nullptr;
    auto existing = cache.find(source);
    if (existing != cache.end()) return &existing->second;

    BITMAP bitmap = {};
    if (!GetObjectW(source, sizeof(bitmap), &bitmap) ||
        bitmap.bmWidth < kAtlasCellW || bitmap.bmHeight < kAtlasCellH) {
        return nullptr;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bitmap.bmWidth;
    info.bmiHeader.biHeight = -bitmap.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> pixels((size_t)bitmap.bmWidth * bitmap.bmHeight);
    if (GetDIBits(atlas, source, 0, (UINT)bitmap.bmHeight, pixels.data(),
                  &info, DIB_RGB_COLORS) != bitmap.bmHeight) {
        HDC screen = GetDC(nullptr);
        const int copied = screen
            ? GetDIBits(screen, source, 0, (UINT)bitmap.bmHeight, pixels.data(),
                        &info, DIB_RGB_COLORS)
            : 0;
        if (screen) ReleaseDC(nullptr, screen);
        if (copied != bitmap.bmHeight) return nullptr;
    }

    AtlasMask result;
    result.source = source;
    result.width = bitmap.bmWidth;
    result.columns = bitmap.bmWidth / kAtlasCellW;
    result.ink.assign((size_t)result.columns * kAtlasCellW * kAtlasCellH, 0);

    const int sourceRow = bitmap.bmHeight >= 4*kAtlasCellH ? 3*kAtlasCellH : 0; // plain, no baked underline
    for (int column = 0; column < result.columns; ++column) {
        std::unordered_map<uint32_t, int> histogram;
        for (int y = 0; y < kAtlasCellH; ++y) {
            for (int x = 0; x < kAtlasCellW; ++x) {
                const uint32_t pixel =
                    pixels[(size_t)(sourceRow+y) * bitmap.bmWidth + column * kAtlasCellW + x];
                ++histogram[pixel & 0x00ffffffu];
            }
        }

        uint32_t background = 0;
        int backgroundCount = -1;
        for (const auto& entry : histogram) {
            if (entry.second > backgroundCount) {
                background = entry.first;
                backgroundCount = entry.second;
            }
        }

        const int backgroundLuma = Luma(background);
        const bool darkInk = backgroundLuma >= 128;
        for (int y = 0; y < kAtlasCellH; ++y) {
            for (int x = 0; x < kAtlasCellW; ++x) {
                const size_t sourceOffset =
                    (size_t)(sourceRow+y) * bitmap.bmWidth + column * kAtlasCellW + x;
                const int pixelLuma = Luma(pixels[sourceOffset] & 0x00ffffffu);
                const int delta = darkInk
                    ? backgroundLuma - pixelLuma
                    : pixelLuma - backgroundLuma;
                if (delta < 48) continue;

                const size_t targetOffset =
                    (size_t)y * ((size_t)result.columns * kAtlasCellW) +
                    (size_t)column * kAtlasCellW + x;
                result.ink[targetOffset] = 0xff;
            }
        }
    }

    return &cache.emplace(source, std::move(result)).first->second;
}

struct StripKey {
    HBITMAP atlas = nullptr;
    int cellWidth = 0;
    int cellHeight = 0;
    COLORREF color = 0;

    bool operator==(const StripKey& other) const {
        return atlas == other.atlas && cellWidth == other.cellWidth &&
               cellHeight == other.cellHeight && color == other.color;
    }
};

struct StripKeyHash {
    size_t operator()(const StripKey& key) const {
        size_t value = std::hash<void*>()((void*)key.atlas);
        value ^= (size_t)key.cellWidth + 0x9e3779b9u + (value << 6) + (value >> 2);
        value ^= (size_t)key.cellHeight + 0x9e3779b9u + (value << 6) + (value >> 2);
        value ^= (size_t)key.color + 0x9e3779b9u + (value << 6) + (value >> 2);
        return value;
    }
};

struct ColoredStrip {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    int columns = 0;
    int cellWidth = 0;
    int cellHeight = 0;

    ColoredStrip() = default;
    ColoredStrip(const ColoredStrip&) = delete;
    ColoredStrip& operator=(const ColoredStrip&) = delete;

    ~ColoredStrip() {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
    }
};

ColoredStrip* GetColoredStrip(const AtlasMask& mask, COLORREF color, float scale) {
    static std::unordered_map<StripKey, std::unique_ptr<ColoredStrip>, StripKeyHash>
        cache;
    static std::deque<StripKey> age;

    StripKey key;
    key.atlas = mask.source;
    key.cellWidth = Scaled(kAtlasCellW, scale);
    key.cellHeight = Scaled(kAtlasCellH, scale);
    key.color = color;

    auto existing = cache.find(key);
    if (existing != cache.end()) return existing->second.get();

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = mask.columns * key.cellWidth;
    info.bmiHeader.biHeight = -key.cellHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits,
                                      nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return nullptr;
    }
    SelectObject(dc, bitmap);

    const uint32_t argb =
        0xff000000u | ((uint32_t)GetRValue(color) << 16) |
        ((uint32_t)GetGValue(color) << 8) | (uint32_t)GetBValue(color);
    uint32_t* destination = (uint32_t*)bits;
    std::fill(destination,
              destination + (size_t)mask.columns * key.cellWidth * key.cellHeight,
              0);

    for (int y = 0; y < key.cellHeight; ++y) {
        const int sourceY = std::min(kAtlasCellH - 1,
                                     y * kAtlasCellH / key.cellHeight);
        for (int x = 0; x < key.cellWidth; ++x) {
            const int sourceX = std::min(kAtlasCellW - 1,
                                         x * kAtlasCellW / key.cellWidth);
            for (int column = 0; column < mask.columns; ++column) {
                const size_t sourceOffset =
                    (size_t)sourceY * (mask.columns * kAtlasCellW) +
                    (size_t)column * kAtlasCellW + sourceX;
                if (mask.ink[sourceOffset])
                    destination[(size_t)y * mask.columns * key.cellWidth +
                                (size_t)column * key.cellWidth + x] = argb;
            }
        }
    }

    std::unique_ptr<ColoredStrip> strip(new ColoredStrip);
    strip->dc = dc;
    strip->bitmap = bitmap;
    strip->columns = mask.columns;
    strip->cellWidth = key.cellWidth;
    strip->cellHeight = key.cellHeight;

    ColoredStrip* result = strip.get();
    cache.emplace(key, std::move(strip));
    age.push_back(key);
    while (age.size() > 24) {
        cache.erase(age.front());
        age.pop_front();
    }
    return result;
}

struct TextUnit {
    size_t offset = 0;
    int length = 0;
    int width = 0;
    int atlasIndex = -1;
};

std::vector<TextUnit> BuildUnits(HDC dc, const std::wstring& text,
                                 int atlasCellWidth, bool useAtlas) {
    std::vector<TextUnit> units;
    units.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        wchar_t ch = text[i];
        int length = 1;
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < text.size() &&
            text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
            length = 2;
            ch = 0;
        }

        TextUnit unit;
        unit.offset = i;
        unit.length = length;
        if (length == 1 && IsAtlasCharacter(ch) && useAtlas) {
            unit.width = atlasCellWidth;
            unit.atlasIndex = AtlasIndex(ch);
        } else {
            SIZE extent = {};
            GetTextExtentPoint32W(dc, text.c_str() + i, length, &extent);
            unit.width = std::max(0, (int)extent.cx);
        }
        units.push_back(unit);
        i += (size_t)length;
    }
    return units;
}

int64_t UnitsWidth(const std::vector<TextUnit>& units, size_t count) {
    int64_t width = 0;
    for (size_t i = 0; i < count; ++i) width += units[i].width;
    return width;
}

} // namespace

void DrawPanelText(HDC dc, RECT pixelRect, const std::wstring& text,
                   COLORREF color, UINT align, float scale, HDC atlas,
                   PanelTextMode mode) {
    if (!dc || text.empty() || pixelRect.right <= pixelRect.left ||
        pixelRect.bottom <= pixelRect.top) {
        return;
    }
    if (!std::isfinite(scale) || scale <= 0.0f) scale = 1.0f;
    if (color == CLR_INVALID) color = GetTextColor(dc);

    const int atlasCellWidth = Scaled(kAtlasCellW, scale);
    const int atlasCellHeight = Scaled(kAtlasCellH, scale);
    const int cjkPixelHeight = Scaled(12, scale);
    HFONT font = Fonts().GetFont(cjkPixelHeight);
    HDC effectiveAtlas = AtlasForCall(atlas);
    AtlasMask* mask = mode == PanelTextMode::AtlasFirst
        ? GetAtlasMask(effectiveAtlas) : nullptr;
    ColoredStrip* strip = mask ? GetColoredStrip(*mask, color, scale) : nullptr;
    const bool useAtlas = strip != nullptr;

    const int saved = SaveDC(dc);
    SetMapMode(dc, MM_TEXT);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetTextColor(dc, color);
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    IntersectClipRect(dc, pixelRect.left, pixelRect.top,
                      pixelRect.right, pixelRect.bottom);

    TEXTMETRICW metrics = {};
    if (font) GetTextMetricsW(dc, &metrics);
    const int cjkHeight = std::max(0, (int)metrics.tmHeight);

    std::vector<TextUnit> units =
        BuildUnits(dc, text, atlasCellWidth, useAtlas);
    const int64_t available = (int64_t)pixelRect.right - pixelRect.left;
    const int64_t naturalWidth = UnitsWidth(units, units.size());

    size_t shown = units.size();
    int ellipsisCount = 0;
    int64_t shownWidth = naturalWidth;
    int64_t ellipsisWidth = 0;
    if (naturalWidth > available) {
        if (useAtlas) {
            ellipsisCount = ClampInt((int)(available / atlasCellWidth), 0, 3);
            ellipsisWidth = (int64_t)ellipsisCount * atlasCellWidth;
        } else if (font) {
            SIZE extent = {};
            GetTextExtentPoint32W(dc, L"...", 3, &extent);
            ellipsisCount = 3;
            ellipsisWidth = std::max(0, (int)extent.cx);
        }
        if (ellipsisCount == 0) {
            if (oldFont) SelectObject(dc, oldFont);
            if (saved) RestoreDC(dc, saved);
            return;
        }
        shownWidth = 0;
        shown = 0;
        while (shown < units.size() &&
               shownWidth + units[shown].width + ellipsisWidth <= available) {
            shownWidth += units[shown].width;
            ++shown;
        }
        while (shown > 0 && shownWidth > 0) {
            const TextUnit& last = units[shown - 1];
            if (last.length != 1 || text[last.offset] != L' ') break;
            shownWidth -= last.width;
            --shown;
        }
    }

    const int64_t finalWidth = shownWidth + ellipsisWidth;
    int64_t x = pixelRect.left;
    if (align & DT_RIGHT)
        x = pixelRect.right - finalWidth;
    else if (align & DT_CENTER)
        x = pixelRect.left + (available - finalWidth) / 2;

    const int atlasY = pixelRect.top +
        ((pixelRect.bottom - pixelRect.top) - atlasCellHeight) / 2;
    const int cjkY = pixelRect.top +
        ((pixelRect.bottom - pixelRect.top) - cjkHeight) / 2;

    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    for (size_t i = 0; i < shown;) {
        if (useAtlas && units[i].atlasIndex >= 0 && strip &&
            units[i].atlasIndex < strip->columns) {
            AlphaBlend(dc, (int)x, atlasY, atlasCellWidth, atlasCellHeight,
                       strip->dc,
                       units[i].atlasIndex * atlasCellWidth, 0,
                       atlasCellWidth, atlasCellHeight, blend);
            x += units[i].width;
            ++i;
            continue;
        }

        size_t end = i + 1;
        while (end < shown && units[end].atlasIndex < 0) ++end;
        const size_t beginOffset = units[i].offset;
        const TextUnit& last = units[end - 1];
        const int length = (int)(last.offset + last.length - beginOffset);
        if (font)
            TextOutW(dc, (int)x, cjkY, text.c_str() + beginOffset, length);
        x += (int64_t)UnitsWidth(units, end) -
             (int64_t)UnitsWidth(units, i);
        i = end;
    }

    if (ellipsisCount > 0) {
        if (useAtlas && strip) {
            const int dotIndex = AtlasIndex(kEllipsisGlyph);
            for (int i = 0; i < ellipsisCount; ++i) {
                AlphaBlend(dc, (int)x, atlasY, atlasCellWidth, atlasCellHeight,
                           strip->dc, dotIndex * atlasCellWidth, 0,
                           atlasCellWidth, atlasCellHeight, blend);
                x += atlasCellWidth;
            }
        } else if (font) {
            TextOutW(dc, (int)x, cjkY, L"...", 3);
        }
    }

    if (oldFont) SelectObject(dc, oldFont);
    if (saved) RestoreDC(dc, saved);
}

void DrawCodepointLine(HDC dc, const std::uint32_t* cps, int count,
                       RECT rect, COLORREF color, int align) {
    if (!dc || !cps || count <= 0 || rect.right <= rect.left) return;
    std::wstring text;
    text.reserve(count);
    for (int i = 0; i < count; ++i) {
        const std::uint32_t cp = cps[i];
        if (cp <= 0xFFFF) text.push_back((wchar_t)cp);
        else {
            const std::uint32_t v = cp - 0x10000;
            text.push_back((wchar_t)(0xD800 + (v >> 10)));
            text.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
        }
    }
    HFONT font = Fonts().GetFont(12);
    const int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    SIZE size = {};
    if (font) GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
    int x = rect.left;
    if (align == 1) x = rect.left + (rect.right - rect.left - size.cx) / 2;
    else if (align == 2) x = rect.right - size.cx;
    const int y = rect.top + (rect.bottom - rect.top - size.cy) / 2;
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    if (font) TextOutW(dc, x, y, text.c_str(), (int)text.size());
    if (oldFont) SelectObject(dc, oldFont);
    if (saved) RestoreDC(dc, saved);
}
