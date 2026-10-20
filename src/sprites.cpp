#include "sprites.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

static int Magenta(Color c) { return std::min(c.r, c.b) - c.g; }  // 255 no magenta puro, <= 0 em pele, roupa, cinza e preto

void RemoveBackground(Image& img, int tolerance) {
    int w = img.width, h = img.height, n = w * h;
    Color* px = (Color*)img.data;

    // Cor do fundo medida na borda da imagem (mediana), para aceitar magentas um pouco mais escuros ou claros.
    std::vector<int> border;
    for (int x = 0; x < w; x++) { border.push_back(Magenta(px[x])); border.push_back(Magenta(px[(h - 1) * w + x])); }
    for (int y = 0; y < h; y++) { border.push_back(Magenta(px[y * w])); border.push_back(Magenta(px[y * w + w - 1])); }
    std::nth_element(border.begin(), border.begin() + border.size() / 2, border.end());
    if (border[border.size() / 2] < 60) {
        // Fundo preto (efeitos de luz: projétil, impacto): o brilho vira opacidade ("unscreen"). O halo escuro
        // some suave sobre o cenário em vez de virar um contorno marrom; o núcleo branco fica opaco.
        for (int i = 0; i < n; i++) {
            int m = std::max({px[i].r, px[i].g, px[i].b});
            if (m < 16) { px[i] = BLANK; continue; }
            px[i] = {(unsigned char)(px[i].r * 255 / m), (unsigned char)(px[i].g * 255 / m), (unsigned char)(px[i].b * 255 / m),
                     (unsigned char)m};
        }
        return;
    }
    int strongMin = std::max(120, border[border.size() / 2] - 50);

    // Fundo = magenta forte em qualquer lugar (inclui buracos fechados, tipo entre braço e corpo).
    // Franja = tons rosados a até 2 px do fundo (antialiasing). Rosa no meio do personagem (lente, roupa) fica.
    std::vector<unsigned char> strong(n), nearBg(n);
    for (int i = 0; i < n; i++) strong[i] = nearBg[i] = Magenta(px[i]) >= strongMin;
    for (int pass = 0; pass < 2; pass++) {
        std::vector<unsigned char> grown = nearBg;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (nearBg[y * w + x])
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < w && ny < h) grown[ny * w + nx] = 1;
                        }
        nearBg.swap(grown);
    }
    for (int i = 0; i < n; i++)
        if (strong[i] || (nearBg[i] && 255 - Magenta(px[i]) <= tolerance)) px[i] = BLANK;
    // Borda que sobrou com tom do fundo (cabelo escuro arroxeado): tira o magenta, fica neutra. Só a 2 px do fundo.
    for (int i = 0; i < n; i++)
        if (nearBg[i] && px[i].a && Magenta(px[i]) > 0) {
            int m = Magenta(px[i]);
            px[i].r = (unsigned char)(px[i].r - m);
            px[i].b = (unsigned char)(px[i].b - m);
        }
}

struct Blob { int size = 0, minX = 0, maxX = 0, minY = 0, maxY = 0, cell = 0; long long sumX = 0, sumY = 0; bool keep = false; };

