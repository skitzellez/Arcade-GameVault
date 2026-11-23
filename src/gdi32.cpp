// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
// GDI text for 32-bit games: the console has no GDI font engine, and games that build their font textures from
// GDI glyphs (Contra: Evolution: AddFontMemResourceEx, CreateFontA, GetGlyphOutlineA) drew no text. Fonts are
// DirectWrite's: the game's own font files (from memory) or the console's; glyphs come out as GDI gives them.
#include <windows.h>
#include <dwrite_3.h>
#include <stdint.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "peload.h"

namespace arcade {
#define P32(p) ((uint8_t*)(uintptr_t)(uint32_t)(p))

static std::mutex g_lock;
static IDWriteFactory5* g_dw = nullptr;
static IDWriteInMemoryFontFileLoader* g_memLoader = nullptr;
static std::vector<IDWriteFontFace*> g_memFaces;   // fonts the game added from memory
struct Font {
    IDWriteFontFace* face = nullptr;
    float em = 16;        // pixels per em
    int weight = 400;
    DWRITE_FONT_METRICS m = {};
};
static std::map<uint32_t, Font> g_fonts;
static std::map<uint32_t, uint32_t> g_dcFont;   // DC -> selected font
static uint32_t g_next = 0x7A100000;

static bool Ready() {
    if (g_dw) return true;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)&g_dw))) return false;
    if (SUCCEEDED(g_dw->CreateInMemoryFontFileLoader(&g_memLoader))) g_dw->RegisterFontFileLoader(g_memLoader);
    return true;
}
static uint64_t G_AddFontMemResourceEx(uint64_t data, uint64_t size, uint64_t, uint64_t count) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!Ready() || !g_memLoader || !(uint32_t)data) return 0;
    IDWriteFontFile* file = nullptr;
    if (FAILED(g_memLoader->CreateInMemoryFontFileReference(g_dw, P32(data), (UINT32)size, nullptr, &file))) return 0;
    BOOL ok = FALSE;
    DWRITE_FONT_FILE_TYPE ft;
    DWRITE_FONT_FACE_TYPE fft;
    UINT32 faces = 0;
    file->Analyze(&ok, &ft, &fft, &faces);
    int added = 0;
    for (UINT32 i = 0; ok && i < faces; i++) {
        IDWriteFontFace* face = nullptr;
        if (SUCCEEDED(g_dw->CreateFontFace(fft, 1, &file, i, DWRITE_FONT_SIMULATIONS_NONE, &face))) g_memFaces.push_back(face), added++;
    }
    file->Release();
    if ((uint32_t)count) *(uint32_t*)P32(count) = added;
    Log("GDI: a font from memory, %u bytes, %d face(s)", (unsigned)size, added);
    return added ? ++g_next : 0;
}
static uint64_t G_RemoveFontMemResourceEx(uint64_t) { return 1; }
// a face by family name: the console's fonts, else the game's own (from memory), else Segoe UI
static IDWriteFontFace* FindFace(const std::string& name, int weight, bool italic) {
    IDWriteFontCollection* sys = nullptr;
    g_dw->GetSystemFontCollection(&sys);
    IDWriteFontFace* out = nullptr;
    auto tryName = [&](const wchar_t* n) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (!sys || FAILED(sys->FindFamilyName(n, &idx, &exists)) || !exists) return false;
        IDWriteFontFamily* fam = nullptr;
        IDWriteFont* f = nullptr;
        if (SUCCEEDED(sys->GetFontFamily(idx, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STRETCH_NORMAL,
                                                italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &f)))
            f->CreateFontFace(&out);
        if (f) f->Release();
        if (fam) fam->Release();
        return out != nullptr;
    };
    std::wstring w(name.begin(), name.end());
    if (!w.empty() && tryName(w.c_str())) {
    } else if (!g_memFaces.empty()) {
        out = g_memFaces.back();
        out->AddRef();
    } else {
        tryName(L"Segoe UI") || tryName(L"Arial");
    }
    if (sys) sys->Release();
    return out;
}
static uint64_t MakeFont(int32_t height, int weight, bool italic, const char* face) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!Ready()) return 0;
    Font f;
    f.weight = weight ? weight : 400;
    f.face = FindFace(face ? face : "", f.weight, italic);
    if (!f.face) return 0;
    f.face->GetMetrics(&f.m);
    float cell = (float)(f.m.ascent + f.m.descent);
    if (height < 0) f.em = (float)-height;
    else if (height > 0) f.em = height * f.m.designUnitsPerEm / cell;
    else f.em = 16;
    uint32_t id = ++g_next;
    g_fonts[id] = f;
    static int told = 0;
    if (told++ < 10) Log("GDI: font \"%s\" height %d weight %d -> %.1f px", face ? face : "", height, f.weight, f.em);
    return id;
}
static uint64_t G_CreateFontA(uint64_t h, uint64_t, uint64_t, uint64_t, uint64_t weight, uint64_t italic, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t face) {
    return MakeFont((int32_t)h, (int)weight, (uint32_t)italic != 0, (uint32_t)face ? (const char*)P32(face) : "");
}
static uint64_t G_CreateFontIndirectA(uint64_t lf) {   // LOGFONTA: height +0, weight +16, italic +20, face +28
    if (!(uint32_t)lf) return 0;
    const uint8_t* p = P32(lf);
    return MakeFont(*(const int32_t*)p, *(const int32_t*)(p + 16), p[20] != 0, (const char*)(p + 28));
}
static uint64_t G_CreateCompatibleDC(uint64_t) {
    std::lock_guard<std::mutex> l(g_lock);
    return ++g_next;
}
static uint64_t G_DeleteDC(uint64_t dc) {
    std::lock_guard<std::mutex> l(g_lock);
    g_dcFont.erase((uint32_t)dc);
    return 1;
}
static uint64_t G_SelectObject(uint64_t dc, uint64_t obj) {
    std::lock_guard<std::mutex> l(g_lock);
    uint32_t was = g_dcFont[(uint32_t)dc];
    if (g_fonts.count((uint32_t)obj)) g_dcFont[(uint32_t)dc] = (uint32_t)obj;
    return was ? was : 0x7A0000FF;
}
static uint64_t G_DeleteObject(uint64_t obj) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_fonts.find((uint32_t)obj);
    if (it != g_fonts.end()) {
        if (it->second.face) it->second.face->Release();
        g_fonts.erase(it);
    }
    return 1;
}
static uint64_t G_GetStockObject(uint64_t i) { return 0x7A000000 + (uint32_t)i; }
static uint64_t G_SetMapMode(uint64_t, uint64_t) { return 1; }
static Font* DcFont(uint64_t dc) {
    auto it = g_dcFont.find((uint32_t)dc);
    if (it == g_dcFont.end()) return nullptr;
    auto f = g_fonts.find(it->second);
    return f == g_fonts.end() ? nullptr : &f->second;
}
static UINT16 GlyphOf(Font* f, uint32_t ch, bool isIndex) {
    if (isIndex) return (UINT16)ch;
    wchar_t w = 0;
    char a[2] = {(char)ch, 0};
    if (ch < 0x80) w = (wchar_t)ch;
    else MultiByteToWideChar(CP_ACP, 0, a, 1, &w, 1);
    UINT32 cp = w;
    UINT16 g = 0;
    f->face->GetGlyphIndices(&cp, 1, &g);
    return g;
}
static float Scale(Font* f) { return f->em / f->m.designUnitsPerEm; }
static uint64_t G_GetTextMetricsA(uint64_t dc, uint64_t out) {
    std::lock_guard<std::mutex> l(g_lock);
    Font* f = DcFont(dc);
    if (!f || !(uint32_t)out) return 0;
    float s = Scale(f);
    int32_t* t = (int32_t*)P32(out);
    memset(t, 0, 56);
    int asc = (int)ceilf(f->m.ascent * s), desc = (int)ceilf(f->m.descent * s);
    t[0] = asc + desc;                          // tmHeight
    t[1] = asc;                                 // tmAscent
    t[2] = desc;                                // tmDescent
    t[3] = (int)(asc + desc - f->em + 0.5f);    // tmInternalLeading
    t[5] = (int)(f->em * 0.5f + 0.5f);          // tmAveCharWidth
    t[6] = (int)(f->em + 0.5f);                 // tmMaxCharWidth
    t[7] = f->weight;                           // tmWeight
    t[9] = t[10] = 96;                          // tmDigitizedAspectX/Y
    uint8_t* b = (uint8_t*)(t + 11);
    b[0] = 0x20, b[1] = 0xFF, b[2] = 0x1F, b[3] = 0x20;   // first, last, default, break
    b[7] = 0x06;                                // TMPF_VECTOR | TMPF_TRUETYPE
    return 1;
}
static uint64_t G_GetCharABCWidthsA(uint64_t dc, uint64_t first, uint64_t last, uint64_t out) {
    std::lock_guard<std::mutex> l(g_lock);
    Font* f = DcFont(dc);
    if (!f || !(uint32_t)out || (uint32_t)last < (uint32_t)first) return 0;
    float s = Scale(f);
    int32_t* abc = (int32_t*)P32(out);
    for (uint32_t c = (uint32_t)first; c <= (uint32_t)last; c++, abc += 3) {
        UINT16 g = GlyphOf(f, c, false);
        DWRITE_GLYPH_METRICS gm = {};
        f->face->GetDesignGlyphMetrics(&g, 1, &gm, FALSE);
        int a = (int)lroundf(gm.leftSideBearing * s), cc = (int)lroundf(gm.rightSideBearing * s);
        int adv = (int)lroundf(gm.advanceWidth * s);
        abc[0] = a, abc[1] = adv - a - cc > 0 ? adv - a - cc : 0, abc[2] = cc;
    }
    return 1;
}
static uint64_t G_GetTextExtentPoint32A(uint64_t dc, uint64_t str, uint64_t n, uint64_t out) {
    std::lock_guard<std::mutex> l(g_lock);
    Font* f = DcFont(dc);
    if (!f || !(uint32_t)out) return 0;
    float s = Scale(f), w = 0;
    const char* p = (const char*)P32(str);
    for (uint32_t i = 0; p && i < (uint32_t)n; i++) {
        UINT16 g = GlyphOf(f, (uint8_t)p[i], false);
        DWRITE_GLYPH_METRICS gm = {};
        f->face->GetDesignGlyphMetrics(&g, 1, &gm, FALSE);
        w += gm.advanceWidth * s;
    }
    int32_t* sz = (int32_t*)P32(out);
    sz[0] = (int32_t)lroundf(w);
    sz[1] = (int32_t)ceilf((f->m.ascent + f->m.descent) * s);
    return 1;
}
// GetGlyphOutlineA: GGO_METRICS 0, GGO_BITMAP 1, GGO_GRAY2/4/8 4/5/6; GGO_GLYPH_INDEX 0x80. GLYPHMETRICS: black box
// x, y, origin x, origin y (from the baseline up), cell x, cell y
static uint64_t G_GetGlyphOutlineA(uint64_t dc, uint64_t ch, uint64_t fmtIn, uint64_t gmP, uint64_t cb, uint64_t buf, uint64_t) {
    std::lock_guard<std::mutex> l(g_lock);
    Font* f = DcFont(dc);
    if (!f) return GDI_ERROR;
    uint32_t fmt = (uint32_t)fmtIn & 0x0F;
    UINT16 g = GlyphOf(f, (uint32_t)ch, ((uint32_t)fmtIn & 0x80) != 0);
    DWRITE_GLYPH_METRICS dm = {};
    f->face->GetDesignGlyphMetrics(&g, 1, &dm, FALSE);
    float adv = dm.advanceWidth * Scale(f);
    // the glyph's coverage (ClearType 3x1, averaged: smooth grey)
    FLOAT advance = 0;
    DWRITE_GLYPH_OFFSET off = {};
    DWRITE_GLYPH_RUN run = {f->face, f->em, 1, &g, &advance, &off, FALSE, 0};
    IDWriteGlyphRunAnalysis* an = nullptr;
    RECT r = {};
    std::vector<BYTE> tex;
    if (SUCCEEDED(g_dw->CreateGlyphRunAnalysis(&run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL, DWRITE_MEASURING_MODE_NATURAL,
                                               0, 0, &an))) {
        an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &r);
        if (r.right > r.left && r.bottom > r.top) {
            tex.resize((size_t)(r.right - r.left) * (r.bottom - r.top) * 3);
            an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &r, tex.data(), (UINT32)tex.size());
        }
        an->Release();
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    bool empty = w <= 0 || h <= 0;
    if ((uint32_t)gmP) {
        int32_t* gm = (int32_t*)P32(gmP);
        gm[0] = empty ? 1 : w;                 // gmBlackBoxX
        gm[1] = empty ? 1 : h;                 // gmBlackBoxY
        gm[2] = empty ? 0 : r.left;            // gmptGlyphOrigin.x
        gm[3] = empty ? 0 : -r.top;            // gmptGlyphOrigin.y
        ((int16_t*)(gm + 4))[0] = (int16_t)lroundf(adv);   // gmCellIncX
        ((int16_t*)(gm + 4))[1] = 0;                      // gmCellIncY
    }
    static int told = 0;
    if (told++ < 6) Log("GDI: glyph %u (format %u): %dx%d", (unsigned)(uint32_t)ch, fmt, w, h);
    if (fmt == 0 || fmt == 2 || fmt == 3) return fmt == 0 ? 1 : 0;   // metrics only / outlines: none
    if (empty) {   // (a space: a blank 1x1 bitmap, which games read whatever the size says)
        if ((uint32_t)cb >= 4 && (uint32_t)buf) memset(P32(buf), 0, 4);
        return 4;
    }
    uint32_t pitch = fmt == 1 ? ((w + 31) / 32) * 4 : (w + 3) & ~3u;
    uint32_t need = pitch * h;
    if (!(uint32_t)cb || !(uint32_t)buf) return need;
    if ((uint32_t)cb < need) return GDI_ERROR;
    uint8_t* out = P32(buf);
    memset(out, 0, need);
    int levels = fmt == 4 ? 4 : fmt == 5 ? 16 : 64;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const BYTE* t = &tex[((size_t)y * w + x) * 3];
            int a = (t[0] + t[1] + t[2]) / 3;
            if (fmt == 1) {
                if (a >= 128) out[y * pitch + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            } else {
                out[y * pitch + x] = (uint8_t)((a * levels + 127) / 255);
            }
        }
    return need;
}

