#include "typography.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace Grad {
const Gradient Fire = {{255, 232, 80, 255}, {255, 140, 30, 255}, {232, 66, 79, 255}, {110, 18, 30, 255}};
const Gradient White = {{255, 255, 255, 255}, {242, 242, 242, 255}, {170, 178, 215, 255}, {14, 10, 22, 255}};
const Gradient Gold = {{255, 244, 160, 255}, {245, 184, 65, 255}, {205, 110, 30, 255}, {70, 30, 10, 255}};
const Gradient P1 = {{255, 160, 160, 255}, {240, 60, 70, 255}, {150, 20, 40, 255}, {50, 5, 15, 255}};
const Gradient P2 = {{180, 225, 255, 255}, {70, 150, 255, 255}, {30, 60, 170, 255}, {8, 15, 60, 255}};
}  // namespace Grad

static const Color OUTLINE = {14, 10, 22, 255};
static Font uiFont{}, displayFont{};
static Image displayAtlas{};  // atlas da fonte Display na CPU (para montar os textos)
struct DisplayTex { Texture2D fill, shadow; int pad, top; };  // top = espaço extra em cima (acentos), em px
const int ACCENT = 3;  // px da fonte acima da linha das maiúsculas reservados para acento (É, Ã, Ç...)
static std::map<std::string, DisplayTex> cache;