// Recorta os quadros de uma sheet (ordem de leitura), já sem fundo, e acrescenta em `cells`/`seeds`/`bodyH`/`bodyW`
// (seed = centro de massa do quadro, bodyH/bodyW = altura/largura da maior mancha = corpo). false = arquivo não abriu.
static bool ExtractCells(const std::string& file, std::vector<int> layout, int frames, int chromaTolerance,
                         std::vector<Image>& cells, std::vector<float>& seeds, std::vector<int>& bodyH,
                         std::vector<int>& bodyW, std::vector<int>& sinks) {
    // grade da sheet: quadros por linha (padrão = uma linha só com todos)
    layout.erase(std::remove_if(layout.begin(), layout.end(), [](int c) { return c < 1; }), layout.end());
    if (layout.empty()) layout = {frames};
    frames = 0;
    for (int c : layout) frames += c;
    if (frames < 1 || !FileExists(file.c_str())) return false;
    Image sheet = LoadImage(file.c_str());
    if (!sheet.data) return false;
    ImageFormat(&sheet, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int w = sheet.width, h = sheet.height, n = w * h;
    Color* px = (Color*)sheet.data;
    RemoveBackground(sheet, chromaTolerance);

    // Manchas conexas (8 vizinhos) na sheet inteira. Cada mancha vai para a célula onde está o seu centro:
    // a IA não centraliza o personagem, e um punho ou pé que passa da divisa continua no quadro certo.
    std::vector<int> label(n, -1), stack;
    std::vector<Blob> blobs;
    for (int s = 0; s < n; s++) {
        if (!px[s].a || label[s] >= 0) continue;
        int id = (int)blobs.size();
        Blob b;
        b.minX = w;
        b.minY = h;
        label[s] = id;
        stack.push_back(s);
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back();
            int x = p % w, y = p / w;
            b.size++;
            b.sumX += x;
            b.sumY += y;
            b.minX = std::min(b.minX, x);
            b.maxX = std::max(b.maxX, x);
            b.minY = std::min(b.minY, y);
            b.maxY = std::max(b.maxY, y);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int q = ny * w + nx;
                    if (px[q].a && label[q] < 0) { label[q] = id; stack.push_back(q); }
                }
        }
        // linha pelo centro vertical, coluna pelo centro horizontal dentro dessa linha
        int rows = (int)layout.size();
        int row = std::clamp((int)(b.sumY / b.size * rows / h), 0, rows - 1);
        int col = std::clamp((int)(b.sumX / b.size * layout[row] / w), 0, layout[row] - 1);
        b.cell = col;
        for (int r = 0; r < row; r++) b.cell += layout[r];
        if ((b.maxX - b.minX) * layout[row] > w * 3 / 2)
            TraceLog(LOG_WARNING, "SPRITE: %s tem personagens encostados (mancha de %d px de largura)", file.c_str(), b.maxX - b.minX + 1);
        blobs.push_back(b);
    }
    // Maior mancha de cada quadro = corpo. A altura dele define a escala, então pedaços soltos
    // (óculos voando, por exemplo) não encolhem o personagem.
    std::vector<int> largest(frames, 0), sheetBodyH(frames, 1), sheetBodyW(frames, 1), bodyBottom(frames, 0);
    for (const Blob& b : blobs)
        if (b.size > largest[b.cell]) {
            largest[b.cell] = b.size;
            sheetBodyH[b.cell] = b.maxY - b.minY + 1;
            sheetBodyW[b.cell] = b.maxX - b.minX + 1;
            bodyBottom[b.cell] = b.maxY;
        }
    bodyH.insert(bodyH.end(), sheetBodyH.begin(), sheetBodyH.end());
    bodyW.insert(bodyW.end(), sheetBodyW.begin(), sheetBodyW.end());
    for (Blob& b : blobs) b.keep = b.size * 1000 >= largest[b.cell];  // descarta ciscos de ruído
    for (const Blob& b : blobs)
        if (b.keep && (b.minX == 0 || b.maxX == w - 1 || b.minY == 0 || b.maxY == h - 1))
            TraceLog(LOG_WARNING, "SPRITE: %s quadro %d encosta na borda da imagem: a arte pode vir cortada", file.c_str(), b.cell + 1);
    // Caixa de cada quadro = união das manchas dele; o recorte apaga o que for de outro quadro.
    std::vector<int> minX(frames, w), minY(frames, h), maxX(frames, -1), maxY(frames, -1);
    for (int p = 0; p < n; p++) {
        if (label[p] < 0 || !blobs[label[p]].keep) continue;
        int c = blobs[label[p]].cell, x = p % w, y = p / w;
        minX[c] = std::min(minX[c], x); maxX[c] = std::max(maxX[c], x);
        minY[c] = std::min(minY[c], y); maxY[c] = std::max(maxY[c], y);
    }
    for (int i = 0; i < frames; i++) {
        int x0 = maxX[i] < 0 ? 0 : minX[i], y0 = maxX[i] < 0 ? 0 : minY[i];
        int bw = maxX[i] < 0 ? 1 : maxX[i] - x0 + 1, bh = maxX[i] < 0 ? 1 : maxY[i] - y0 + 1;  // vazio: 1 px transparente
        Image cell = ImageFromImage(sheet, {(float)x0, (float)y0, (float)bw, (float)bh});
        Color* cp = (Color*)cell.data;
        double sumX = 0;
        int count = 0;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int l = label[(y0 + y) * w + x0 + x];
                if (l < 0 || !blobs[l].keep || blobs[l].cell != i) cp[y * bw + x] = BLANK;
                else { sumX += x + 0.5; count++; }
            }
        seeds.push_back(count ? (float)(sumX / count) : bw / 2.0f);
        // pedaço solto abaixo do corpo (óculos caindo): quanto o recorte desce além do corpo
        sinks.push_back(maxX[i] < 0 ? 0 : std::max(0, maxY[i] - bodyBottom[i]));
        TraceLog(LOG_INFO, "SPRITE: %s frame %d -> caixa %dx%d em (%d,%d)", file.c_str(), i, bw, bh, x0, y0);
        cells.push_back(cell);
    }
    UnloadImage(sheet);
    return true;
}