struct GdiFn {
    const char* name;
    int args;
    void* fn;
};
static const GdiFn kGdi[] = {
    {"AddFontMemResourceEx", 4, (void*)G_AddFontMemResourceEx}, {"RemoveFontMemResourceEx", 1, (void*)G_RemoveFontMemResourceEx},
    {"CreateFontA", 14, (void*)G_CreateFontA}, {"CreateFontIndirectA", 1, (void*)G_CreateFontIndirectA},
    {"CreateCompatibleDC", 1, (void*)G_CreateCompatibleDC}, {"DeleteDC", 1, (void*)G_DeleteDC},
    {"SelectObject", 2, (void*)G_SelectObject}, {"DeleteObject", 1, (void*)G_DeleteObject},
    {"GetStockObject", 1, (void*)G_GetStockObject}, {"SetMapMode", 2, (void*)G_SetMapMode},
    {"GetTextMetricsA", 2, (void*)G_GetTextMetricsA}, {"GetCharABCWidthsA", 4, (void*)G_GetCharABCWidthsA},
    {"GetTextExtentPoint32A", 4, (void*)G_GetTextExtentPoint32A}, {"GetGlyphOutlineA", 7, (void*)G_GetGlyphOutlineA},
};
void* FindGdi32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "gdi32.dll")) return nullptr;
    for (const GdiFn& g : kGdi)
        if (!strcmp(g.name, func)) {
            args = g.args;
            return g.fn;
        }
    return nullptr;
}
}  // namespace arcade