// Fonte pixel no tamanho nativo (8 px = 1 em), sem o meio-tom do antialias. O raylib escala pela altura
// ascendente+descendente da tabela hhea, não pelo "em" (Silkscreen: 1,28 em, então 8 px saía fora da grade e as
// letras deformavam). Na cópia em memória, a ascendente vira em + descendente: altura = 1 em, linha de base inteira.
// O atlas é montado na CPU (sem ler a textura de volta da placa: no WebGL isso não funciona com fonte); `atlas`, se
// pedido, fica com a cópia RGBA.
static Font LoadPixelFont(const char* file, Image* atlas = nullptr) {
    int size = 0;
    unsigned char* d = FileExists(file) ? LoadFileData(file, &size) : nullptr;
    if (!d) return {};
    auto u16 = [&](int o) { return o >= 0 && o + 2 <= size ? (d[o] << 8) | d[o + 1] : 0; };
    auto u32 = [&](int o) { return (u16(o) << 16) | u16(o + 2); };
    int upem = 0, hhea = -1;
    for (int i = 0, n = u16(4); i < n && 12 + 16 * i + 16 <= size; i++) {
        int rec = 12 + 16 * i, off = u32(rec + 8);
        if (!memcmp(d + rec, "head", 4) && off + 20 <= size) upem = u16(off + 18);
        if (!memcmp(d + rec, "hhea", 4) && off + 8 <= size) hhea = off;
    }
    if (upem > 0 && hhea >= 0) {
        int asc = upem + (short)u16(hhea + 6);
        d[hhea + 4] = (unsigned char)(asc >> 8);
        d[hhea + 5] = (unsigned char)(asc & 255);
    }
    int cps[224];  // ASCII + Latin-1
    for (int k = 0; k < 224; k++) cps[k] = 32 + k;
    Font f{};
    f.baseSize = 8;
    f.glyphCount = 224;
    f.glyphPadding = 4;  // como o LoadFontFromMemory do raylib
    f.glyphs = LoadFontData(d, size, 8, cps, 224, FONT_DEFAULT);
    UnloadFileData(d);
    if (!f.glyphs) return {};
    Image a = GenImageFontAtlas(f.glyphs, &f.recs, f.glyphCount, f.baseSize, f.glyphPadding, 0);
    ImageFormat(&a, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    Color* px = (Color*)a.data;
    for (int k = 0; k < a.width * a.height; k++) px[k] = {255, 255, 255, (unsigned char)(px[k].a >= 128 ? 255 : 0)};
    f.texture = LoadTextureFromImage(a);
    if (atlas) *atlas = a;
    else UnloadImage(a);
    return f;
}

static Font Ui() { return uiFont.texture.id ? uiFont : GetFontDefault(); }
static Font Display() { return displayFont.texture.id ? displayFont : GetFontDefault(); }
static float Spacing(const Font& f) { return f.texture.id == GetFontDefault().texture.id ? 1.0f : 0.0f; }  // por px da fonte

void LoadTypography() {
    uiFont = LoadPixelFont("assets/fonts/Silkscreen-Regular.ttf");
    displayFont = LoadPixelFont("assets/fonts/Silkscreen-Bold.ttf", &displayAtlas);
    if (!displayAtlas.data) {  // sem o arquivo: fonte padrão do raylib
        displayAtlas = LoadImageFromTexture(Display().texture);
        ImageFormat(&displayAtlas, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    }
}

void UnloadTypography() {
    for (auto& [key, d] : cache) UnloadTexture(d.fill), UnloadTexture(d.shadow);
    cache.clear();
    for (Font* f : {&uiFont, &displayFont})
        if (f->texture.id) UnloadFont(*f), *f = {};
    UnloadImage(displayAtlas);
    displayAtlas = {};
}

int UITextHeight(int scale) { return Ui().baseSize * scale; }

int MeasureUIText(const char* text, int scale) {
    Font f = Ui();
    return (int)MeasureTextEx(f, text, (float)(f.baseSize * scale), Spacing(f) * scale).x;
}

void DrawUIText(const char* text, Vector2 pos, int scale, Color color, bool outline) {
    Font f = Ui();
    float size = (float)(f.baseSize * scale), sp = Spacing(f) * scale;
    Vector2 p = {floorf(pos.x), floorf(pos.y)};
    if (outline)
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy) DrawTextEx(f, text, {p.x + dx, p.y + dy}, size, sp, Fade(OUTLINE, color.a / 255.0f));
    DrawTextEx(f, text, p, size, sp, color);
}

Vector2 MeasureDisplayText(const char* text, int scale) {
    Font f = Display();
    Vector2 m = MeasureTextEx(f, text, (float)f.baseSize, Spacing(f));
    return {ceilf(m.x) * scale, (float)f.baseSize * scale};
}

// Texto Display pronto: letras ampliadas `scale`x (vizinho mais próximo), degradê de 3 cores pela altura das letras,
// brilho branco de 1 px no topo de cada traço, contorno escuro; `shadow` = a silhueta inteira na cor da sombra.
static const DisplayTex& Build(const char* text, int scale, const Gradient& g) {
    std::string key = std::string(text) + '\x1f' + std::to_string(scale) + '\x1f' + std::to_string((uintptr_t)&g);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Font f = Display();
    float sp = Spacing(f);
    Vector2 m = MeasureDisplayText(text, 1);
    Image L = GenImageColor(std::max(1, (int)m.x), f.baseSize + ACCENT, BLANK);
    float x = 0;
    for (const char* s = text; *s;) {
        int n = 0, cp = GetCodepointNext(s, &n);
        s += n;
        int gi = GetGlyphIndex(f, cp);
        Rectangle r = f.recs[gi];
        GlyphInfo gl = f.glyphs[gi];
        ImageDraw(&L, displayAtlas, r, {x + gl.offsetX, (float)(gl.offsetY + ACCENT), r.width, r.height}, WHITE);
        x += (gl.advanceX ? gl.advanceX : r.width) + sp;
    }
    int o = scale >= 2 ? 2 : 1;
    ImageResizeNN(&L, L.width * scale, L.height * scale);
    ImageResizeCanvas(&L, L.width + 2 * o, L.height + 2 * o, o, o, BLANK);
    int w = L.width, h = L.height, y0 = h, y1 = 0;
    Color* lp = (Color*)L.data;
    auto on = [&](int px, int py) { return px >= 0 && py >= 0 && px < w && py < h && lp[py * w + px].a >= 128; };
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++)
            if (on(px, py)) y0 = std::min(y0, py), y1 = std::max(y1, py);
    Image F = GenImageColor(w, h, BLANK), S = GenImageColor(w, h, BLANK);
    Color* fp = (Color*)F.data;
    Color* sp2 = (Color*)S.data;
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            Color c = BLANK;
            if (on(px, py)) {
                float t = (py - y0) / (float)std::max(1, y1 - y0);
                c = t < 0.5f ? ColorLerp(g.top, g.mid, t * 2) : ColorLerp(g.mid, g.bottom, (t - 0.5f) * 2);
                if (!on(px, py - 1)) c = ColorLerp(c, WHITE, 0.75f);  // brilho no topo do traço
            } else {
                for (int dy = -o; dy <= o && !c.a; dy++)
                    for (int dx = -o; dx <= o; dx++)
                        if (on(px + dx, py + dy)) { c = OUTLINE; break; }
            }
            fp[py * w + px] = c;
            sp2[py * w + px] = c.a ? g.shadow : BLANK;
        }
    DisplayTex d = {LoadTextureFromImage(F), LoadTextureFromImage(S), o, ACCENT * scale};
    UnloadImage(L);
    UnloadImage(F);
    UnloadImage(S);
    return cache[key] = d;
}

void DrawDisplayText(const char* text, Vector2 pos, int scale, const Gradient& g, bool shadow, float zoom, float alpha) {
    if (!*text) return;
    const DisplayTex& d = Build(text, scale, g);
    float w = (float)d.fill.width, h = (float)d.fill.height;
    float cx = floorf(pos.x) - d.pad + w / 2, cy = floorf(pos.y) - d.pad - d.top + h / 2;
    Rectangle src = {0, 0, w, h};
    Rectangle dst = {roundf(cx - w * zoom / 2), roundf(cy - h * zoom / 2), roundf(w * zoom), roundf(h * zoom)};
    Color tint = Fade(WHITE, alpha);
    if (shadow) {
        float so = roundf((scale == 1 ? 2 : scale == 2 ? 3 : 4) * zoom);  // sombra sólida de 3-4 px (2 na escala 1)
        DrawTexturePro(d.shadow, src, {dst.x + so, dst.y + so, dst.width, dst.height}, {0, 0}, 0, tint);
    }
    DrawTexturePro(d.fill, src, dst, {0, 0}, 0, tint);
}