Anim LoadAnim(const std::vector<std::string>& files, int frames, int height, int chromaTolerance, const AnimOptions& opt) {
    Anim a;
    SetAnimFps(a, (float)std::max(1, opt.fps));
    std::vector<Image> cells;
    std::vector<float> seeds;
    std::vector<int> bodyH, bodyW, sinks;
    for (const std::string& file : files)  // várias sheets em sequência = uma animação só
        if (!ExtractCells(file, opt.layout, frames, chromaTolerance, cells, seeds, bodyH, bodyW, sinks)) {
            for (Image& c : cells) UnloadImage(c);
            return a;  // falta um arquivo: animação inteira cai no fallback
        }
    if (cells.empty()) return a;

    // Uma escala só para todos os quadros (de todos os arquivos), assim diferenças de tamanho entre quadros
    // continuam visíveis. Padrão: o corpo de referência (`refFrame`, senão o mais alto) vira `height`
    // (braço esticado para cima no pulo não encolhe o personagem: o JSON aponta o quadro "em pé").
    // Com `bodyTarget`: a MEDIANA das alturas de corpo vira esse valor (mesma escala de outra animação;
    // a mediana ignora um quadro fora do padrão).
    std::vector<int> sorted = bodyH;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    float median = (float)sorted[sorted.size() / 2];
    // `refByWidth` (personagem deitado, K.O.): o comprimento do corpo no quadro de referência vira `height`
    const std::vector<int>& refDim = opt.refByWidth ? bodyW : bodyH;
    int ref = opt.refFrame >= 0 && opt.refFrame < (int)refDim.size() ? refDim[opt.refFrame]
                                                                      : *std::max_element(refDim.begin(), refDim.end());
    float scale = opt.pixelScale > 0 ? opt.pixelScale : opt.bodyTarget > 0 ? opt.bodyTarget / median : (float)height / ref;
    TraceLog(LOG_INFO, "SPRITE: %s escala %.4f px do jogo por px da imagem", files[0].c_str(), scale);
    a.bodyMedian = median * scale;
    float minSpan = 1e9f, maxSpan = 0;
    for (size_t k = 0; k < cells.size(); k++) {
        Image& c = cells[k];
        int oldW = c.width;
        float s = scale * (k < opt.frameScale.size() ? opt.frameScale[k] : 1);
        ImageResizeNN(&c, std::max(1, (int)lroundf(c.width * s)), std::max(1, (int)lroundf(c.height * s)));
        a.bodyHeights.push_back(bodyH[k] * s);
        a.boxes.push_back(MeasureFrame(c, seeds[k] * c.width / oldW, opt.feetAnchor));
        a.boxes.back().sink = sinks[k] * s;
        a.anchors.push_back(a.boxes.back().anchor);
        a.frames.push_back(LoadTextureFromImage(c));
        Color* px = (Color*)c.data;
        // largura ocupada pelos pés (faixa de baixo, 8% da altura): juntos = 1 sapato, afastados = passo + 1 sapato
        int band = std::max(2, c.height * 8 / 100), lo = c.width, hi = -1;
        for (int y = c.height - band; y < c.height; y++)
            for (int x = 0; x < c.width; x++)
                if (px[y * c.width + x].a) { lo = std::min(lo, x); hi = std::max(hi, x); }
        if (hi >= lo) { minSpan = std::min(minSpan, (float)(hi - lo + 1)); maxSpan = std::max(maxSpan, (float)(hi - lo + 1)); }
        UnloadImage(c);
    }
    a.stepLen = maxSpan > minSpan ? maxSpan - minSpan : 0;
    return a;
}

Texture2D LoadPicture(const std::string& file, int chromaTolerance, int w, int h, bool circle, float scale, float focusX,
                      float square, bool roundMask) {
    if (file.empty() || !FileExists(file.c_str())) return {};
    Image img = LoadImage(file.c_str());
    if (!img.data) return {};
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    RemoveBackground(img, chromaTolerance);
    Rectangle r = GetImageAlphaBorder(img, 0);
    if (circle) {  // cabeça: quadrado no topo do desenho
        float side = std::min(r.width, r.height) * std::clamp(square, 0.1f, 1.0f);
        r = {r.x + (r.width - side) * std::clamp(focusX, 0.0f, 1.0f), r.y, side, side};
    }
    if (r.width < 1 || r.height < 1) { UnloadImage(img); return {}; }
    ImageCrop(&img, r);
    float s = scale > 0 ? scale : std::min(w / r.width, h / r.height);
    ImageResizeNN(&img, std::max(1, (int)lroundf(r.width * s)), std::max(1, (int)lroundf(r.height * s)));
    if (circle && roundMask) {
        Color* px = (Color*)img.data;
        float c = img.width / 2.0f, rad = std::min(img.width, img.height) / 2.0f;
        for (int y = 0; y < img.height; y++)
            for (int x = 0; x < img.width; x++)
                if ((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c) > rad * rad) px[y * img.width + x] = BLANK;
    }
    // 1 px transparente em volta: cortada em forma inclinada além da imagem, a borda esticada pelo CLAMP fica invisível
    ImageResizeCanvas(&img, img.width + 2, img.height + 2, 1, 1, BLANK);
    Texture2D t = LoadTextureFromImage(img);
    SetTextureWrap(t, TEXTURE_WRAP_CLAMP);  // recortada em forma inclinada (menu): fora da imagem = borda, não repete
    UnloadImage(img);
    return t;
}

void UnloadAnim(Anim& a) {
    for (Texture2D& t : a.frames) UnloadTexture(t);
    a = Anim{};
}

FrameBoxes MeasureFrame(const Image& img, float seedX, bool feetAnchor) {
    int w = img.width, h = img.height;
    const Color* px = (const Color*)img.data;
    FrameBoxes fb{};
    fb.anchor = w / 2.0f;
    fb.left = -fb.anchor;
    fb.right = w - fb.anchor;
    std::vector<int> count(w, 0);  // pixels visíveis por coluna
    int total = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px[y * w + x].a) { count[x]++; total++; }
    if (!total) return fb;

    // Tronco = faixa contínua de colunas "cheias" (>= 40% da coluna mais cheia) perto de seedX.
    // Colunas só de braço, punho, bolsa ou pé de apoio são baixas e ficam de fora.
    int maxC = *std::max_element(count.begin(), count.end());
    auto isBody = [&](int x) { return count[x] * 10 >= maxC * 4; };
    int ac = std::clamp((int)seedX, 0, w - 1), seed = ac;
    for (int d = 0; d < w; d++) {
        if (ac - d >= 0 && isBody(ac - d)) { seed = ac - d; break; }
        if (ac + d < w && isBody(ac + d)) { seed = ac + d; break; }
    }
    int bl = seed, br = seed;
    while (bl > 0 && isBody(bl - 1)) bl--;
    while (br < w - 1 && isBody(br + 1)) br++;
    // centro dos pés: média dos pixels na faixa de baixo (8% da altura)
    int band = std::max(2, h * 8 / 100);
    double feetSum = 0;
    int feetN = 0;
    for (int y = h - band; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px[y * w + x].a) { feetSum += x + 0.5; feetN++; }
    float feetCenter = feetN ? (float)(feetSum / feetN) : (bl + br + 1) / 2.0f;
    // Ponto fixo do personagem: centro do tronco (braço, bolsa ou perna esticando não deslocam o corpo),
    // ou centro dos pés (`feetAnchor`: animação parada, os pés ficam plantados enquanto o tronco respira).
    float anchor = fb.anchor = feetAnchor ? feetCenter : (bl + br + 1) / 2.0f;
    fb.feetX = feetCenter - anchor;
    fb.left = -anchor;
    fb.right = w - anchor;
    // topo do corpo = 1ª linha com um trecho CONTÍNUO de >= 35% da largura do tronco (a cabeça); arma fina
    // atravessando por cima (lápis em diagonal, ponta de cabelo) não conta, senão a caixa de dano sobe junto e
    // ninguém consegue pular por cima
    int top = h, need = std::max(2, (br - bl + 1) * 35 / 100);
    for (int y = 0; y < h && top == h; y++)
        for (int x = bl, run = 0; x <= br; x++)
            if ((run = px[y * w + x].a ? run + 1 : 0) >= need) { top = y; break; }
    fb.body = {bl - anchor, 0, br + 1 - anchor, (float)(h - top)};
    // frente na altura do tronco e dos braços (30%–90% da altura): guarda, braço ou perna esticados contam,
    // o pé de apoio lá embaixo não
    int frontX = br;
    for (int y = h / 10; y < h * 7 / 10; y++)
        for (int x = w - 1; x > frontX; x--)
            if (px[y * w + x].a) { frontX = x; break; }
    fb.front = frontX + 1 - anchor;

    // Acerto = o membro que vai mais longe à frente do corpo: acha o pixel mais à frente e pega só o que está
    // à frente do corpo numa faixa de ±15% da altura em volta dele (o pé de apoio lá embaixo não entra no soco).
    int farX = -1;
    for (int x = w - 1; x > br; x--)
        if (count[x]) { farX = x; break; }
    if (farX < 0) return fb;
    long long sumY = 0;
    for (int y = 0; y < h; y++)
        if (px[y * w + farX].a) sumY += y;
    int yFar = (int)(sumY / count[farX]), win = std::max(3, h * 15 / 100);
    int yLo = std::max(0, yFar - win), yHi = std::min(h - 1, yFar + win);
    // Só a parte grossa da ponta bate: volta da ponta enquanto a coluna tiver >= 40% da espessura máxima.
    // Punho+braço e pé+perna são grossos até o fim e entram inteiros; a bolsa pendurada pelas alças finas
    // entra sozinha (alça e mão não acertam ninguém).
    std::vector<int> thick(w, 0);
    for (int y = yLo; y <= yHi; y++)
        for (int x = br + 1; x <= farX; x++)
            if (px[y * w + x].a) thick[x]++;
    int maxT = *std::max_element(thick.begin() + br + 1, thick.begin() + farX + 1), tip = farX;
    auto isThick = [&](int x) { return thick[x] * 10 >= maxT * 4; };
    while (tip > br + 1 && !isThick(tip)) tip--;          // pula a borda arredondada (fina) da ponta
    while (tip - 1 > br && isThick(tip - 1)) tip--;       // e volta enquanto a parte que bate for grossa
    int x0 = w, x1 = -1, y0 = h, y1 = -1, n = 0;
    for (int y = yLo; y <= yHi; y++)
        for (int x = tip; x <= farX; x++)
            if (px[y * w + x].a) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
                n++;
            }
    if (n >= std::max(4, total / 300)) {
        fb.hasHit = true;
        fb.hit = {x0 - anchor, (float)(h - 1 - y1), x1 + 1 - anchor, (float)(h - y0)};
    }
    // O contorno escuro de 1 px da pixel art é desenho, não corpo nem bolsa: sai das caixas. Assim um golpe que
    // só "raspa" o contorno (a bolsa passando rente ao cabelo de quem está agachado) não conta.
    const float OUTLINE = 1;
    fb.body.x0 += OUTLINE; fb.body.x1 -= OUTLINE; fb.body.y1 -= OUTLINE;
    fb.front -= OUTLINE;
    if (fb.hasHit) { fb.hit.x0 += OUTLINE; fb.hit.x1 -= OUTLINE; fb.hit.y0 += OUTLINE; fb.hit.y1 -= OUTLINE; }
    fb.hasHit = fb.hasHit && fb.hit.x1 > fb.hit.x0 && fb.hit.y1 > fb.hit.y0;  // ponta de 1-2 px: só contorno, não acerta
    return fb;
}

Image EdgeMask(const Image& img, int dx) {
    Image m = GenImageColor(img.width, img.height, BLANK);
    const Color* px = (const Color*)img.data;
    Color* out = (Color*)m.data;
    auto solid = [&](int x, int y) { return x >= 0 && y >= 0 && x < img.width && y < img.height && px[y * img.width + x].a; };
    for (int y = 0; y < img.height; y++)
        for (int x = 0; x < img.width; x++)
            if (solid(x, y) && (!solid(x + dx, y) || !solid(x, y - 1))) out[y * img.width + x] = WHITE;
    return m;
}

// Quad texturizado com uma cor por vértice (p0 = canto (0,0) da textura, depois sentido anti-horário).
static void TexQuad(const Texture2D& t, bool flipU, Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3,
                    Color c0, Color c1, Color c2, Color c3) {
    float u0 = flipU ? 1.0f : 0.0f, u1 = 1 - u0;
    rlSetTexture(t.id);
    rlBegin(RL_QUADS);
    rlNormal3f(0, 0, 1);
    rlColor4ub(c0.r, c0.g, c0.b, c0.a); rlTexCoord2f(u0, 0); rlVertex2f(p0.x, p0.y);
    rlColor4ub(c1.r, c1.g, c1.b, c1.a); rlTexCoord2f(u0, 1); rlVertex2f(p1.x, p1.y);
    rlColor4ub(c2.r, c2.g, c2.b, c2.a); rlTexCoord2f(u1, 1); rlVertex2f(p2.x, p2.y);
    rlColor4ub(c3.r, c3.g, c3.b, c3.a); rlTexCoord2f(u1, 0); rlVertex2f(p3.x, p3.y);
    rlEnd();
    rlSetTexture(0);
}

// Shaders dos sprites (uma textura por quadro em vez de quatro): "silhueta" pinta os pixels visíveis na cor do
// vértice (flash branco, sombra preta, penumbra e luz quente em degradê); "borda" acende só os pixels com vizinho
// vazio do lado do sol ou em cima (fora da textura conta como vazio). Sem parâmetros por desenho (mudar parâmetro
// entre um sprite e outro fazia o driver da NVIDIA sincronizar: a luta ia de 6% a 60% de CPU): o tamanho do texel
// vem de textureSize e o lado (+1 = direita da textura) vem do verde da cor do vértice (232 = +1, 231 = -1).
#if defined(PLATFORM_WEB)
#define SHADER_HEAD "#version 100\nprecision mediump float;\nvarying vec2 fragTexCoord;\nvarying vec4 fragColor;\n" \
                    "#define TEX texture2D\n#define OUT gl_FragColor\n"
#else
#define SHADER_HEAD "#version 330\nin vec2 fragTexCoord;\nin vec4 fragColor;\nout vec4 finalColor;\n" \
                    "#define TEX texture\n#define OUT finalColor\n"
#endif
static const char* SIL_FS = SHADER_HEAD "uniform sampler2D texture0;\nuniform vec4 colDiffuse;\n"
    "void main() { OUT = vec4(fragColor.rgb * colDiffuse.rgb, TEX(texture0, fragTexCoord).a * fragColor.a * colDiffuse.a); }\n";
static const char* RIM_FS = SHADER_HEAD "uniform sampler2D texture0;\nuniform vec4 colDiffuse;\n"
    "float A(vec2 uv) { return (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) ? 0.0 : TEX(texture0, uv).a; }\n"
    "void main() { float a = A(fragTexCoord);\n"
    "  vec2 texel = 1.0 / vec2(textureSize(texture0, 0));\n"
    "  float dir = fragColor.g > 231.5 / 255.0 ? 1.0 : -1.0;\n"
    "  float e = (a > 0.0 && (A(fragTexCoord + vec2(dir * texel.x, 0.0)) == 0.0 || A(fragTexCoord - vec2(0.0, texel.y)) == 0.0)) ? 1.0 : 0.0;\n"
    "  OUT = vec4(fragColor.rgb * colDiffuse.rgb, e * fragColor.a * colDiffuse.a); }\n";
static Shader SilShader() {
    static Shader s{};
    if (!s.id) s = LoadShaderFromMemory(nullptr, SIL_FS);
    return s;
}
static Shader RimShader() {
    static Shader s{};
    if (!s.id) s = LoadShaderFromMemory(nullptr, RIM_FS);
    return s;
}

Vector2 ShadowPoint(float x, float z, float groundY, int sunSide, float length) {
    return {x - sunSide * z * length, groundY + z * SHADOW_SQUASH};
}

void DrawAnimShadow(const Anim& a, int frame, float feetX, float z, float groundY, bool flipX, int sunSide,
                    float length, float alpha) {
    const Texture2D& t = a.frames[frame % a.frames.size()];  // silhueta (shader), pintada de preto pelo vértice
    float left = AnimLeft(a, frame, feetX, flipX), right = left + t.width;
    float fade = std::max(0.0f, 1 - z / t.height);  // mais alto no pulo = sombra mais fraca
    unsigned char aFeet = (unsigned char)(255 * alpha * fade), aHead = (unsigned char)(aFeet * 0.35f);
    Color feet = {0, 0, 0, aFeet}, head = {0, 0, 0, aHead};  // a sombra clareia longe dos pés
    float zFeet = z, zHead = z + t.height;
    // a sombra é o sprite espelhado na vertical; espelhar também na horizontal (e inverter u) mantém o sentido
    // anti-horário dos vértices, senão o GPU descarta o quadrilátero como "de costas"
    BeginShaderMode(SilShader());
    TexQuad(t, !flipX, ShadowPoint(right, zHead, groundY, sunSide, length), ShadowPoint(right, zFeet, groundY, sunSide, length),
            ShadowPoint(left, zFeet, groundY, sunSide, length), ShadowPoint(left, zHead, groundY, sunSide, length),
            head, feet, feet, head);
    EndShaderMode();
}

void DrawAnimLight(const Anim& a, int frame, Rectangle dst, bool flipX, int sunSide, float shade, float warm, float rim) {
    if (shade <= 0 && warm <= 0 && rim <= 0) return;
    size_t i = frame % a.frames.size();
    const Texture2D& sil = a.frames[i];  // pintada pelo shader de silhueta
    Vector2 tl = {dst.x, dst.y}, bl = {dst.x, dst.y + dst.height};
    Vector2 br = {dst.x + dst.width, dst.y + dst.height}, tr = {dst.x + dst.width, dst.y};
    // lado contrário ao sol: penumbra azulada; lado do sol: luz quente. Dois degradês separados (cada um some até
    // o outro lado) para o meio não virar uma mistura marrom.
    auto a8 = [](float v) { return (unsigned char)std::clamp(v, 0.0f, 255.0f); };
    Color shadeOn = {35, 30, 80, a8(85 * shade)}, shadeOff = {35, 30, 80, 0}, warmOn = {255, 205, 130, a8(45 * warm)}, warmOff = {255, 205, 130, 0};
    Color sl = sunSide > 0 ? shadeOn : shadeOff, sr = sunSide > 0 ? shadeOff : shadeOn;
    Color wl = sunSide > 0 ? warmOff : warmOn, wr = sunSide > 0 ? warmOn : warmOff;
    BeginShaderMode(SilShader());
    TexQuad(sil, flipX, tl, bl, br, tr, sl, sl, sr, sr);
    TexQuad(sil, flipX, tl, bl, br, tr, wl, wl, wr, wr);
    EndShaderMode();
    // borda de 1 px nos contornos virados para o sol (e no topo): com o sprite espelhado, o lado do sol na tela
    // é o outro lado da textura
#if defined(PLATFORM_WEB)
    return;  // shortcut: GLSL ES 1.0 não tem textureSize; na web fica sem a borda de luz
#endif
    if (rim <= 0) return;
    unsigned char g = (sunSide > 0) != flipX ? 232 : 231;  // lado do sol na textura, lido pelo shader
    BeginShaderMode(RimShader());
    Rectangle src = {0, 0, (float)(flipX ? -sil.width : sil.width), (float)sil.height};
    DrawTexturePro(sil, src, dst, {0, 0}, 0, {255, g, 170, a8(160 * rim)});
    EndShaderMode();
}

float AnimLeft(const Anim& a, int frame, float feetX, bool flipX) {
    size_t i = frame % a.frames.size();
    float anchor = flipX ? a.frames[i].width - a.anchors[i] : a.anchors[i];
    return feetX - anchor;
}

Rectangle DrawAnimFrame(const Anim& a, int frame, Vector2 feet, bool flipX, bool flash, float pixelsPerUnit,
                        float stretch, Color tint) {
    const Texture2D& t = a.frames[frame % a.frames.size()];
    Rectangle src = {0, 0, (float)(flipX ? -t.width : t.width), (float)t.height};
    // encaixa na grade de pixels da tela: cada pixel do sprite vira exatamente pixelsPerUnit pixels, sem serrilhar
    auto snap = [&](float v) { return roundf(v * pixelsPerUnit) / pixelsPerUnit; };
    float top = snap(feet.y - t.height - stretch), bottom = snap(feet.y);  // pés fixos, só o topo sobe
    Rectangle dst = {snap(AnimLeft(a, frame, feet.x, flipX)), top, (float)t.width, bottom - top};
    if (flash) BeginShaderMode(SilShader());  // flash de dano: silhueta na cor `tint` (branca)
    DrawTexturePro(t, src, dst, {0, 0}, 0, tint);
    if (flash) EndShaderMode();
    return dst;
}
