#include "fighter.h"
#include "theme.h"
#include "typography.h"
#include "rlgl.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#ifdef PLATFORM_WEB
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

static const char* DATA = "assets/data/characters.json";
static const char* SETTINGS = "settings.json";  // opções do jogador, gravadas ao mudar
static const char* LOGO = "assets/raw/menu/logo.png";  // logo do menu inicial (fundo magenta); sem ele, título em texto
static const char* MENU = "assets/data/menu.json";      // ritmo, zoom, escurecimento e partículas do menu inicial
static Texture2D logo{}, logoShine{};  // shine = silhueta branca do logo (máscara da faixa de brilho)
static const char* KO_ART = "assets/ui/ko.png";  // opcional (fundo magenta): substitui as letras desenhadas do K.O.
const float KO_ART_W = 180, KO_ART_H = 110, KO_ART_SMALL = 0.28f;  // caixa do K.O. grande (px) e escala no topo
static Texture2D koArt{}, koArtSmall{};  // grande e a do topo (cada uma 1:1 na tela, nítidas)

// Menu inicial: cena animada de cada cartão (assets/raw/menu/card_<id>_1..4.png) e as partículas do escolhido.
static const char* CARD_IDS[4] = {"cpu", "2p", "opcoes", "sair"};  // na ordem de CARDS
const int CARD_ART = 118;  // lado da cena em px lógicos (cobre o cartão escolhido, maior, e o parallax)
struct MenuCfg {
    float fps = 5, fpsDim = 2, zoom = 1.05f, dim = 0.6f, parallax = 2, life = 1;
    float rate[4] = {30, 45, 40, 35};  // partículas por segundo
    Color color[4] = {{255, 60, 60, 255}, {90, 180, 255, 255}, {255, 220, 60, 255}, {255, 200, 90, 255}};
};
static MenuCfg menu;
static std::vector<Texture2D> cardFrames[4];

static Roster roster;
static bool loaded = false;
static Fighter p[2];
static Input in[2];
static int hitstop = 0;
static bool cpu = false, debug = false, quit = false;
static float worldCam = 0;  // câmera horizontal da luta (arena larga), interpolada; 0 na sala
static int koTicks = 0, menuSel = 0;  // menu do fim da luta
static float koT = -1;  // segundos desde o K.O. (tela de fim de luta); -1 = sem K.O.
// "ROUND N" segura todo mundo parado no começo de cada round; quem vence `opt.wins` rounds leva a luta.
const int INTRO_TICKS = 60, NEXT_ROUND_TICKS = 150;
static int wins[2] = {}, roundNum = 1, roundTicks = 0;
static int chosen[2];   // personagens da luta (índices em roster.chars), vindos da tela de escolha
static int chosenPet[2] = {-1, -1};  // suporte de cada um (índice em roster.supports.list; -1 = nenhum)
static float trail[2];  // vida mostrada no rastro vermelho: escorre devagar até a vida de verdade

// Opções (tela OPÇÕES, gravadas em settings.json). Efeitos ficam guardados: o jogo ainda não tem efeitos sonoros.
struct Options { int scale = 3; bool fullscreen = false; int music = 80, sfx = 80, wins = 2, cpuLevel = 1; bool maxFps = false; };
static Options opt;

// Telas. Esc volta: escolha -> menu inicial; luta -> pausa; opções -> de onde veio.
enum class Screen { Loading, Title, Select, Versus, Fight, Options };
static Screen screen = Screen::Title, optionsFrom = Screen::Title;
static bool paused = false;
static int pauseSel = 0, optSel = 0;
static float camY = 0;  // câmera vertical da luta (px lógicos que o mundo desce; ver CAM_UP_MAX)
// Transição entre telas: cortina inclinada entra (0..1), a troca acontece coberta, e ela sai (1..2).
static float transT = -1;
static Color transColor = BLACK, transEdge = {215, 35, 50, 255};  // cortina (a saída do loading usa as do tema)
static std::function<void()> transDo;
static void GoTo(std::function<void()> change) {
    if (transT >= 0) return;
    transT = 0;
    transDo = std::move(change);
}

static int titleSel = 0;
static float titlePop[4];    // 0..1: quanto cada cartão está "saltado" (anima até o alvo)
static int cursor[2], pick[2];  // cartão sob o cursor de cada jogador; personagem confirmado (-1 = escolhendo)
static double selectAt = 0;     // quando a tela de escolha abriu (animação de entrada)
static int petCursor[2], petPick[2];  // suporte sob o cursor e o confirmado (-1 = escolhendo)
static int selectStage = 0;           // tela de escolha: 0 = lutadores, 1 = suportes (os dois já com lutador), 2 = arena
static double pickedAt[2], readyAt;
static double acc = 0;
static std::string msg;
static double msgUntil = 0;

// Texto do jogo: fonte pixel de UI (typography.h). `size` no jeito antigo (10 = 1x, 20 = 2x...) vira escala inteira.
static int TextScale(int size) { return std::max(1, (size + 5) / 10); }
static void Text(const char* t, int x, int y, int size, Color c) { DrawUIText(t, {(float)x, (float)y}, TextScale(size), c); }
static int TextW(const char* t, int size) { return MeasureUIText(t, TextScale(size)); }

// TextToUpper da raylib só sobe ASCII ("Pérola" virava "PéROLA"): sobe também as minúsculas acentuadas (UTF-8 C3 xx).
static const char* Upper(const char* t) {
    unsigned char* s = (unsigned char*)TextToUpper(t);  // buffer estático da raylib
    for (unsigned char* c = s; *c; c++)
        if (c[0] == 0xC3 && c[1] >= 0xA0 && c[1] <= 0xBE && c[1] != 0xB7) c[1] -= 0x20;  // B7 = ÷
    return (const char*)s;
}

static void Say(const std::string& s) {
    msg = s;
    msgUntil = GetTime() + 3;
    TraceLog(LOG_INFO, "JOGO: %s", s.c_str());
}

static void StartRound() {
    float mid = (arena.x0 + arena.x1) / 2, gap = SCREEN_W / 2.0f - 120;  // no meio da arena (sala: 120 e 264)
    arena.cam = arena.prevCam = mid - SCREEN_W / 2.0f;
    p[0] = MakeFighter(roster.chars[chosen[0]], mid - gap, SKYBLUE);
    p[1] = MakeFighter(roster.chars[chosen[1]], mid + gap, ORANGE);
    for (int i = 0; i < 2; i++) {
        p[i].pets = roster.supports.list.empty() ? nullptr : &roster.supports;
        p[i].petChoice = chosenPet[i] < (int)roster.supports.list.size() ? chosenPet[i] : -1;
    }
    in[0] = in[1] = {};
    hitstop = 0;
    koTicks = menuSel = roundTicks = 0;
    koT = -1;
    camY = 0;
    for (int i = 0; i < 2; i++) trail[i] = (float)p[i].hp;
}

static void StartFight() {  // luta nova: zera os rounds
    wins[0] = wins[1] = 0;
    roundNum = 1;
    StartRound();
}

static bool MatchOver() { return wins[0] >= opt.wins || wins[1] >= opt.wins; }

// Escolhas feitas: luta nova com os lutadores e suportes da tela de escolha.
static void StartMatch() {
    for (int i = 0; i < 2; i++) {
        chosen[i] = pick[i];
        chosenPet[i] = petPick[i];
    }
    screen = Screen::Fight;
    paused = false;
    StartFight();
}

static void OpenSelect() {
    screen = Screen::Select;
    paused = false;
    cursor[0] = roster.p1;  // cursores começam em "p1"/"p2" do JSON
    cursor[1] = roster.p2;
    selectAt = GetTime();
    pick[0] = pick[1] = -1;
    selectStage = 0;
    for (int i = 0; i < 2; i++) {
        petPick[i] = -1;
        petCursor[i] = std::max(0, chosenPet[i]);  // começa no último escolhido
    }
    readyAt = 0;
}

static void UnloadMenu() {
    for (Texture2D* t : {&logo, &logoShine, &koArt, &koArtSmall})
        if (t->id) UnloadTexture(*t), *t = {};
    for (auto& v : cardFrames) {
        for (auto& t : v) UnloadTexture(t);
        v.clear();
    }
}

// menu.json (valores que faltarem ficam no padrão) e logo; as cenas dos cartões vêm em LoadCard. F5 recarrega.
static void LoadMenuBase(int tol) {
    UnloadMenu();
    menu = {};
    try {
        std::ifstream f(MENU);
        nlohmann::json j = f ? nlohmann::json::parse(f) : nlohmann::json::object();
        menu.fps = j.value("fps", menu.fps);
        menu.fpsDim = j.value("fps_unselected", menu.fpsDim);
        menu.zoom = j.value("zoom", menu.zoom);
        menu.dim = std::clamp(j.value("dim", menu.dim), 0.0f, 1.0f);
        menu.parallax = j.value("parallax", menu.parallax);
        nlohmann::json ps = j.value("particles", nlohmann::json::object());
        menu.life = ps.value("life", menu.life);
        for (int i = 0; i < 4; i++) {
            nlohmann::json c = ps.value(CARD_IDS[i], nlohmann::json::object());
            menu.rate[i] = c.value("rate", menu.rate[i]);
            if (c.contains("color") && c["color"].size() >= 3)
                menu.color[i] = {(unsigned char)c["color"][0].get<int>(), (unsigned char)c["color"][1].get<int>(),
                                 (unsigned char)c["color"][2].get<int>(), 255};
        }
    } catch (const std::exception& e) {
        Say(std::string("Erro no menu.json: ") + e.what());
    }

    logo = LoadPicture(LOGO, tol, 120 * PICTURE_RES, 60 * PICTURE_RES, false);
    koArt = LoadPicture(KO_ART, tol, (int)(KO_ART_W * PICTURE_RES), (int)(KO_ART_H * PICTURE_RES), false);
    koArtSmall = LoadPicture(KO_ART, tol, (int)(KO_ART_W * KO_ART_SMALL * PICTURE_RES), (int)(KO_ART_H * KO_ART_SMALL * PICTURE_RES), false);
    if (logo.id) {  // silhueta branca com 1 px transparente em volta (a borda esticada pelo CLAMP fica invisível)
        Image s = LoadImageFromTexture(logo);
        ImageFormat(&s, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        Color* px = (Color*)s.data;
        for (int k = 0; k < s.width * s.height; k++) px[k] = {255, 255, 255, px[k].a};
        ImageResizeCanvas(&s, s.width + 2, s.height + 2, 1, 1, BLANK);
        logoShine = LoadTextureFromImage(s);
        SetTextureWrap(logoShine, TEXTURE_WRAP_CLAMP);
        UnloadImage(s);
    }
}

// Cena animada do cartão i (quadros 1..4).
static void LoadCard(int i) {
    for (int n = 1; n <= 4; n++) {
        const char* file = TextFormat("assets/raw/menu/card_%s_%d.png", CARD_IDS[i], n);
        Image img = FileExists(file) ? LoadImage(file) : Image{};
        if (!img.data) {  // faltou um quadro: fica só o 1º, parado
            for (size_t k = 1; k < cardFrames[i].size(); k++) UnloadTexture(cardFrames[i][k]);
            cardFrames[i].resize(std::min<size_t>(1, cardFrames[i].size()));
            break;
        }
        int side = std::min(img.width, img.height);  // quadrado do meio da cena
        ImageCrop(&img, {(img.width - side) / 2.0f, (img.height - side) / 2.0f, (float)side, (float)side});
        // ilustração, não pixel art: reduz com média (sem chuvisco); na tela fica 1:1 no zoom 3x
        ImageResize(&img, CARD_ART * PICTURE_RES, CARD_ART * PICTURE_RES);
        Texture2D t = LoadTextureFromImage(img);
        SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
        cardFrames[i].push_back(t);
        UnloadImage(img);
    }
}

static void LoadMenu() {
    LoadMenuBase(roster.chromaTolerance);
    for (int i = 0; i < 4; i++) LoadCard(i);
}

static void LoadStage();  // arena (stages.json)
static void LoadStageList();
static void OpenStageSelect();
static const char* StageName();
static void TvHit();
static void UpdateLife();  // cenário em camadas (Mercadão): cliente, pombos, nuvens
static void StageHype();   // torcida empolgada (golpe forte, especial, K.O.)
static void UpdatePlatform();

static void Reload() {
    Roster r;
    std::string err;
    if (!LoadRoster(DATA, r, err)) { Say("Erro no JSON: " + err); return; }
    UnloadRoster(roster);
    roster = std::move(r);
    loaded = true;
    LoadStage();
    LoadStageList();
    for (int& c : chosen) c = std::min(c, (int)roster.chars.size() - 1);  // o JSON novo pode ter menos gente
    if (screen == Screen::Select) OpenSelect();
    else if (screen == Screen::Fight) StartFight();
    LoadMenu();
    Say("Dados carregados");
}

// Efeitos sonoros: todo arquivo de assets/sfx (wav/ogg/mp3), pelo nome sem extensão. Volume = EFEITOS das opções.
static std::map<std::string, Sound> sounds;

static void LoadSounds() {
    FilePathList fl = LoadDirectoryFilesEx("assets/sfx", ".wav;.ogg;.mp3", false);
    for (unsigned k = 0; k < fl.count; k++) sounds[GetFileNameWithoutExt(fl.paths[k])] = LoadSound(fl.paths[k]);
    UnloadDirectoryFiles(fl);
}

// Vozes dos personagens (as do JSON) tocam mais baixo, no "volume" do "voice" de quem fala.
static float VoiceGain(const std::string& n) {
    for (const CharDef& d : roster.chars)
        for (const std::string* v : {&d.hurtVoice, &d.koVoice, &d.winVoice, &d.punch.voice, &d.kick.voice, &d.airAttack.voice,
                                     &d.special.voice, &d.crouchPunch.voice, &d.crouchKick.voice, &d.parry.voice})
            if (*v == n) return d.voiceVolume;
    return 1;
}

static void PlaySfx(const char* name) {
    auto it = sounds.find(name);
    if (it == sounds.end()) return;
    SetSoundVolume(it->second, opt.sfx / 100.0f * VoiceGain(name));
    PlaySound(it->second);
}

// Controle de videogame: o 1º conectado é o P1, o 2º é o P2 (botões pela posição: A/Cruz embaixo, B/Bola à
// direita, X/Quadrado à esquerda, Y/Triângulo em cima). Direcional ou analógico esquerdo movem. Na luta: Quadrado =
// soco, Triângulo = chute, RB/R1 = suporte, Start = pausa. Nos menus: Cruz confirma; Bola desfaz nas telas de
// escolha e volta no menu inicial, nas opções e na pausa; Start volta.
enum PadButton { PAD_LEFT, PAD_RIGHT, PAD_UP, PAD_DOWN, PAD_OK, PAD_BACK, PAD_PUNCH, PAD_KICK, PAD_SUPPORT, PAD_START, PAD_N };
static bool padDown[2][PAD_N], padPrev[2][PAD_N];

// Controle de toque (celular): controle virtual do P1 por cima de tudo, em px da tela inteira / zoom (nos celulares
// mais largos que 16:9 fica nas faixas pretas dos lados). Entra como o controle 0, então os menus funcionam sem
// código próprio. Na luta, um botão por golpe especial do lutador, com o comando já feito (Input.cmd).
struct TouchBtn { const char* label; Vector2 c; float r; int pad, cmd; bool kick; };
static bool touchUi = false, touchSeen = false, touchHeld[PAD_N];
static int touchCmd = 0;
static bool touchKick = false;
static Vector2 touchStick{};  // deslocamento do polegar no direcional (só desenho)
static float TouchZoom() { return (float)std::max(1, std::min(GetScreenWidth() / SCREEN_W, GetScreenHeight() / SCREEN_H)); }
static Vector2 TouchView() { return {GetScreenWidth() / TouchZoom(), GetScreenHeight() / TouchZoom()}; }
static Vector2 StickCenter() { return {34, TouchView().y - 40}; }
static bool TouchFight() { return loaded && screen == Screen::Fight && !paused && koT < 0; }

static std::vector<TouchBtn> TouchButtons() {
    Vector2 v = TouchView();
    bool f = TouchFight();
    std::vector<TouchBtn> b = {{f ? "SOCO" : "OK", {v.x - 62, v.y - 26}, 16, f ? PAD_PUNCH : PAD_OK, 0, false},
                               {f ? "CHUTE" : "VOLTA", {v.x - 26, v.y - 46}, 16, f ? PAD_KICK : PAD_BACK, 0, true}};
    if (loaded && screen == Screen::Fight && !paused) b.push_back({"II", {11, 11}, 9, PAD_START, 0, false});  // pausa no canto
    if (!f) return b;
    const CharDef& d = *p[0].def;
    b.push_back({"SUP", {v.x - 96, v.y - 16}, 10, PAD_SUPPORT, 0, false});
    Vector2 slot[3] = {{v.x - 94, v.y - 46}, {v.x - 62, v.y - 62}, {v.x - 26, v.y - 82}};
    int n = 0;
    auto add = [&](const char* l, int cmd, bool kick) { if (n < 3) b.push_back({l, slot[n++], 12, -1, cmd, kick}); };
    if (d.hasSpecial) add("ESP", 1, false);
    if (d.suitcase.on) add("ESP2", 1, true);
    if (d.hasSuper || (d.dash.on && d.dash.halfCircle)) add("SUPER", 3, true);
    if (d.dash.on && !d.dash.halfCircle) add("DASH", 4, false);
    if (d.parry.on) add("PARRY", 2, false);
    return b;
}

static std::vector<Vector2> TouchPoints() {  // dedos na tela, em px da tela
    std::vector<Vector2> pts;
#ifdef PLATFORM_WEB
    // o raylib web perde quem soltou com 2 dedos na tela: a página guarda os toques ativos (web/shell.html)
    static float xy[20];
    int n = EM_ASM_INT({
        var a = Module.touchXY ? Module.touchXY() : [];
        for (var i = 0; i < a.length && i < 20; i++) HEAPF32[($0 >> 2) + i] = a[i];
        return Math.min(a.length, 20) >> 1;
    }, xy);
    double cw, ch;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    float k = cw > 0 ? GetScreenWidth() / (float)cw : 1;
    for (int i = 0; i < n; i++) pts.push_back({xy[2 * i] * k, xy[2 * i + 1] * k});
#else
    for (int i = 0; i < GetTouchPointCount(); i++) pts.push_back(GetTouchPosition(i));
#endif
    return pts;
}

static void UpdateTouch() {
    static bool prev[8] = {};
    std::fill(touchHeld, touchHeld + PAD_N, false);
    touchStick = {};
    std::vector<Vector2> pts = TouchPoints();
    touchSeen |= !pts.empty();
    touchUi |= !pts.empty();
    if (!touchUi) return;
    float z = TouchZoom();
    for (Vector2& t : pts) t = {t.x / z, t.y / z};
    if (!touchSeen && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) pts.push_back({GetMouseX() / z, GetMouseY() / z});  // teste no PC
    std::vector<TouchBtn> b = TouchButtons();
    bool held[8] = {};
    Vector2 v = TouchView(), c = StickCenter();
    for (Vector2 t : pts) {
        if (t.x < v.x * 0.4f && t.y > v.y * 0.3f) {  // direcional: 8 direções a partir do centro, folga de 6 px
            float dx = t.x - c.x, dy = t.y - c.y, len = sqrtf(dx * dx + dy * dy), k = std::min(1.0f, 16 / std::max(len, 1.0f));
            touchStick = {dx * k, dy * k};
            if (len < 6) continue;
            touchHeld[PAD_LEFT] |= dx < -0.38f * len;
            touchHeld[PAD_RIGHT] |= dx > 0.38f * len;
            touchHeld[PAD_UP] |= dy < -0.38f * len;
            touchHeld[PAD_DOWN] |= dy > 0.38f * len;
            continue;
        }
        int best = -1;  // o botão mais perto (em raios), até 1,3 raio
        float bd = 1.3f;
        for (int i = 0; i < (int)b.size() && i < 8; i++) {
            float d = sqrtf((t.x - b[i].c.x) * (t.x - b[i].c.x) + (t.y - b[i].c.y) * (t.y - b[i].c.y)) / b[i].r;
            if (d < bd) bd = d, best = i;
        }
        if (best >= 0) held[best] = true;
    }
    for (int i = 0; i < (int)b.size() && i < 8; i++) {
        if (held[i] && b[i].pad >= 0) touchHeld[b[i].pad] = true;
        if (held[i] && !prev[i] && b[i].cmd) touchCmd = b[i].cmd, touchKick = b[i].kick;
        prev[i] = held[i];
    }
}

static void DrawTouch() {
    if (!touchUi) return;
    float z = TouchZoom();
    Vector2 v = TouchView(), c = StickCenter();
    BeginMode2D(Camera2D{{0, 0}, {0, 0}, 0, z});
    if (v.y > v.x) {  // celular em pé: o jogo é deitado
        int w = MeasureUIText("GIRE O CELULAR", 1);
        DrawUIText("GIRE O CELULAR", {roundf((v.x - w) / 2), roundf(v.y / 2)}, 1, WHITE, true);
    }
    DrawCircleV(c, 26, Fade(BLACK, 0.3f));
    DrawCircleLinesV(c, 26, Fade(WHITE, 0.4f));
    bool dir = touchStick.x != 0 || touchStick.y != 0;
    DrawCircleV({c.x + touchStick.x, c.y + touchStick.y}, 11, Fade(WHITE, dir ? 0.6f : 0.3f));
    for (const TouchBtn& b : TouchButtons()) {
        bool on = b.pad >= 0 && touchHeld[b.pad];
        DrawCircleV(b.c, b.r, Fade(b.cmd ? Color{200, 40, 40, 255} : BLACK, on ? 0.6f : 0.35f));
        DrawCircleLinesV(b.c, b.r, Fade(WHITE, on ? 0.9f : 0.45f));
        int w = MeasureUIText(b.label, 1);
        DrawUIText(b.label, {roundf(b.c.x - w / 2.0f), roundf(b.c.y - 4)}, 1, Fade(WHITE, 0.85f), true);
    }
    EndMode2D();
}

static void ReadPads() {  // uma vez por quadro: o "apertou" do analógico sai da comparação com o quadro anterior
    for (int i = 0; i < 2; i++) {
        std::copy(padDown[i], padDown[i] + PAD_N, padPrev[i]);
        bool on = IsGamepadAvailable(i);
        auto btn = [&](int b) { return on && IsGamepadButtonDown(i, b); };
        float ax = on ? GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_X) : 0, ay = on ? GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_Y) : 0;
        padDown[i][PAD_LEFT] = btn(GAMEPAD_BUTTON_LEFT_FACE_LEFT) || ax < -0.5f;
        padDown[i][PAD_RIGHT] = btn(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || ax > 0.5f;
        padDown[i][PAD_UP] = btn(GAMEPAD_BUTTON_LEFT_FACE_UP) || ay < -0.5f;
        padDown[i][PAD_DOWN] = btn(GAMEPAD_BUTTON_LEFT_FACE_DOWN) || ay > 0.5f;
        padDown[i][PAD_OK] = btn(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        padDown[i][PAD_BACK] = btn(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        padDown[i][PAD_PUNCH] = btn(GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
        padDown[i][PAD_KICK] = btn(GAMEPAD_BUTTON_RIGHT_FACE_UP);
        padDown[i][PAD_SUPPORT] = btn(GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
        padDown[i][PAD_START] = btn(GAMEPAD_BUTTON_MIDDLE_RIGHT);
    }
    UpdateTouch();
    for (int b = 0; b < PAD_N; b++) padDown[0][b] |= touchHeld[b];
}
static bool PadHeld(int i, int b) { return padDown[i][b]; }
static bool PadPressed(int i, int b) { return padDown[i][b] && !padPrev[i][b]; }
static bool AnyPad(int b) { return PadPressed(0, b) || PadPressed(1, b); }
static bool Press(int i, int key, int pad) { return IsKeyPressed(key) || PadPressed(i, pad); }  // tecla ou botão do jogador i

static void ReadInput() {
    // segurar vale no tick; apertar fica guardado até o próximo tick rodar
    in[0].left = IsKeyDown(KEY_A) || PadHeld(0, PAD_LEFT);
    in[0].right = IsKeyDown(KEY_D) || PadHeld(0, PAD_RIGHT);
    in[0].down = IsKeyDown(KEY_S) || PadHeld(0, PAD_DOWN);
    in[0].up = IsKeyDown(KEY_W) || PadHeld(0, PAD_UP);
    in[0].punch |= Press(0, KEY_J, PAD_PUNCH);
    in[0].kick |= Press(0, KEY_K, PAD_KICK);
    in[0].punchHeld = IsKeyDown(KEY_J) || PadHeld(0, PAD_PUNCH);
    in[1].left = IsKeyDown(KEY_LEFT) || PadHeld(1, PAD_LEFT);
    in[1].right = IsKeyDown(KEY_RIGHT) || PadHeld(1, PAD_RIGHT);
    in[1].down = IsKeyDown(KEY_DOWN) || PadHeld(1, PAD_DOWN);
    in[1].up = IsKeyDown(KEY_UP) || PadHeld(1, PAD_UP);
    in[1].punch |= Press(1, KEY_KP_1, PAD_PUNCH);
    in[1].kick |= Press(1, KEY_KP_2, PAD_KICK);
    in[1].punchHeld = IsKeyDown(KEY_KP_1) || PadHeld(1, PAD_PUNCH);
    // suporte: 1 ou L (P1), Numpad 7 ou Numpad 3 (P2); o aperto fica guardado até o próximo tick, como os golpes
    in[0].support |= IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_L) || PadPressed(0, PAD_SUPPORT);
    in[1].support |= IsKeyPressed(KEY_KP_7) || IsKeyPressed(KEY_KP_3) || PadPressed(1, PAD_SUPPORT);
    if (touchCmd) {  // botão de golpe especial do controle de toque: comando pronto + soco/chute
        in[0].cmd = touchCmd;
        if (touchCmd != 4) (touchKick ? in[0].kick : in[0].punch) = true;
        touchCmd = 0;
    }
}

static void Tick() {
    for (Fighter& f : p) {
        f.prevX = f.x; f.prevY = f.y; f.shot.prevX = f.shot.x; f.shot.prevY = f.shot.y; f.pet.prevX = f.pet.x;
        for (Prop& o : f.props) { o.prevX = o.x; o.prevY = o.y; }
    }
    arena.prevCam = arena.cam;
    UpdateLife();
    UpdatePlatform();
    if (hitstop > 0) { hitstop--; return; }  // no hitstop o rastro também para
    for (Fighter& f : p)  // rastro de dano: derrete até a vida nova
        trail[&f - p] = std::max((float)f.hp, trail[&f - p] - f.def->health / 30.0f);  // barra inteira em ~0,5 s
    roundTicks++;
    if (p[0].act == Act::KO || p[1].act == Act::KO) {
        if (koTicks++ == 0)  // K.O. duplo: o round não conta para ninguém
            for (int i = 0; i < 2; i++) wins[i] += p[1 - i].act == Act::KO && p[i].act != Act::KO;
        if (!MatchOver() && koTicks >= NEXT_ROUND_TICKS) { roundNum++; StartRound(); return; }
        UpdateAfterKO(p[0], p[1]);
        return;
    }
    if (roundTicks <= INTRO_TICKS) {  // "ROUND N": animações seguem, ninguém comanda ainda
        UpdateAfterKO(p[0], p[1]);
        for (Input& i : in) { i.punch = i.kick = false; i.support = i.cmd = 0; }
        return;
    }
    Input i2 = cpu ? CpuInput(p[1], p[0], opt.cpuLevel) : in[1];
    int hp0[2] = {p[0].hp, p[1].hp};
    UpdateFight(p[0], p[1], in[0], i2, hitstop);
    for (int i = 0; i < 2; i++)
        if (hp0[i] - p[i].hp >= 9) { TvHit(); StageHype(); }  // golpe forte (a torcida vibra)
    for (const Fighter& f : p)
        if ((f.act == Act::Special || f.act == Act::Super) && f.t == 0) StageHype();  // especial
    for (Input& i : in) { i.punch = i.kick = false; i.support = i.cmd = 0; }
    if (koT < 0 && (p[0].act == Act::KO || p[1].act == Act::KO)) {  // K.O.: a luta congela 6 ticks
        koT = 0;
        StageHype();
        hitstop = std::max(hitstop, 6);
    }
}

static bool MenuOpen() { return MatchOver() && koT >= 2.8f; }  // painel do menu já parado

// Teclas de menu, valem para os dois jogadores (e os dois controles)
static bool Beep(bool hit, const char* sfx) { if (hit) PlaySfx(sfx); return hit; }  // menu: som de cursor/confirmar
static bool KeyUp() { return Beep(IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP) || AnyPad(PAD_UP), "cursor"); }
static bool KeyDown() { return Beep(IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN) || AnyPad(PAD_DOWN), "cursor"); }
static bool KeyLeft() { return Beep(IsKeyPressed(KEY_A) || IsKeyPressed(KEY_LEFT) || AnyPad(PAD_LEFT), "cursor"); }
static bool KeyRight() { return Beep(IsKeyPressed(KEY_D) || IsKeyPressed(KEY_RIGHT) || AnyPad(PAD_RIGHT), "cursor"); }
static bool KeyOk() {
    return Beep(IsKeyPressed(KEY_J) || IsKeyPressed(KEY_KP_1) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || AnyPad(PAD_OK), "confirmar");
}
// Bola do controle só volta onde não é "desfaz": menu inicial, opções e pausa
static bool KeyBack() {
    bool bBacks = screen == Screen::Title || screen == Screen::Options || (screen == Screen::Fight && paused);
    return IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || AnyPad(PAD_START) || (bBacks && AnyPad(PAD_BACK));
}

static void OpenOptions(Screen from) {
    optionsFrom = from;
    screen = Screen::Options;
    optSel = 0;
}

// Fim da luta: W/S ou ↑/↓ escolhem; J, Numpad 1 ou Enter confirmam.
static const char* MENU_ITEMS[] = {"REVANCHE", "TROCAR LUTADORES", "MENU INICIAL", "SAIR DO JOGO"};
const int MENU_N = 4;

static void UpdateMenu() {
    if (!MenuOpen()) return;
    if (KeyUp()) menuSel = (menuSel + MENU_N - 1) % MENU_N;
    if (KeyDown()) menuSel = (menuSel + 1) % MENU_N;
    if (!KeyOk()) return;
    if (menuSel == 0) { PlaySfx("rewind"); GoTo(StartFight); }  // revanche: rebobina a fita
    else if (menuSel == 1) GoTo(OpenSelect);
    else if (menuSel == 2) GoTo([] { screen = Screen::Title; });
    else GoTo([] { quit = true; });
}

// Pausa (Esc na luta): a luta congela embaixo; Esc de novo continua.
static const char* PAUSE_ITEMS[] = {"CONTINUAR", "OPÇÕES", "TROCAR LUTADORES", "MENU INICIAL"};
const int PAUSE_N = 4;

static void UpdatePause() {
    if (KeyBack()) { paused = false; return; }
    if (KeyUp()) pauseSel = (pauseSel + PAUSE_N - 1) % PAUSE_N;
    if (KeyDown()) pauseSel = (pauseSel + 1) % PAUSE_N;
    if (!KeyOk()) return;
    if (pauseSel == 0) paused = false;
    else if (pauseSel == 1) GoTo([] { OpenOptions(Screen::Fight); });  // continua pausado ao voltar
    else if (pauseSel == 2) GoTo(OpenSelect);
    else GoTo([] { paused = false; screen = Screen::Title; });
}

static void OutlinedText(const char* t, int x, int y, int size, Color fill, int border);
static void Band(float x0, float x1, float y, float h, float slant, Color c);
static void DrawMenuBackground();
static void DrawCornerTitle(const char* title, const char* right);

// Lista de menu no meio da tela: faixas inclinadas, a escolhida em amarelo.
static void DrawList(const char* title, const char* const* items, int n, int sel, int y) {
    int rowH = 15, h = 10 + (title ? 24 : 0) + n * rowH + 14;
    float w = 176, x = SCREEN_W / 2 - w / 2, fy = (float)y, fh = (float)h;
    Band(x - 2, x + w + 2, fy - 2, fh + 4, 6, BLACK);
    Band(x, x + w, fy, fh, 6, {30, 22, 40, 235});
    int ty = y + 6;
    if (title) {
        OutlinedText(title, SCREEN_W / 2 - TextW(title, 20) / 2, ty, 20, {255, 80, 90, 255}, 1);
        ty += 24;
    }
    for (int i = 0; i < n; i++) {
        float ry = (float)(ty + i * rowH);
        if (i == sel) Band(x + 10, x + w - 6, ry - 2, 13, 3, {255, 215, 60, 255});
        Text(items[i], SCREEN_W / 2 - TextW(items[i], 10) / 2, (int)ry, 10, i == sel ? BLACK : LIGHTGRAY);
    }
    const char* hint = "W/S ESCOLHE   J/ENTER CONFIRMA";
    Text(hint, SCREEN_W / 2 - TextW(hint, 10) / 2, y + h - 12, 10, Fade(WHITE, 0.5f));
}

// Quadrilátero com cor indo de `a` (lado p1-p4) para `b` (lado p2-p3). Culling desligado por quem chama:
// o lado do P2 é o do P1 espelhado, e espelhar inverte a ordem dos vértices.
static void Quad(Vector2 p1, Vector2 p2, Vector2 p3, Vector2 p4, Color a, Color b) {
    auto v = [](Vector2 q, Color c) { rlColor4ub(c.r, c.g, c.b, c.a); rlVertex2f(q.x, q.y); };
    rlBegin(RL_TRIANGLES);
    v(p1, a); v(p4, a); v(p3, b);
    v(p1, a); v(p3, b); v(p2, b);
    rlEnd();
}

// Ícone redondo do HUD: aro preto e branco (ou `ring`), ícone dentro (espelhado no P2, olhando para o centro).
// Sem personagem (vaga de suporte): círculo escuro com "?".
static void HudIcon(Vector2 c, float r, const CharDef* who, bool flip, Color ring = WHITE) {
    DrawCircleV(c, r + 2, BLACK);
    DrawCircleV(c, r + 1, who ? ring : GRAY);
    DrawCircleV(c, r, {30, 30, 45, 255});
    if (who && who->icon.id) {
        const Texture2D& t = who->icon;
        DrawTexturePro(t, {0, 0, (float)(flip ? -t.width : t.width), (float)t.height}, {c.x - r, c.y - r, 2 * r, 2 * r}, {0, 0}, 0, WHITE);
    } else {
        const char* t = who ? TextFormat("%c", who->name[0]) : "?";
        Text(t, (int)(c.x - TextW(t, 10) / 2.0f), (int)c.y - 4, 10, who ? WHITE : GRAY);
    }
}

// Ícone redondo de mascote: aceso (cor) ou apagado (escuro), aro na cor pedida.
static void PetIcon(Vector2 c, float r, const SupportDef& s, bool flip, bool lit, Color ring) {
    DrawCircleV(c, r + 2, BLACK);
    DrawCircleV(c, r + 1, ring);
    DrawCircleV(c, r, {30, 30, 45, 255});
    Color tint = lit ? WHITE : Color{135, 135, 150, 255};  // apagado, mas ainda dá para ver quem é
    if (s.icon.id)
        DrawTexturePro(s.icon, {0, 0, (float)(flip ? -s.icon.width : s.icon.width), (float)s.icon.height},
                       {c.x - r, c.y - r, 2 * r, 2 * r}, {0, 0}, 0, tint);
    else
        Text(TextFormat("%c", s.name[0]), (int)c.x - 3, (int)c.y - 4, 10, tint);
}

// ---------- fim de luta (pós-K.O.) ----------
// Tempo 1 (0-1,2 s): a luta congela 6 ticks, flash branco, tremida, a arena escurece e o "K.O." cai letra por letra.
// Tempo 2 (1,2-2,5 s, só quando a luta acaba): o K.O. encolhe para o topo e a faixa do vencedor entra pela esquerda.
// Tempo 3 (2,5 s em diante): o painel do menu desliza da direita. Round que não fecha a luta: só o tempo 1.
// Texto em typography.h (K.O., nome e "VENCEU!" no mesmo estilo Display do "VS"); posições em px inteiros.
static float menuShift[4];
const float TAN12 = 0.2126f;  // inclinação de 12°
const Color INK_DARK = {14, 10, 22, 255};

static float EaseOut(float k) { k = std::clamp(k, 0.0f, 1.0f); return 1 - (1 - k) * (1 - k) * (1 - k); }
static int Winner() { return wins[0] >= opt.wins ? 0 : 1; }

// Nome em Display na maior escala (até maxScale) que caiba em maxW; nas escalas 1 e 2, nome comprido pode quebrar no
// último espaço (line2). Devolve a escala.
static int FitName(std::string& line1, std::string& line2, int maxW, int maxScale) {
    line2.clear();
    for (int s = maxScale; s >= 1; s--) {
        if (MeasureDisplayText(line1.c_str(), s).x <= maxW) return s;
        size_t sp = line1.rfind(' ');
        if (s <= 2 && sp != std::string::npos) {
            std::string a = line1.substr(0, sp), b = line1.substr(sp + 1);
            if (std::max(MeasureDisplayText(a.c_str(), s).x, MeasureDisplayText(b.c_str(), s).x) <= maxW) {
                line1 = a;
                line2 = b;
                return s;
            }
        }
    }
    return 1;
}

// Tremida (px inteiros): forte logo depois do congelamento e uma curta quando cada letra do K.O. bate.
static Vector2 KoShake() {
    if (koT < 0.1f) return {0, 0};
    float a = koT < 0.45f ? 5 * (1 - (koT - 0.1f) / 0.35f) : 0;
    for (int g = 0; g < 3; g++) {
        float land = 0.1f + g * 0.08f + 0.15f;
        if (koT >= land && koT < land + 0.08f) a = std::max(a, 2.0f);
    }
    int ai = (int)a;
    return ai ? Vector2{(float)GetRandomValue(-ai, ai), (float)GetRandomValue(-ai, ai)} : Vector2{0, 0};
}

// Arena escurecida (azul-noite 55% + listras diagonais lentas); a partir do tempo 2 o vencedor volta à cor normal.
static void DrawKoArena(float alpha, float pixelsPerUnit, const Light& light) {
    if (koT < 0) return;
    float k = std::min(1.0f, koT / 0.15f);
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(Theme::Bg, 0.55f * k));
    int off = (int)(koT * 8) % 28;
    for (int i = -2; i < 18; i++)
        DrawLineEx({(float)(i * 28 + off), 0}, {(float)(i * 28 + off - 90), (float)SCREEN_H}, 6, Fade(WHITE, 0.035f * k));
    if (MatchOver() && koT >= 1.2f) {
        rlPushMatrix();
        rlTranslatef(-worldCam, roundf(camY * pixelsPerUnit) / pixelsPerUnit, 0);  // mesma câmera da luta
        DrawFighter(p[Winner()], false, alpha, pixelsPerUnit, light, false);
        rlPopMatrix();
    }
}

// "K.O.": K, O e os pontos caem um depois do outro (escala 2 -> 0,9 -> 1); no fim da luta encolhe para o topo.
static void DrawKoTitle(float t, bool shrink) {
    float e = shrink ? EaseOut((t - 1.2f) / 0.35f) : 0;
    bool rest = e >= 1;  // parado no topo: letras pequenas próprias (nítidas), não as grandes reduzidas
    float S = 1 + (1 / 3.0f - 1) * e, cy = 96 + (38 - 96) * e;
    auto drop = [&](float q, float& k, float& dy) {  // q: 0..1 da queda
        if (q < 0.6f) k = 2 + (0.9f - 2) * (q / 0.6f), dy = -40 * (1 - q / 0.6f);
        else k = 0.9f + 0.1f * std::min(1.0f, (q - 0.6f) / 0.4f), dy = 0;
    };
    if (koArt.id) {  // arte pronta (assets/ui/ko.png): entra inteira e no topo usa a versão pequena
        float q = (t - 0.1f) / 0.25f, k, dy;
        if (q <= 0) return;
        drop(q, k, dy);
        float sa = 1 + (KO_ART_SMALL - 1) * e, y = 96 + (41 - 96) * e;
        const Texture2D& a = rest && koArtSmall.id ? koArtSmall : koArt;
        float w = (rest ? a.width : koArt.width * sa) / PICTURE_RES * k, h = (rest ? a.height : koArt.height * sa) / PICTURE_RES * k;
        DrawTexturePro(a, {0, 0, (float)a.width, (float)a.height}, {roundf(SCREEN_W / 2.0f - w / 2), roundf(y + dy - h / 2), roundf(w), roundf(h)},
                       {0, 0}, 0, Fade(WHITE, std::min(1.0f, q * 4)));
        return;
    }
    const char* letters[4] = {"K", ".", "O", "."};
    const int group[4] = {0, 2, 1, 2};  // entram K, depois O, depois os pontos
    int sc = rest ? 2 : 6;
    float base = rest ? 1 : S, total = 0;
    for (const char* l : letters) total += MeasureDisplayText(l, sc).x;
    float x = SCREEN_W / 2.0f - total * base / 2;
    for (int i = 0; i < 4; i++) {
        Vector2 m = MeasureDisplayText(letters[i], sc);
        float adv = m.x * base, q = (t - 0.1f - group[i] * 0.08f) / 0.25f, k, dy;
        if (q > 0) {
            drop(q, k, dy);
            DrawDisplayText(letters[i], {roundf(x + adv / 2 - m.x / 2), roundf(cy + dy - m.y / 2)}, sc, Grad::Fire, true,
                            k * base, std::min(1.0f, q * 4));
        }
        x += adv;
    }
}

// Faixa a 12° pegando só as linhas [y, y+h) de uma faixa maior (Y, H, inclinação sl): mantém a mesma diagonal.
static void SubBand(float x0, float x1, float Y, float H, float sl, float y, float h, Color c) {
    float shift = sl * (Y + H - (y + h)) / H;
    Band(x0 + shift, x1 + shift, y, h, sl * h / H, c);
}

// Tempo 2: faixa do vencedor (cor da barra dele no HUD) com retrato, nome, "VENCEU!" e os rounds ganhos.
static void DrawWinnerBand(float t) {
    int w = Winner();
    const Fighter& f = p[w];
    float e = EaseOut((t - 1.2f) / 0.3f);
    Color team = w == 0 ? Color{255, 225, 40, 255} : Color{60, 225, 235, 255};
    const float Y = 52, H = 56, SL = roundf(H * TAN12), X0 = -80;
    float x1 = roundf(-40 + (SCREEN_W + 120) * e);
    Band(X0, x1 + 3, Y - 3, H + 6, roundf((H + 6) * TAN12), INK_DARK);
    Band(X0, x1, Y, H, SL, ColorLerp(Theme::Bg, team, 0.4f));
    SubBand(X0, x1, Y, H, SL, Y, 2, team);
    SubBand(X0, x1, Y, H, SL, Y + H - 2, 2, ColorLerp(team, BLACK, 0.4f));
    float sx = roundf(fmodf((t - 1.2f) * 170, SCREEN_W + 140) - 70);  // brilho percorrendo a faixa
    if (sx + 16 < x1) Band(sx, sx + 16, Y + 2, H - 4, roundf((H - 4) * TAN12), Fade(WHITE, 0.14f));

    int dx = (int)roundf(x1 - (SCREEN_W + 80));  // o conteúdo vem junto com a ponta da faixa
    HudIcon({(float)(42 + dx), Y + H / 2}, 22, f.def, false);
    // nome grande se couber antes do painel do menu (166 px); senão menor, e nome comprido quebra em duas linhas
    std::string name = Upper(f.def->name.c_str()), line2;
    int ns = FitName(name, line2, 166, 3), nx = 74 + dx, ny = (int)Y + (line2.empty() ? 6 : 4);
    int lh = (int)MeasureDisplayText("A", ns).y;
    DrawDisplayText(name.c_str(), {(float)nx, (float)ny}, ns, Grad::White, true);
    if (!line2.empty()) DrawDisplayText(line2.c_str(), {(float)nx, (float)(ny + lh + 3)}, ns, Grad::White, true);
    const char* won = "VENCEU!";
    int vs = std::max(1, ns - 1), vy = ny + (line2.empty() ? 1 : 2) * (lh + 3) + 2;
    Vector2 vm = MeasureDisplayText(won, vs);
    float ph = fmodf(t - 1.5f, 1.2f), pop = t > 1.5f && ph < 0.2f ? 0.15f * sinf(ph / 0.2f * PI) : 0;  // pulsar curto
    DrawDisplayText(won, {(float)nx, (float)vy}, vs, Grad::Gold, true, 1 + pop);
    if (opt.wins > 1)  // rounds ganhos: losangos acendendo um a um
        for (int k = 0; k < opt.wins; k++) {
            Vector2 c = {(float)(nx + (int)vm.x + 14 + k * 13), (float)(vy + (int)vm.y / 2)};
            bool lit = k < wins[w] && t >= 1.5f + k * 0.2f;
            DrawPoly(c, 4, 6, 0, INK_DARK);
            DrawPoly(c, 4, 4, 0, lit ? Theme::Gold : Color{50, 45, 75, 255});
            if (lit) DrawRectangle((int)c.x - 1, (int)c.y - 3, 1, 1, WHITE);
        }
}

// Ícones 8x8 dos itens do menu: setas circulares, duas silhuetas, casinha, porta.
static const char* const END_ICONS[MENU_N][8] = {
    {"..####..", ".#....##", "#....###", "#.......", ".......#", "###....#", "##....#.", "..####.."},
    {".##..##.", ".##..##.", "........", "###..###", "###..###", "###..###", "###..###", "........"},
    {"...##...", "..####..", ".######.", "########", ".#....#.", ".#.##.#.", ".#.##.#.", ".######."},
    {".######.", ".######.", ".######.", ".######.", ".####.#.", ".######.", ".######.", "########"},
};

static void DrawKey(int x, int y, const char* k) {  // tecla desenhada: caixinha com a letra
    DrawRectangle(x, y, 11, 11, INK_DARK);
    DrawRectangle(x + 1, y + 1, 9, 9, Theme::Border);
    DrawRectangle(x + 1, y + 9, 9, 1, Theme::Bg);
    DrawUIText(k, {(float)(x + 6 - MeasureUIText(k, 1) / 2), (float)(y + 2)}, 1, Theme::Ink);
}

// Tempo 3: painel inclinado com borda dupla, cabeçalho vermelho, itens em cartões e as teclas no rodapé.
static void DrawEndPanel(float t) {
    float e = EaseOut((t - 2.5f) / 0.3f), dt = GetFrameTime();
    const float W = 160, Y = 62, H = 136, R = SCREEN_W + 60;
    float x0 = roundf(SCREEN_W - W + (1 - e) * (W + 60));
    auto left = [&](float y) { return x0 + (Y + H - y) * TAN12; };  // borda esquerda do painel na altura y
    auto band = [&](float ya, float yb, float inset, float right, Color c) {
        Band(roundf(left(yb) + inset), right, ya, yb - ya, roundf((yb - ya) * TAN12), c);
    };
    band(Y - 3, Y + H + 3, -3, R, INK_DARK);  // borda de fora (escura)
    band(Y - 1, Y + H + 1, -1, R, Theme::Gold);  // de dentro (dourada fina)
    band(Y, Y + H, 0, R, Theme::Panel);

    const char* title = "FIM DE LUTA";
    int ts = MeasureDisplayText(title, 2).x <= W - 34 ? 2 : 1;
    float hy = Y + 6, hh = MeasureDisplayText(title, ts).y + 6;
    band(hy, hy + hh, 6, R, Theme::Red);
    DrawDisplayText(title, {roundf(left(hy + hh) + 14), hy + 3}, ts, Grad::White, false);

    float cy = hy + hh + 8;
    const float CH = 16;
    for (int i = 0; i < MENU_N; i++, cy += CH + 4) {
        bool sel = i == menuSel;
        menuShift[i] += ((sel ? 6.0f : 0.0f) - menuShift[i]) * std::min(1.0f, dt * 16);
        float cx = roundf(left(cy + CH) + 8 + menuShift[i]), right = SCREEN_W - 6, sl = roundf(CH * TAN12);
        Band(cx - 1, right + 1, cy - 1, CH + 2, sl, sel ? INK_DARK : Theme::Border);
        Band(cx, right, cy, CH, sl, sel ? Theme::Gold : Theme::Panel);
        if (sel) {
            float sx = roundf(cx + fmodf(t * 90, right - cx + 20) - 10);  // brilho percorrendo o cartão
            if (sx > cx && sx + 6 < right) Band(sx, sx + 6, cy, CH, sl, Fade(WHITE, 0.35f));
            if (fmodf(t, 0.5f) < 0.32f) {  // seta piscando à esquerda
                float ax = cx - 7, ay = cy + CH / 2;
                DrawTriangle({ax, ay - 4}, {ax, ay + 4}, {ax + 4, ay}, Theme::Gold);
            }
        }
        int ix = (int)cx + 6, iy = (int)cy + 4;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (END_ICONS[i][r][c] == '#') DrawRectangle(ix + c, iy + r, 1, 1, sel ? INK_DARK : Theme::Gold);
        DrawUIText(MENU_ITEMS[i], {(float)(ix + 12), (float)iy}, 1, sel ? INK_DARK : Theme::Ink);
    }

    int ky = (int)(Y + H - 17), kx = (int)roundf(left((float)(ky + 11)) + 12);
    DrawKey(kx, ky, "W");
    DrawKey(kx + 13, ky, "S");
    DrawUIText("MOVER", {(float)(kx + 29), (float)(ky + 2)}, 1, Theme::Ink);
    int jx = kx + 31 + MeasureUIText("MOVER", 1) + 10;
    DrawKey(jx, ky, "J");
    DrawUIText("CONFIRMAR", {(float)(jx + 16), (float)(ky + 2)}, 1, Theme::Ink);
}

static void TexturedQuad(const Texture2D& t, Rectangle art, const Vector2 p[4], Color tint, bool flip);

// HUD no estilo Marvel vs Capcom Infinite, espelhado: ícone grande do lutador no canto com o do suporte menor
// encaixado embaixo (acende quando dá para chamar e dá um pulinho ao ficar pronto), nome em Display em cima da barra
// inclinada (degradê; vermelha pulsando abaixo de 25%; flash de 2 quadros ao apanhar; o rastro vermelho do dano
// derrete em ~0,5 s), medidor do suporte embaixo (pisca ao chamar). No meio, a placa do tempo (luta sem tempo: ∞) e
// as medalhas de round embaixo dela.
const float HUD_M = 6, HUD_GAP = 4, PLATE_W = 30, PLATE_H = 22;  // margem, respiro, placa do tempo
static float hudPrevHp[2] = {-1, -1}, hudHit[2], hudHop[2] = {9, 9}, hudUse[2] = {9, 9}, medalT[2][3] = {{9, 9, 9}, {9, 9, 9}};
static bool hudReady[2];
static int hudPet[2] = {-1, -1}, hudWins[2];
static const char* const INFINITY_PX[5] = {".###...###.", "#...#.#...#", "#....#....#", "#...#.#...#", ".###...###."};
static const char* const MEDAL_PX[7] = {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."};

static void DrawBits(const char* const* rows, int n, int x, int y, int px, Color c) {  // desenho pixel em escala px
    for (int r = 0; r < n; r++)
        for (int k = 0; rows[r][k]; k++)
            if (rows[r][k] == '#') DrawRectangle(x + k * px, y + r * px, px, px, c);
}

static void DrawHud() {
    float dt = GetFrameTime(), now = (float)GetTime();
    rlDisableBackfaceCulling();
    // barra do P1: ponta de dentro cortada em diagonal; começa um pouco embaixo do retrato (o círculo cobre ~3 px)
    const float BX0 = 32, BX1 = 171, BY = 17, BH = 9, SL = 7;
    for (int i = 0; i < 2; i++) {
        const Fighter& f = p[i];
        auto X = [&](float x) { return i == 0 ? x : SCREEN_W - x; };
        auto bar = [&](float from, float to, float y, float h, Color a, Color b) {  // trecho [from, to] da barra (0..1)
            float x0 = BX0 + (BX1 - BX0) * from, x1 = BX0 + (BX1 - BX0) * to;
            Quad({X(x0), y}, {X(x1), y}, {X(std::max(BX0, x1 - SL * h / BH)), y + h}, {X(std::max(BX0, x0 - SL * h / BH)), y + h}, a, b);
        };
        if (hudPrevHp[i] >= 0 && f.hp < hudPrevHp[i]) hudHit[i] = 2 / 60.0f;  // apanhou: flash de 2 quadros
        hudPrevHp[i] = (float)f.hp;
        float hpR = (float)f.hp / f.def->health, trailR = trail[i] / f.def->health;
        Color outer = i == 0 ? Color{255, 225, 40, 255} : Color{40, 90, 255, 255};   // P1 amarelo→verde
        Color inner = i == 0 ? Color{110, 220, 60, 255} : Color{60, 225, 235, 255};   // P2 azul→ciano
        if (hpR < 0.25f) outer = inner = ColorLerp({200, 30, 40, 255}, {255, 95, 95, 255}, 0.5f + 0.5f * sinf(now * 8));
        bar(-0.02f, 1.02f, BY - 1.5f, BH + 3, BLACK, BLACK);
        bar(0, 1, BY, BH, {35, 25, 45, 235}, {35, 25, 45, 235});
        if (trailR > hpR) bar(hpR, trailR, BY, BH, {230, 40, 40, 255}, {230, 40, 40, 255});
        if (hpR > 0) {
            bar(0, hpR, BY, BH, outer, ColorLerp(outer, inner, hpR));
            bar(0, hpR, BY, 2, Fade(WHITE, 0.45f), Fade(WHITE, 0.45f));  // brilho em cima
            if (hudHit[i] > 0) bar(0, hpR, BY, BH, WHITE, WHITE);
        }
        hudHit[i] -= dt;
        const char* name = Upper(f.def->name.c_str());
        float nameW = MeasureDisplayText(name, 1).x;
        DrawDisplayText(name, {i == 0 ? BX0 + 6 : SCREEN_W - BX0 - 6 - nameW, 5}, 1, Grad::White, true);  // longe do círculo
        if (f.pets && f.petChoice >= 0) {
            // medidor do suporte embaixo da barra de vida, com a marca do custo; a vaga do suporte ao lado do retrato
            // acende (aro amarelo) quando dá para chamar; a tecla no fim do medidor
            const SupportSet& ss = *f.pets;
            float mx0 = BX0 + 9, mx1 = BX0 + 86, my = BY + BH + 4, mh = 5, m = f.meter / ss.maxMeter;  // ponta sob o ícone do suporte
            auto meter = [&](float from, float to, float y, float h, Color a, Color b) {
                float x0 = mx0 + (mx1 - mx0) * from, x1 = mx0 + (mx1 - mx0) * to;
                Quad({X(x0), y}, {X(x1), y}, {X(x1 - 3 * h / mh), y + h}, {X(x0 - 3 * h / mh), y + h}, a, b);
            };
            Color c0 = {40, 150, 255, 255}, c1 = {190, 255, 255, 255}, back = {25, 25, 45, 235};
            meter(-0.03f, 1.03f, my - 1, mh + 2, BLACK, BLACK);
            meter(0, 1, my, mh, back, back);
            if (m > 0) meter(0, m, my, mh, c0, ColorLerp(c0, c1, m));
            const SupportDef& sd = ss.list[f.petChoice];
            float cost = sd.cost / ss.maxMeter;
            meter(cost - 0.007f, cost + 0.007f, my, mh, Fade(WHITE, 0.8f), Fade(WHITE, 0.8f));
            bool lit = CanCallPet(f), here = f.pet.id >= 0;
            if (lit && !hudReady[i]) hudHop[i] = 0;  // ficou pronto: pulinho do ícone
            if (here && hudPet[i] < 0) hudUse[i] = 0;  // chamou: o medidor pisca
            hudReady[i] = lit;
            hudPet[i] = f.pet.id;
            if (hudUse[i] < 0.25f) meter(0, 1, my, mh, Fade(WHITE, 1 - hudUse[i] / 0.25f), Fade(WHITE, 1 - hudUse[i] / 0.25f));
            float hop = hudHop[i] < 0.3f ? roundf(3 * sinf(hudHop[i] / 0.3f * PI)) : 0;
            hudHop[i] += dt;
            hudUse[i] += dt;
            PetIcon({X(34), 34 - hop}, 7, sd, i == 1, lit || here, lit ? YELLOW : here ? WHITE : DARKGRAY);
            const char* key = i == 0 ? "1" : "7";
            OutlinedText(key, (int)X(mx1 + 8) - TextW(key, 10) / 2, (int)my - 2, 10, lit ? YELLOW : GRAY, 1);
        } else {
            HudIcon({X(34), 34}, 7, nullptr, i == 1);  // sem suporte
        }
        if (f.guard > 0) {  // barra de guarda (só com algo nela): curta, depois da tecla do suporte; quase cheia, pisca
            float gx0 = BX0 + 102, gx1 = BX0 + 138, gy = BY + BH + 4, gh = 5, g = std::min(1.0f, f.guard / 100);
            auto gbar = [&](float from, float to, float y, float h, Color a, Color b) {
                float x0 = gx0 + (gx1 - gx0) * from, x1 = gx0 + (gx1 - gx0) * to;
                Quad({X(x0), y}, {X(x1), y}, {X(x1 - 3 * h / gh), y + h}, {X(x0 - 3 * h / gh), y + h}, a, b);
            };
            Color g0 = {255, 200, 60, 255}, g1 = g > 0.75f && fmodf(now * 8, 2) < 1 ? WHITE : Color{255, 60, 40, 255};
            gbar(-0.05f, 1.05f, gy - 1, gh + 2, BLACK, BLACK);
            gbar(0, 1, gy, gh, {25, 25, 45, 235}, {25, 25, 45, 235});
            gbar(0, g, gy, gh, g0, ColorLerp(g0, g1, g));
        }
        if (f.def->spray.on) {  // tanque do borrifo (Thays): fino, rosa, embaixo do medidor; travado (esvaziou) = escuro piscando
            float sx0 = BX0 + 9, sx1 = BX0 + 60, sy = BY + BH + 11, sh = 3, v = f.spray / 100;
            auto sbar = [&](float from, float to, float y, float h, Color c) {
                float x0 = sx0 + (sx1 - sx0) * from, x1 = sx0 + (sx1 - sx0) * to;
                Quad({X(x0), y}, {X(x1), y}, {X(x1 - 2 * h / sh), y + h}, {X(x0 - 2 * h / sh), y + h}, c, c);
            };
            Color pink = f.sprayLock ? (fmodf(now * 4, 2) < 1 ? Color{120, 50, 90, 255} : Color{80, 30, 60, 255}) : Color{255, 120, 200, 255};
            sbar(-0.03f, 1.03f, sy - 1, sh + 2, BLACK);
            sbar(0, 1, sy, sh, {25, 25, 45, 235});
            if (v > 0) sbar(0, v, sy, sh, pink);
        }
        if (f.warnT >= 0) {  // aviso: "REPETIDO -40%" (dano menor) ou "GUARDA QUEBRADA!", sobe 2 px e some
            const char* t = f.warnKind == 2 ? "GUARDA QUEBRADA!" : TextFormat("REPETIDO -%d%%", f.warnPct);
            float a = f.warnT < 55 ? 1 : (75 - f.warnT) / 20.0f, w = (float)MeasureUIText(t, 1);
            Color c = f.warnKind == 2 ? Color{255, 80, 70, 255} : Color{255, 190, 70, 255};
            float y = BY + BH + 12 + (f.def->spray.on ? 4 : 0) - std::min(2.0f, f.warnT / 6.0f);
            DrawUIText(t, {roundf(i == 0 ? BX0 + 9 : SCREEN_W - BX0 - 9 - w), roundf(y)}, 1, Fade(c, a), true);
        }
        HudIcon({X(19), 19}, 14, f.def, i == 1);   // lutador
    }

    // placa do tempo: hexágono inclinado, borda escura por fora e dourada fina por dentro; ∞ pequeno (sem tempo)
    float cx = SCREEN_W / 2.0f, cy = HUD_M + PLATE_H / 2;
    auto hexa = [&](float grow, Color c) {
        float w = PLATE_W / 2 + grow, h = PLATE_H / 2 + grow, s = h * TAN12, k = w - 5;
        Vector2 v[6] = {{cx - w, cy}, {cx - k + s, cy - h}, {cx + k + s, cy - h}, {cx + w, cy}, {cx + k - s, cy + h}, {cx - k - s, cy + h}};
        for (int j = 0; j < 6; j++) DrawTriangle({cx, cy}, v[(j + 1) % 6], v[j], c);
    };
    hexa(2, INK_DARK);
    hexa(1, Theme::Gold);
    hexa(0, Theme::Panel);
    int ix = (int)cx - 11, iy = (int)cy - 5;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) DrawBits(INFINITY_PX, 5, ix + dx, iy + dy, 2, INK_DARK);
    DrawBits(INFINITY_PX, 5, ix, iy, 2, WHITE);

    // medalhas de round embaixo da placa: P1 à esquerda, P2 à direita; ganhas douradas, piscam ao acender
    for (int i = 0; i < 2; i++) {
        if (wins[i] != hudWins[i]) {
            if (wins[i] > hudWins[i] && wins[i] <= 3) medalT[i][wins[i] - 1] = 0;
            hudWins[i] = wins[i];
        }
        for (int k = 0; k < opt.wins && k < 3; k++) {
            int mx = (int)(i == 0 ? cx - 6 - 7 - k * 10 : cx + 6 + k * 10), my = (int)(HUD_M + PLATE_H + HUD_GAP);
            bool won = k < wins[i];
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    if (dx || dy) DrawBits(MEDAL_PX, 7, mx + dx, my + dy, 1, INK_DARK);
            Color c = won ? Theme::Gold : Theme::Panel;
            if (won && medalT[i][k] < 0.3f && fmodf(medalT[i][k], 0.1f) < 0.05f) c = WHITE;  // piscada ao acender
            DrawBits(MEDAL_PX, 7, mx, my, 1, c);
            if (won) DrawRectangle(mx + 2, my + 1, 2, 1, Fade(WHITE, 0.8f)), DrawRectangle(mx + 1, my + 2, 1, 1, Fade(WHITE, 0.8f));
            medalT[i][k] += dt;
        }
    }
    rlDrawRenderBatchActive();  // desenha agora, ainda sem culling
    rlEnableBackfaceCulling();
    auto big = [](const char* t, int y, int scale, const Gradient& g) {
        Vector2 m = MeasureDisplayText(t, scale);
        DrawDisplayText(t, {roundf(SCREEN_W / 2.0f - m.x / 2), (float)y}, scale, g, true);
    };
    if (koT >= 0) {
        bool end = MatchOver();
        if (end && koT >= 1.2f) DrawWinnerBand(koT);
        DrawKoTitle(koT, end);
        if (end && koT >= 2.5f) DrawEndPanel(koT);
    } else if (roundTicks < INTRO_TICKS) {
        bool last = opt.wins > 1 && wins[0] == opt.wins - 1 && wins[1] == opt.wins - 1;
        big(last ? "ROUND FINAL" : TextFormat("ROUND %d", roundNum), 80, 3, Grad::White);
    } else if (roundTicks < INTRO_TICKS + 30) {
        big("LUTE!", 80, 3, Grad::Fire);
    }
}

static void TexturedQuad(const Texture2D& t, Rectangle art, const Vector2 p[4], Color tint, bool flip);
static void OpenVersus();
static void UpdatePetSelect();
static void DrawSelectHeader(const char* title);

// ---------- tela de escolha ----------
// Lados: retrato (busto) do personagem sob o cursor num painel inclinado de borda dupla e, embaixo, a placa com nome,
// estilo, FORÇA/VELOCIDADE/ALCANCE (segmentos) e o especial com o comando (P2 espelhado). Centro: grade 4xN de
// cartões a 8°, o último é "?" (aleatório); vaga sem personagem = silhueta com cadeado ("EM BREVE" no cursor).
// Entrada: painéis deslizam das bordas e os cartões surgem em cascata. Arena: só o visual (o jogo tem uma).
const int GRID_COLS = 4, CARD_PX = 28, CARD_GAP = 3, RANDOM_CARD = -2;
const float TAN8 = 0.1405f;
const float PANEL_X0 = 10, PANEL_X1 = 122, PANEL_Y0 = 34, PANEL_Y1 = 190, PANEL_SL = 14, PLATE_Y = 104;
static const Color P_COLOR[2] = {{240, 60, 70, 255}, {70, 150, 255, 255}};
static const char* const STAT_NAMES[3] = {"FORÇA", "VELOCIDADE", "ALCANCE"};

static int GridCards() { return std::max(8, ((int)roster.chars.size() + GRID_COLS) / GRID_COLS * GRID_COLS); }
static int CardChar(int k) { return k == GridCards() - 1 ? RANDOM_CARD : k < (int)roster.chars.size() ? k : -1; }
static const char* PlayerTag(int i) { return i == 0 ? "1P" : cpu ? "CPU" : "2P"; }

// P1: A/D/W/S + J confirma, K desfaz. P2: setas + Numpad 1, Numpad 2 desfaz. Com a CPU ligada, o P1 escolhe o
// próprio lutador e depois o da CPU, com as mesmas teclas (K desfaz o da CPU, depois o próprio). "?" sorteia.
// Os dois confirmados: suportes (ou a tela de arena, sem mascotes).
static void UpdateSelect() {
    struct Keys { int left, right, up, down, ok, back; };
    const Keys keys[2] = {{KEY_A, KEY_D, KEY_W, KEY_S, KEY_J, KEY_K},
                          {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_KP_1, KEY_KP_2}};
    if (KeyBack()) { GoTo([] { screen = Screen::Title; }); return; }
    int nPets = (int)roster.supports.list.size(), nChars = (int)roster.chars.size(), rows = GridCards() / GRID_COLS;
    int turn = pick[0] < 0 ? 0 : 1;  // CPU: de quem é a vez (antes do loop: o J que confirmou não vale para a CPU)
    for (int i = 0; i < 2; i++) {
        int ctl = cpu ? 0 : i;  // teclas/controle de quem comanda este lado
        const Keys& k = keys[ctl];
        if (cpu && i != turn) continue;
        if (pick[i] >= 0) {
            if (Press(ctl, k.back, PAD_BACK) && readyAt == 0) pick[i] = -1;
            continue;
        }
        if (Press(ctl, k.back, PAD_BACK)) {
            if (i == 0) { GoTo([] { screen = Screen::Title; }); return; }  // nada escolhido: volta
            if (cpu) { pick[0] = -1; continue; }  // escolhendo a CPU: desfaz o próprio
        }
        int c = cursor[i] % GRID_COLS, r = cursor[i] / GRID_COLS;
        if (Press(ctl, k.left, PAD_LEFT)) c = (c + GRID_COLS - 1) % GRID_COLS;
        if (Press(ctl, k.right, PAD_RIGHT)) c = (c + 1) % GRID_COLS;
        if (Press(ctl, k.up, PAD_UP)) r = (r + rows - 1) % rows;
        if (Press(ctl, k.down, PAD_DOWN)) r = (r + 1) % rows;
        if (r * GRID_COLS + c != cursor[i]) PlaySfx("cursor");
        cursor[i] = r * GRID_COLS + c;
        int who = CardChar(cursor[i]);
        if (Press(ctl, k.ok, PAD_OK) && who != -1) {  // vaga "EM BREVE" não confirma
            PlaySfx("confirmar");
            pick[i] = who == RANDOM_CARD ? GetRandomValue(0, nChars - 1) : who;
            pickedAt[i] = GetTime();
        }
    }
    if (cpu && pick[0] < 0) pick[1] = -1;  // P1 desfez o próprio: escolhe a CPU de novo depois
    if (pick[0] < 0 || pick[1] < 0) return;
    if (readyAt == 0) readyAt = GetTime() + (nPets ? 0.6 : 1.0);
    else if (GetTime() >= readyAt) {
        readyAt = 1e18;  // a transição já foi pedida
        if (nPets) GoTo([] { selectStage = 1; petPick[0] = petPick[1] = -1; readyAt = 0; selectAt = GetTime(); });
        else GoTo(OpenStageSelect);
    }
}

// Vinheta escura nas bordas e scanlines sutis (tela de loading e telas de escolha).
static void DrawVignette(bool scan) {
    Color v0 = Fade(BLACK, 0.55f), v1 = Fade(BLACK, 0);
    DrawRectangleGradientH(0, 0, 48, SCREEN_H, v0, v1);
    DrawRectangleGradientH(SCREEN_W - 48, 0, 48, SCREEN_H, v1, v0);
    DrawRectangleGradientV(0, 0, SCREEN_W, 28, v0, v1);
    DrawRectangleGradientV(0, SCREEN_H - 28, SCREEN_W, 28, v1, v0);
    if (scan)
        for (int y = 0; y < SCREEN_H; y += 2) DrawRectangle(0, y, SCREEN_W, 1, Fade(BLACK, 0.08f));
}

// Painel lateral do jogador i (tl, tr, br, bl), crescido `grow` px para os lados e deslocado `dx`; o do P2 é o do P1
// espelhado (mesma ordem de cantos, então o desenho não vira do avesso).
static void SidePanel(int i, float dx, float grow, Vector2 q[4]) {
    float x0 = PANEL_X0 - grow, x1 = PANEL_X1 + grow, y0 = PANEL_Y0 - grow, y1 = PANEL_Y1 + grow;
    Vector2 p[4] = {{x0 + PANEL_SL, y0}, {x1 + PANEL_SL, y0}, {x1, y1}, {x0, y1}};
    for (int k = 0; k < 4; k++) q[k] = {p[k].x + dx, p[k].y};
    if (i == 1) {
        Vector2 m[4] = {{SCREEN_W - p[1].x - dx, y0}, {SCREEN_W - p[0].x - dx, y0}, {SCREEN_W - p[3].x - dx, y1},
                        {SCREEN_W - p[2].x - dx, y1}};
        std::copy(m, m + 4, q);
    }
}

// x das bordas esquerda/direita do quadrilátero q na altura y
static float EdgeX(const Vector2 q[4], float y, bool right) {
    const Vector2 &a = right ? q[1] : q[0], &b = right ? q[2] : q[3];
    return a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y);
}

static void FillQuad(const Vector2 q[4], Color c) {
    rlDisableBackfaceCulling();
    Quad(q[0], q[1], q[2], q[3], c, c);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
}

// Ícones de 7x7 do comando do especial.
static const char* const ARROW_DOWN[7] = {"..###..", "..###..", "..###..", "#######", ".#####.", "..###..", "...#..."};
static const char* const ARROW_RIGHT[7] = {"...#...", "...##..", "######.", "#######", "######.", "...##..", "...#..."};
static void DrawIcon7(const char* const rows[7], int x, int y, Color c, bool flip) {
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 7; k++)
            if (rows[r][flip ? 6 - k : k] == '#') DrawRectangle(x + k, y + r, 1, 1, c);
}

// Arte grande do personagem dentro do quadrilátero q: busto (retrato) ou, sem ele, o idle ampliado 2x.
// y = topo da arte; fromBottom: y = base da arte (VS: o busto encosta embaixo da tela)
static void DrawFighterArt(const CharDef& d, const Vector2 q[4], float cx, float y, float zoom, bool flip, Color tint,
                           bool fromBottom = false) {
    const Texture2D* t = d.portrait.id ? &d.portrait : nullptr;
    float k = 1.0f / PICTURE_RES;
    if (!t) {
        auto a = d.anims.find("idle");
        if (a == d.anims.end() || a->second.frames.empty()) a = d.anims.find("walk");
        if (a == d.anims.end() || a->second.frames.empty()) return;
        t = &a->second.frames[0];
        k = 2;
    }
    float w = roundf(t->width * k) * zoom, h = roundf(t->height * k) * zoom;
    TexturedQuad(*t, {roundf(cx - w / 2), roundf(fromBottom ? y - h : y), w, h}, q, tint, flip);
}

// Cartão da grade: ícone quadrado ou, sem ele, o rosto recortado do sprite parado.
static void DrawCardArt(const CharDef& d, const Vector2 q[4], Rectangle box, Color tint) {
    if (d.card.id) { TexturedQuad(d.card, box, q, tint, false); return; }
    auto a = d.anims.find("idle");
    if (a == d.anims.end() || a->second.frames.empty()) a = d.anims.find("walk");
    if (a == d.anims.end() || a->second.frames.empty()) return;
    const Texture2D& t = a->second.frames[0];
    float w = box.width * 1.6f, h = w * t.height / t.width;  // ampliado e alinhado pelo topo: só a cabeça aparece
    TexturedQuad(t, {roundf(box.x + box.width / 2 - w / 2), box.y + 1, w, h}, q, tint, false);
}

static void DrawSilhouette(float cx, float top, float s, Color c) {  // vaga sem personagem
    DrawCircle((int)cx, (int)(top + 7 * s), 6 * s, c);
    DrawRectangleRounded({cx - 10 * s, top + 14 * s, 20 * s, 16 * s}, 0.5f, 4, c);
}

static const char* const LOCK[8] = {"..###...", ".#...#..", ".#...#..", "#######.", "###.###.", "###.###.", "#######.", "........"};

static void DrawStatBar(int x, int y, int value) {  // 5 segmentos de 4 px, estilo barra de energia
    DrawRectangle(x - 1, y - 1, 5 * 5 + 1, 6, INK_DARK);
    for (int s = 0; s < 5; s++)
        DrawRectangle(x + s * 5, y, 4, 4, s < value ? ColorLerp(Theme::Red, Theme::Gold, s / 4.0f) : Theme::Panel);
}

static void DrawSelectSide(int i, float t) {
    float slide = (1 - EaseOut(t / 0.35f)) * 150 * (i == 0 ? -1 : 1);
    Vector2 q[4], b1[4], b2[4];
    SidePanel(i, slide, 3, b1);
    SidePanel(i, slide, 1, b2);
    SidePanel(i, slide, 0, q);
    FillQuad(b1, INK_DARK);  // borda dupla: escura por fora, cor do jogador por dentro
    FillQuad(b2, P_COLOR[i]);
    FillQuad(q, ColorLerp(Theme::Bg, P_COLOR[i], 0.25f));
    int who = pick[i] >= 0 ? pick[i] : CardChar(cursor[i]);
    float cx = (EdgeX(q, PANEL_Y0 + 30, false) + EdgeX(q, PANEL_Y0 + 30, true)) / 2;
    if (who >= 0) {
        const CharDef& d = roster.chars[who];
        // retrato pela metade (as cabeças continuam do mesmo tamanho entre si) para o busto inteiro caber acima da
        // placa: encostado nela, sem passar do topo do painel. Sem retrato: o idle ampliado, como antes.
        float zoom = d.portrait.id ? 0.5f : 1, artH = roundf(d.portrait.height / (float)PICTURE_RES) * zoom;
        float top = d.portrait.id ? std::max(PANEL_Y0 + 4, PLATE_Y + 4 - artH) : PANEL_Y0 + 4;
        DrawFighterArt(d, q, cx, top, zoom, i == 1, WHITE);
        float k = (float)(GetTime() - pickedAt[i]) / 0.25f;  // confirmou: o retrato pisca em branco
        if (pick[i] >= 0 && k < 1) {
            BeginBlendMode(BLEND_ADDITIVE);
            DrawFighterArt(d, q, cx, top, zoom, i == 1, Fade(WHITE, 1 - k));
            EndBlendMode();
        }
    } else if (who == RANDOM_CARD) {
        Vector2 m = MeasureDisplayText("?", 6);
        DrawDisplayText("?", {roundf(cx - m.x / 2), PANEL_Y0 + 14}, 6, Grad::Fire, true);
    } else {
        DrawSilhouette(cx, PANEL_Y0 + 12, 2.5f, {10, 8, 18, 230});
    }

    // placa: nome, estilo, atributos e o especial com o comando (P2 alinhado à direita)
    Vector2 plate[4] = {{EdgeX(q, PLATE_Y, false), PLATE_Y}, {EdgeX(q, PLATE_Y, true), PLATE_Y}, q[2], q[3]};
    FillQuad(plate, Fade(Theme::Bg, 0.92f));
    Vector2 stripe[4] = {plate[0], plate[1], {EdgeX(q, PLATE_Y + 2, true), PLATE_Y + 2}, {EdgeX(q, PLATE_Y + 2, false), PLATE_Y + 2}};
    FillQuad(stripe, P_COLOR[i]);  // filete na cor do jogador em cima da placa
    auto at = [&](float y, float w) {  // x de um bloco de largura w na linha y
        return roundf(i == 0 ? EdgeX(q, y + 8, false) + 6 : EdgeX(q, y + 8, true) - 6 - w);
    };
    if (who >= 0) {
        const CharDef& d = roster.chars[who];
        std::string name = Upper(d.name.c_str()), line2;
        float maxW = EdgeX(q, PLATE_Y, true) - EdgeX(q, PLATE_Y, false) - 14;
        int ns = FitName(name, line2, (int)maxW, 2);
        float y = PLATE_Y + 5, lh = MeasureDisplayText("A", ns).y;
        DrawDisplayText(name.c_str(), {at(y, MeasureDisplayText(name.c_str(), ns).x), y}, ns, Grad::White, true);
        if (!line2.empty()) {
            y += lh + 2;
            DrawDisplayText(line2.c_str(), {at(y, MeasureDisplayText(line2.c_str(), ns).x), y}, ns, Grad::White, true);
        }
        y = PLATE_Y + 24;
        const char* style = d.style.empty() ? "LUTADOR" : d.style.c_str();
        DrawUIText(style, {at(y, (float)MeasureUIText(style, 1)), y}, 1, Theme::InkDim);
        int labelW = 0;
        for (const char* s : STAT_NAMES) labelW = std::max(labelW, MeasureUIText(s, 1));
        for (int k = 0; k < 3; k++) {
            y = PLATE_Y + 34 + k * 9.0f;
            float x = at(y, (float)(labelW + 4 + 25));
            DrawUIText(STAT_NAMES[k], {x, y}, 1, Theme::Ink);
            DrawStatBar((int)x + labelW + 4, (int)y + 2, d.stats[k]);
        }
        y = PLATE_Y + 62;
        const char* sp = d.specialName.empty() ? (d.hasSpecial ? "ESPECIAL" : "PARRY") : d.specialName.c_str();
        if (d.hasSpecial || d.parry.on) {
            DrawUIText(sp, {at(y, (float)MeasureUIText(sp, 1)), y}, 1, Theme::Gold);
            y += 10;  // comando: ↓ e → (especial) ou ← (parry), + soco
            const char* key = i == 0 ? "J" : "1";
            float w = 7 + 2 + 7 + 3 + (float)MeasureUIText("+", 1) + 3 + 11, x = at(y, w);
            DrawIcon7(ARROW_DOWN, (int)x, (int)y + 2, Theme::Ink, false);
            DrawIcon7(ARROW_RIGHT, (int)x + 9, (int)y + 2, Theme::Ink, !d.hasSpecial);
            DrawUIText("+", {x + 19, y + 1}, 1, Theme::Ink);
            DrawKey((int)(x + 22 + MeasureUIText("+", 1)), (int)y, key);
        }
    } else {
        const char* t1 = who == RANDOM_CARD ? "ALEATÓRIO" : "EM BREVE";
        float y = PLATE_Y + 30;
        DrawDisplayText(t1, {at(y, MeasureDisplayText(t1, 1).x), y}, 1, Grad::White, true);
    }
    if (pick[i] >= 0) {  // PRONTO! sobre a placa
        Vector2 m = MeasureDisplayText("PRONTO!", 2);
        float px = roundf((EdgeX(q, PLATE_Y + 40, false) + EdgeX(q, PLATE_Y + 40, true)) / 2 - m.x / 2);
        Band(px - 8, px + m.x + 6, PLATE_Y + 31, m.y + 10, 3, Fade(INK_DARK, 0.85f));
        DrawDisplayText("PRONTO!", {px, PLATE_Y + 36}, 2, Grad::Gold, true);
    }
}

// Fundo das telas de escolha (SF3 espelhado): degradê, raios diagonais abrindo do centro (andando devagar), brilho
// no meio, vinheta e scanlines.
static void DrawSelectBackground(Color top, Color bottom, Color glow, float glowY) {
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, top, bottom);
    float off = fmodf((float)GetTime() * 6, 26);
    for (int i = -1; i < 9; i++) {
        float x = SCREEN_W / 2 - 10 - i * 26.0f - off;
        DrawLineEx({x, 0}, {x - 90, (float)SCREEN_H}, 7, Fade(BLACK, 0.13f));
        DrawLineEx({SCREEN_W - x, 0}, {SCREEN_W - x + 90, (float)SCREEN_H}, 7, Fade(BLACK, 0.13f));
    }
    DrawCircleGradient(SCREEN_W / 2, (int)glowY, 95, glow, Fade(glow, 0));
    DrawVignette(true);
}

static void DrawSelect() {
    DrawSelectBackground({60, 6, 18, 255}, {220, 100, 30, 255}, {255, 210, 90, 150}, 100);
    float t = (float)(GetTime() - selectAt), now = (float)GetTime();

    DrawSelectHeader(cpu && pick[0] >= 0 && pick[1] < 0 ? "ESCOLHA O RIVAL" : "ESCOLHA SEU LUTADOR");

    for (int i = 0; i < 2; i++) DrawSelectSide(i, t);

    // grade de cartões a 8° (cascata na entrada)
    int n = GridCards(), rows = n / GRID_COLS;
    float sl = roundf(CARD_PX * TAN8), gw = GRID_COLS * CARD_PX + (GRID_COLS - 1) * CARD_GAP + sl;
    float gx = roundf(SCREEN_W / 2.0f - gw / 2), gy = roundf(110 - (rows * (CARD_PX + CARD_GAP) - CARD_GAP) / 2.0f);
    auto cardQuad = [&](int k, float grow, Vector2 q[4]) {
        float x = gx + (k % GRID_COLS) * (CARD_PX + CARD_GAP), y = gy + (k / GRID_COLS) * (CARD_PX + CARD_GAP);
        q[0] = {x + sl - grow, y - grow}, q[1] = {x + sl + CARD_PX + grow, y - grow};
        q[2] = {x + CARD_PX + grow, y + CARD_PX + grow}, q[3] = {x - grow, y + CARD_PX + grow};
    };
    for (int k = 0; k < n; k++) {
        float a = std::clamp((t - 0.15f - k * 0.04f) / 0.15f, 0.0f, 1.0f);
        if (a <= 0) continue;
        Vector2 q[4], b[4];
        cardQuad(k, 1, b);
        cardQuad(k, 0, q);
        float dy = roundf((1 - a) * -6);
        for (int j = 0; j < 4; j++) q[j].y += dy, b[j].y += dy;
        FillQuad(b, Fade(INK_DARK, a));
        int who = CardChar(k);
        FillQuad(q, Fade(who == -1 ? Color{30, 22, 38, 255} : Theme::Panel, a));
        Rectangle box = {q[3].x + sl / 2, q[0].y, (float)CARD_PX, (float)CARD_PX};
        if (who >= 0) {
            DrawCardArt(roster.chars[who], q, box, Fade(WHITE, a));
        } else if (who == RANDOM_CARD) {
            Vector2 m = MeasureDisplayText("?", 2);
            DrawDisplayText("?", {roundf(box.x + CARD_PX / 2 - m.x / 2), roundf(box.y + CARD_PX / 2 - m.y / 2)}, 2,
                            Grad::Gold, false, 1, a);
        } else {
            DrawSilhouette(box.x + CARD_PX / 2, box.y + 5, 0.75f, Fade({10, 8, 18, 255}, a));
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++)
                    if (LOCK[r][c] == '#') DrawRectangle((int)box.x + 18 + c, (int)box.y + 18 + r, 1, 1, Fade(Theme::InkDim, a));
        }
    }

    // cursores: moldura pulsando (vermelho 1P, azul 2P/CPU; no mesmo cartão, dividida); etiqueta em cima
    if (t > 0.3f) {
        for (int i = 0; i < 2; i++) {
            Vector2 q[4];
            cardQuad(cursor[i], 2, q);
            bool same = cursor[0] == cursor[1];
            Color c = P_COLOR[i];
            if (pick[i] < 0) c = ColorLerp(c, WHITE, 0.35f + 0.35f * sinf(now * 10));
            for (int e = 0; e < 4; e++) {
                if (same && (i == 0) != (e == 0 || e == 3)) continue;  // dividido: 1P em cima/esquerda, 2P embaixo/direita
                DrawLineEx(q[e], q[(e + 1) % 4], 2, c);
            }
            const char* tag = PlayerTag(i);
            int tw = MeasureUIText(tag, 1);
            float lx = same ? (i == 0 ? q[0].x - 2 : q[1].x - tw + 2) : (q[0].x + q[1].x) / 2 - tw / 2.0f;
            DrawUIText(tag, {roundf(lx), q[0].y - 10}, 1, P_COLOR[i], true);
        }
        for (int i = 0; i < 2; i++)  // cursor em vaga vazia: "EM BREVE" embaixo da grade
            if (pick[i] < 0 && CardChar(cursor[i]) == -1) {
                int w = MeasureUIText("EM BREVE", 1);
                DrawUIText("EM BREVE", {roundf(SCREEN_W / 2.0f - w / 2), gy + rows * (CARD_PX + CARD_GAP) + 3}, 1,
                           Theme::Ink, true);
                break;
            }
    }

    // rodapé: teclas (a arena é escolhida na sua própria tela, depois do suporte)
    float fy = 196;
    int kx = 10;
    for (const char* k : {"W", "A", "S", "D"}) DrawKey(kx, (int)fy - 1, k), kx += 12;
    DrawUIText("MOVER", {(float)kx + 2, fy + 1}, 1, Theme::Ink, true);
    int rx = SCREEN_W - 10 - (12 + MeasureUIText("CONFIRMA", 1) + 8 + 12 + MeasureUIText("VOLTA", 1));
    DrawKey(rx, (int)fy - 1, "J");
    DrawUIText("CONFIRMA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
    rx += 13 + MeasureUIText("CONFIRMA", 1) + 7;
    DrawKey(rx, (int)fy - 1, "K");
    DrawUIText("VOLTA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
}

// Faixa inclinada (parallelogramo) para nomes e títulos; slant > 0 inclina como "/", < 0 como "\".
static void Band(float x0, float x1, float y, float h, float slant, Color c) {
    Vector2 tl = {x0 + slant, y}, tr = {x1 + slant, y}, bl = {x0, y + h}, br = {x1, y + h};
    DrawTriangle(tl, bl, br, c);
    DrawTriangle(tl, br, tr, c);
}

static void OutlinedText(const char* t, int x, int y, int size, Color fill, int border) {
    for (int dx = -border; dx <= border; dx += border)
        for (int dy = -border; dy <= border; dy += border) Text(t, x + dx, y + dy, size, BLACK);
    Text(t, x, y, size, fill);
}

// ---------- tela VS ----------
// 2 s depois de tudo escolhido: tela dividida na diagonal (1P vermelho à esquerda, 2P azul à direita) com raios
// andando, retratos entrando pelas bordas com leve zoom, nomes na base, "VS" batendo (2 -> 0,9 -> 1) com flash e
// tremida, e a arena embaixo. Depois a cortina leva direto à luta.
static double vsAt = 0;
const float VS_HIT = 0.55f, VS_TIME = 2.0f, VS_D = 30;  // quando o VS bate; duração; meia inclinação da diagonal

static void OpenVersus() {
    screen = Screen::Versus;
    vsAt = GetTime();
}

static void UpdateVersus() {
    if (GetTime() - vsAt >= VS_TIME) GoTo(StartMatch);
}

static Vector2 VsShake() {
    float t = (float)(GetTime() - vsAt) - VS_HIT;
    int a = t >= 0 && t < 0.25f ? 1 + (int)(4 * (1 - t / 0.25f)) : 0;
    return a ? Vector2{(float)GetRandomValue(-a, a), (float)GetRandomValue(-a, a)} : Vector2{0, 0};
}

static void DrawVersus() {
    float t = (float)(GetTime() - vsAt);
    const float MX = SCREEN_W / 2.0f;
    Vector2 half[2][4] = {{{-10, 0}, {MX + VS_D, 0}, {MX - VS_D, SCREEN_H}, {-10, SCREEN_H}},
                          {{MX + VS_D, 0}, {SCREEN_W + 10, 0}, {SCREEN_W + 10, SCREEN_H}, {MX - VS_D, SCREEN_H}}};
    rlDisableBackfaceCulling();
    Quad(half[0][0], half[0][1], half[0][2], half[0][3], ColorLerp(Theme::Bg, P_COLOR[0], 0.3f), ColorLerp(Theme::Bg, P_COLOR[0], 0.7f));
    Quad(half[1][0], half[1][1], half[1][2], half[1][3], ColorLerp(Theme::Bg, P_COLOR[1], 0.7f), ColorLerp(Theme::Bg, P_COLOR[1], 0.3f));
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    for (int k = 0; k < 8; k++) {  // raios paralelos à diagonal, andando para fora
        float d = fmodf(t * 70 + k * 26, 210);
        DrawLineEx({MX + VS_D - 8 - d, 0}, {MX - VS_D - 8 - d, (float)SCREEN_H}, 5, Fade(WHITE, 0.07f));
        DrawLineEx({MX + VS_D + 8 + d, 0}, {MX - VS_D + 8 + d, (float)SCREEN_H}, 5, Fade(WHITE, 0.07f));
    }

    for (int i = 0; i < 2; i++) {  // retratos entram pelas bordas com leve zoom, cada um preso à sua metade
        const CharDef& d = roster.chars[pick[i]];
        float e = EaseOut(t / 0.4f), zoom = 1.12f - 0.12f * e, slide = (1 - e) * 220 * (i == 0 ? -1 : 1);
        DrawFighterArt(d, half[i], (i == 0 ? 96 : SCREEN_W - 96) + slide, SCREEN_H + 4, zoom, i == 1, WHITE, true);
    }
    DrawLineEx({MX + VS_D, 0}, {MX - VS_D, (float)SCREEN_H}, 7, INK_DARK);  // divisória
    DrawLineEx({MX + VS_D, 0}, {MX - VS_D, (float)SCREEN_H}, 2, Theme::Gold);
    DrawVignette(true);

    for (int i = 0; i < 2; i++) {  // nomes na base, subindo depois dos retratos
        float e = EaseOut((t - 0.25f) / 0.3f);
        if (e <= 0) continue;
        std::string name = Upper(roster.chars[pick[i]].name.c_str()), line2;
        int ns = FitName(name, line2, 168, 3);
        float lh = MeasureDisplayText("A", ns).y, y = roundf(SCREEN_H - 12 - lh * (line2.empty() ? 1 : 2) - 3 + (1 - e) * 40);
        const char* tag = PlayerTag(i);
        float tagW = MeasureDisplayText(tag, 1).x;
        DrawDisplayText(tag, {i == 0 ? 12 : SCREEN_W - 12 - tagW, y - 12}, 1, i == 0 ? Grad::P1 : Grad::P2, true);
        for (const std::string& l : {name, line2}) {
            if (l.empty()) continue;
            float w = MeasureDisplayText(l.c_str(), ns).x;
            DrawDisplayText(l.c_str(), {i == 0 ? 12 : SCREEN_W - 12 - w, y}, ns, Grad::White, true);
            y += lh + 3;
        }
    }

    float p = (t - (VS_HIT - 0.15f)) / 0.25f;  // "VS": bate em VS_HIT
    if (p > 0) {
        float k = p < 0.6f ? 2 + (0.9f - 2) * (p / 0.6f) : 0.9f + 0.1f * std::min(1.0f, (p - 0.6f) / 0.4f);
        Vector2 m = MeasureDisplayText("VS", 6);
        DrawDisplayText("VS", {roundf(MX - m.x / 2), roundf(92 - m.y / 2)}, 6, Grad::Fire, true, k, std::min(1.0f, p * 4));
        if (t >= VS_HIT) {
            const char* arena = StageName();
            int aw = MeasureUIText(arena, 1);
            DrawUIText(arena, {roundf(MX - aw / 2.0f), 92 + m.y / 2 + 8}, 1, Theme::Ink, true);
        }
    }
    if (t >= VS_HIT && t < VS_HIT + 2 / 60.0f) DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WHITE);  // flash
}

// Tesoura numa área da tela lógica (dentro do Mode2D); EndClipLogical volta à da tela inteira.
static Camera2D viewCam{};
static Rectangle viewClip{};
static void BeginClipLogical(Rectangle r) {
    BeginScissorMode((int)(viewCam.offset.x + r.x * viewCam.zoom), (int)(viewCam.offset.y + r.y * viewCam.zoom),
                     (int)(r.width * viewCam.zoom), (int)(r.height * viewCam.zoom));
}
static void EndClipLogical() {
    EndScissorMode();
    BeginScissorMode((int)viewClip.x, (int)viewClip.y, (int)viewClip.width, (int)viewClip.height);
}

// ---------- tela de suporte ----------
// Mesmo padrão da escolha de lutadores. Lados: palco do jogador (raios na cor dele, almofada redonda) com o pet na pose
// parada respirando; ao trocar o cursor o pet atual sai correndo pela borda de fora e o novo entra correndo por ela
// (corrida 2x), desacelera e para (~0,35 s, com poeira). Embaixo, a ficha: nome, tipo (ATAQUE/CONTROLE/CURA),
// recarga em segmentos, efeito curto ("short" no supports.json) e a tecla de chamar do jogador. Centro: 3 cartões a
// 8° com o rosto de cada pet. Confirmar: pulinho, som (assets/sfx/<id>.wav/.ogg/.mp3, se existir), painel pisca e
// "PRONTO!". Fundo roxo com raios, vinheta, scanlines e chão de lajotas.
const float PET_X0 = 10, PET_X1 = 128, PET_Y0 = 32, PET_Y1 = 184, PET_SL = 12, PET_FEET = 108, PET_SHEET_Y = 114;
const int PET_CARD = 34, PET_CARD_GAP = 4;
struct PetStage { int pet = -1, prev = -1; float t = 1; };  // t: segundos desde a troca (corrida de entrada/saída)
static PetStage petStage[2];
struct Puff { Vector2 p, v; float age; };
static std::vector<Puff> petPuffs;
static int PetShown(int i) { return petPick[i] >= 0 ? petPick[i] : petCursor[i]; }

// A/D (←/→; W/S e ↑/↓ também) escolhem, confirma; desfazer tira a escolha e, sem escolha, volta aos lutadores.
// CPU: o P1 escolhe o próprio e depois o da CPU, com as mesmas teclas. Os dois prontos: 1 s e a cortina leva à arena.
static void UpdatePetSelect() {
    struct Keys { int left, right, up, down, ok, back; };
    const Keys keys[2] = {{KEY_A, KEY_D, KEY_W, KEY_S, KEY_J, KEY_K},
                          {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_KP_1, KEY_KP_2}};
    auto backToFighters = [] { selectStage = 0; pick[0] = pick[1] = -1; petPick[0] = petPick[1] = -1; readyAt = 0; };
    auto confirm = [](int i, int k) {
        petPick[i] = k;
        pickedAt[i] = GetTime();
        PlaySfx(roster.supports.list[k].sfx.c_str());
    };
    if (KeyBack()) { GoTo(backToFighters); return; }
    int n = (int)roster.supports.list.size(), turn = petPick[0] < 0 ? 0 : 1;  // CPU: o P1 escolhe o dela depois do seu
    for (int i = 0; i < 2; i++) {
        int ctl = cpu ? 0 : i;
        const Keys& k = keys[ctl];
        if (cpu && i != turn) continue;
        if (petPick[i] >= 0) {
            if (Press(ctl, k.back, PAD_BACK) && readyAt == 0) petPick[i] = -1;
            continue;
        }
        if (Press(ctl, k.back, PAD_BACK)) {
            if (cpu && i == 1) { petPick[0] = -1; continue; }  // escolhendo o da CPU: desfaz o próprio
            GoTo(backToFighters);
            return;
        }
        int d = (Press(ctl, k.right, PAD_RIGHT) || Press(ctl, k.down, PAD_DOWN)) - (Press(ctl, k.left, PAD_LEFT) || Press(ctl, k.up, PAD_UP));
        if (d) PlaySfx("cursor");
        petCursor[i] = (petCursor[i] + d + n) % n;
        if (Press(ctl, k.ok, PAD_OK)) confirm(i, petCursor[i]);
    }
    if (cpu && petPick[0] < 0) petPick[1] = -1;
    for (int i = 0; i < 2; i++)  // trocou o pet do palco: o antigo sai correndo e o novo entra
        if (PetShown(i) != petStage[i].pet) petStage[i] = {PetShown(i), petStage[i].pet, 0};
    if (petPick[0] < 0 || petPick[1] < 0) return;
    if (readyAt == 0) readyAt = GetTime() + 1.0;
    else if (GetTime() >= readyAt) {
        readyAt = 1e18;
        GoTo(OpenStageSelect);
    }
}

// Faixa central do topo com o título e os jogadores nos cantos (escolha de lutador e de suporte).
static void DrawSelectHeader(const char* title) {
    int ts = MeasureDisplayText(title, 2).x <= SCREEN_W - 2 * 66 ? 2 : 1;
    Vector2 tm = MeasureDisplayText(title, ts);
    float tx = roundf(SCREEN_W / 2.0f - tm.x / 2);
    Band(tx - 14, tx + tm.x + 10, 4, tm.y + 10, 4, INK_DARK);
    Band(tx - 12, tx + tm.x + 8, 6, tm.y + 6, 3, Theme::Red);
    DrawDisplayText(title, {tx, 9}, ts, Grad::White, false);
    DrawDisplayText("1P", {10, 9}, 2, Grad::P1, true);
    DrawDisplayText(PlayerTag(1), {SCREEN_W - 10 - MeasureDisplayText(PlayerTag(1), 2).x, 9}, 2, Grad::P2, true);
}

// Corrida em 2x com os pés em (x, y); dir = +1 para a direita.
static void DrawPetRun(const SupportDef& s, float x, float y, int dir, float t) {
    auto a = s.anims.find("run");
    if (a == s.anims.end() || a->second.frames.empty()) {  // sem arte: bolinha com a inicial
        DrawCircle((int)x, (int)y - 10, 10, Theme::Gold);
        return;
    }
    const Anim& an = a->second;
    int fr = (int)(t * an.fps) % (int)an.frames.size();
    const Texture2D& tx = an.frames[fr];
    float w = tx.width * 2.0f, h = tx.height * 2.0f, ax = an.anchors[fr] * 2;
    Rectangle dst = {roundf(dir > 0 ? x - ax : x - (w - ax)), roundf(y - h + an.boxes[fr].sink * 2), w, h};
    DrawTexturePro(tx, {0, 0, (float)(dir > 0 ? tx.width : -tx.width), (float)tx.height}, dst, {0, 0}, 0, WHITE);
}

// Pose parada no palco, respirando (1 px, ciclo de 1,2 s), com os pés em (x, y); olha para o centro da tela.
static void DrawPetPose(const SupportDef& s, float x, float y, bool flip, Color tint) {
    const Texture2D& t = s.stage.id ? s.stage : s.portrait;
    if (!t.id) { DrawPetRun(s, x, y, flip ? -1 : 1, 0); return; }
    float w = roundf(t.width / (float)PICTURE_RES), h = roundf(t.height / (float)PICTURE_RES);
    float breath = roundf(0.5f + 0.5f * sinf((float)GetTime() * 2 * PI / 1.2f));
    DrawTexturePro(t, {0, 0, (float)(flip ? -t.width : t.width), (float)t.height},
                   {roundf(x - w / 2), roundf(y - h - breath), w, h + breath}, {0, 0}, 0, tint);
}

static void DrawPetSide(int i, float t) {
    const std::vector<SupportDef>& list = roster.supports.list;
    float slide = (1 - EaseOut(t / 0.35f)) * 150 * (i == 0 ? -1 : 1), dt = GetFrameTime();
    auto panel = [&](float grow, Vector2 q[4]) {  // como o painel da escolha de lutador, mais largo
        float x0 = PET_X0 - grow, x1 = PET_X1 + grow, y0 = PET_Y0 - grow, y1 = PET_Y1 + grow;
        Vector2 p[4] = {{x0 + PET_SL, y0}, {x1 + PET_SL, y0}, {x1, y1}, {x0, y1}};
        for (int k = 0; k < 4; k++) q[k] = {(i == 0 ? p[k].x : SCREEN_W - p[(k + (k % 2 ? 3 : 1)) % 4].x) + slide, p[k].y};
    };
    Vector2 q[4], b1[4], b2[4];
    panel(3, b1);
    panel(1, b2);
    panel(0, q);
    FillQuad(b1, INK_DARK);
    FillQuad(b2, P_COLOR[i]);
    FillQuad(q, ColorLerp(Theme::Bg, P_COLOR[i], 0.3f));
    float off = fmodf((float)GetTime() * 10, 20);  // raios paralelos à borda do painel, andando
    for (float x = q[3].x - 40 + off; x < q[2].x + 20; x += 20) {
        float x0 = std::max(x, q[3].x), x1 = std::min(x + 7, q[2].x);
        if (x1 > x0) Band(x0, x1, PET_Y0, PET_Y1 - PET_Y0, i == 0 ? PET_SL : -PET_SL, Fade(WHITE, 0.06f));
    }
    rlDisableBackfaceCulling();  // vinheta do palco: escuro em cima e embaixo
    Quad(q[0], {EdgeX(q, PET_Y0 + 30, false), PET_Y0 + 30}, {EdgeX(q, PET_Y0 + 30, true), PET_Y0 + 30}, q[1], Fade(BLACK, 0.4f), Fade(BLACK, 0));
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();

    // palco: almofada redonda com borda e faixa de padrão, sombra suave
    float cx = roundf((EdgeX(q, PET_FEET, false) + EdgeX(q, PET_FEET, true)) / 2);
    Color cush = ColorLerp(P_COLOR[i], BLACK, 0.35f);
    DrawEllipse((int)cx + 2, (int)PET_FEET + 3, 42, 9, Fade(BLACK, 0.35f));
    DrawEllipse((int)cx, (int)PET_FEET, 40, 9, INK_DARK);
    DrawEllipse((int)cx, (int)PET_FEET - 1, 38, 7, cush);
    DrawEllipseLines((int)cx, (int)PET_FEET - 1, 30, 5, ColorLerp(cush, WHITE, 0.35f));
    for (int k = -3; k <= 3; k++) DrawRectangle((int)cx + k * 8 - 1, (int)PET_FEET - 1, 2, 1, ColorLerp(cush, Theme::Gold, 0.6f));

    // pet: o antigo sai correndo pela borda de fora, o novo entra por ela, desacelera e para na pose parada
    PetStage& st = petStage[i];
    st.t += dt;
    int outward = i == 0 ? -1 : 1;
    float edge = i == 0 ? q[3].x - 50 : q[2].x + 50;
    BeginClipLogical({std::min(q[3].x, q[0].x), PET_Y0, std::max(q[1].x, q[2].x) - std::min(q[3].x, q[0].x), PET_SHEET_Y - PET_Y0});
    const float OUT = 0.12f, IN = 0.23f;
    if (st.t < OUT && st.prev >= 0) {
        float k = st.t / OUT;
        DrawPetRun(list[st.prev], cx + (edge - cx) * k * k, PET_FEET, outward, st.t);
    } else if (st.t < OUT + IN && st.pet >= 0) {
        float k = EaseOut((st.t - OUT) / IN), x = edge + (cx - edge) * k;
        DrawPetRun(list[st.pet], x, PET_FEET, -outward, st.t);
        if (GetRandomValue(0, 1)) petPuffs.push_back({{x + outward * 10, PET_FEET - 1}, {outward * 25.0f, -12}, 0});
    } else if (st.pet >= 0) {
        float hop = 0, k = (float)(GetTime() - pickedAt[i]) / 0.3f;  // confirmou: pulinho
        if (petPick[i] >= 0 && k < 1) hop = roundf(8 * sinf(k * PI));
        DrawPetPose(list[st.pet], cx, PET_FEET - hop, i == 1, WHITE);
    }
    EndClipLogical();

    // ficha: painel escuro inclinado de borda dupla
    Vector2 sheet[4] = {{EdgeX(q, PET_SHEET_Y, false), PET_SHEET_Y}, {EdgeX(q, PET_SHEET_Y, true), PET_SHEET_Y}, q[2], q[3]};
    FillQuad(sheet, Fade(Theme::Bg, 0.94f));
    Vector2 stripe[4] = {sheet[0], sheet[1], {EdgeX(q, PET_SHEET_Y + 2, true), PET_SHEET_Y + 2}, {EdgeX(q, PET_SHEET_Y + 2, false), PET_SHEET_Y + 2}};
    FillQuad(stripe, P_COLOR[i]);
    if (st.pet >= 0) {
        const SupportDef& s = list[st.pet];
        auto at = [&](float y, float w) { return roundf(i == 0 ? EdgeX(q, y + 8, false) + 6 : EdgeX(q, y + 8, true) - 6 - w); };
        float y = PET_SHEET_Y + 6;
        std::string name = Upper(s.name.c_str()), line2;
        int ns = FitName(name, line2, 84, 2);
        DrawDisplayText(name.c_str(), {at(y, MeasureDisplayText(name.c_str(), ns).x + (i == 0 ? 0 : 16)), y}, ns, Grad::White, true);
        const char* key = i == 0 ? "1" : "7";  // tecla de chamar do jogador (P2: Numpad 7)
        DrawKey(i == 0 ? (int)roundf(EdgeX(q, y + 8, true)) - 18 : (int)at(y, 11), (int)y + 2, key);

        y = PET_SHEET_Y + 26;  // tipo
        const char* kind = s.kind == PetKind::Attack ? "ATAQUE" : s.kind == PetKind::Heal ? "CURA" : "CONTROLE";
        Color kc = s.kind == PetKind::Attack ? Theme::Red : s.kind == PetKind::Heal ? Color{70, 190, 90, 255} : Color{70, 150, 255, 255};
        int kw = MeasureUIText(kind, 1);
        float kx = at(y, (float)kw + 8);
        Band(kx, kx + kw + 8, y - 1, 10, 2, kc);
        DrawUIText(kind, {kx + 4, y}, 1, WHITE, true);

        y = PET_SHEET_Y + 38;  // recarga: quanto menor, mais cheia (escala de 0 a 10 s)
        float secs = s.cooldown / 60.0f;
        int fill = std::clamp((int)lroundf(8 * (1 - secs / 10)), 0, 8);
        const char* cd = TextFormat("%.1fs", secs);
        int rw = MeasureUIText("RECARGA", 1), cw = MeasureUIText(cd, 1);
        float rx = at(y, (float)(rw + 4 + 40 + 4 + cw));
        DrawUIText("RECARGA", {rx, y}, 1, Theme::InkDim);
        DrawRectangle((int)rx + rw + 3, (int)y + 1, 8 * 5 + 1, 6, INK_DARK);
        for (int k = 0; k < 8; k++)
            DrawRectangle((int)rx + rw + 4 + k * 5, (int)y + 2, 4, 4, k < fill ? ColorLerp(Theme::Red, Theme::Gold, k / 7.0f) : Theme::Panel);
        DrawUIText(cd, {rx + rw + 4 + 40 + 4, y}, 1, Theme::Ink);

        // efeito em até 2 linhas, quebrando nas palavras
        float maxW = EdgeX(q, PET_SHEET_Y + 60, true) - EdgeX(q, PET_SHEET_Y + 60, false) - 14;
        std::string lines[2], word;
        int li = 0;
        for (size_t c = 0; c <= s.shortText.size() && li < 2; c++) {
            char ch = c < s.shortText.size() ? s.shortText[c] : ' ';
            if (ch != ' ' && ch != '\n') { word += ch; continue; }
            if (word.empty()) continue;
            std::string tryLine = lines[li].empty() ? word : lines[li] + " " + word;
            if (MeasureUIText(tryLine.c_str(), 1) <= maxW) lines[li] = tryLine;
            else if (++li < 2) lines[li] = word;
            word.clear();
        }
        for (int k = 0; k < 2; k++) {
            y = PET_SHEET_Y + 50 + k * 10.0f;
            DrawUIText(lines[k].c_str(), {at(y, (float)MeasureUIText(lines[k].c_str(), 1)), y}, 1, Theme::Ink);
        }
    }
    float k = (float)(GetTime() - pickedAt[i]) / 0.25f;  // confirmou: o painel pisca em branco
    if (petPick[i] >= 0 && k < 1) FillQuad(q, Fade(WHITE, 0.6f * (1 - k)));
    if (petPick[i] >= 0) {
        Vector2 m = MeasureDisplayText("PRONTO!", 2);
        float px = roundf((EdgeX(q, PET_SHEET_Y + 40, false) + EdgeX(q, PET_SHEET_Y + 40, true)) / 2 - m.x / 2);
        Band(px - 8, px + m.x + 6, PET_SHEET_Y + 27, m.y + 10, 3, Fade(INK_DARK, 0.85f));
        DrawDisplayText("PRONTO!", {px, PET_SHEET_Y + 32}, 2, Grad::Gold, true);
    }
}

static void DrawPetSelect() {
    DrawSelectBackground({25, 8, 50, 255}, {110, 40, 140, 255}, {210, 140, 255, 110}, 90);
    // chão de lajotas (casa) embaixo
    const int FY = 188;
    DrawRectangle(0, FY, SCREEN_W, SCREEN_H - FY, {92, 60, 70, 255});
    for (int r = 0; FY + r * 9 < SCREEN_H; r++)
        for (int c = -1; c * 24 < SCREEN_W; c++) {
            int x = c * 24 + (r % 2) * 12, y = FY + r * 9;
            DrawRectangle(x + 1, y + 1, 22, 7, (r + c) % 2 ? Color{120, 78, 82, 255} : Color{108, 70, 76, 255});
        }
    DrawRectangle(0, FY, SCREEN_W, 2, INK_DARK);
    DrawSelectHeader(cpu && petPick[0] >= 0 && petPick[1] < 0 ? "SUPORTE DO RIVAL" : "ESCOLHA SEU SUPORTE");
    float t = (float)(GetTime() - selectAt);
    for (int i = 0; i < 2; i++) DrawPetSide(i, t);

    // poeira dos pés (corrida de troca)
    float dt = GetFrameTime();
    for (Puff& d : petPuffs) d.age += dt, d.p.x += d.v.x * dt, d.p.y += d.v.y * dt, d.v.y += 40 * dt;
    petPuffs.erase(std::remove_if(petPuffs.begin(), petPuffs.end(), [](const Puff& d) { return d.age >= 0.3f; }), petPuffs.end());
    for (const Puff& d : petPuffs) DrawRectangle((int)d.p.x, (int)d.p.y, 2, 2, Fade(Theme::InkDim, 1 - d.age / 0.3f));

    // cartões no meio, na horizontal
    const std::vector<SupportDef>& list = roster.supports.list;
    int n = (int)list.size();
    float sl = roundf(PET_CARD * TAN8), gw = n * PET_CARD + (n - 1) * PET_CARD_GAP + sl;
    float gx = roundf(SCREEN_W / 2.0f - gw / 2), gy = 84, now = (float)GetTime();
    auto cardQuad = [&](int k, float grow, Vector2 q[4]) {
        float x = gx + k * (PET_CARD + PET_CARD_GAP);
        q[0] = {x + sl - grow, gy - grow}, q[1] = {x + sl + PET_CARD + grow, gy - grow};
        q[2] = {x + PET_CARD + grow, gy + PET_CARD + grow}, q[3] = {x - grow, gy + PET_CARD + grow};
    };
    for (int k = 0; k < n; k++) {
        float a = std::clamp((t - 0.15f - k * 0.05f) / 0.15f, 0.0f, 1.0f);
        if (a <= 0) continue;
        Vector2 q[4], b[4];
        cardQuad(k, 1, b);
        cardQuad(k, 0, q);
        FillQuad(b, Fade(INK_DARK, a));
        FillQuad(q, Fade(Theme::Panel, a));
        const SupportDef& s = list[k];
        Rectangle box = {q[3].x + sl / 2, gy, (float)PET_CARD, (float)PET_CARD};
        if (s.card.id) {
            TexturedQuad(s.card, box, q, Fade(WHITE, a), false);
        } else if (s.portrait.id) {  // sem ícone: canto de cima da frente do desenho (a cabeça)
            float side = std::min(s.portrait.width, s.portrait.height) * 0.6f, z = PET_CARD / side;
            TexturedQuad(s.portrait, {box.x - (s.portrait.width - side) * z, box.y, s.portrait.width * z, s.portrait.height * z}, q, Fade(WHITE, a), false);
        }
        const char* nm = Upper(s.name.c_str());
        int w = MeasureUIText(nm, 1);
        bool on = k == PetShown(0) || k == PetShown(1);
        DrawUIText(nm, {roundf(box.x + PET_CARD / 2.0f - w / 2.0f), gy + PET_CARD + 5}, 1, Fade(on ? Theme::Ink : Theme::InkDim, a), true);
    }
    if (t > 0.3f)
        for (int i = 0; i < 2; i++) {  // cursores como na escolha de lutador
            Vector2 q[4];
            cardQuad(PetShown(i), 2, q);
            bool same = PetShown(0) == PetShown(1);
            Color c = P_COLOR[i];
            if (petPick[i] < 0) c = ColorLerp(c, WHITE, 0.35f + 0.35f * sinf(now * 10));
            for (int e = 0; e < 4; e++) {
                if (same && (i == 0) != (e == 0 || e == 3)) continue;
                DrawLineEx(q[e], q[(e + 1) % 4], 2, c);
            }
            const char* tag = PlayerTag(i);
            int tw = MeasureUIText(tag, 1);
            float lx = same ? (i == 0 ? q[0].x - 2 : q[1].x - tw + 2) : (q[0].x + q[1].x) / 2 - tw / 2.0f;
            DrawUIText(tag, {roundf(lx), q[0].y - 10}, 1, P_COLOR[i], true);
        }

    // rodapé: teclas
    float fy = 199;
    int kx = 10;
    for (const char* k : {"A", "D"}) DrawKey(kx, (int)fy - 1, k), kx += 12;
    DrawUIText("MOVER", {(float)kx + 2, fy + 1}, 1, Theme::Ink, true);
    int rx = SCREEN_W - 10 - (12 + MeasureUIText("CONFIRMA", 1) + 8 + 12 + MeasureUIText("VOLTA", 1));
    DrawKey(rx, (int)fy - 1, "J");
    DrawUIText("CONFIRMA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
    rx += 13 + MeasureUIText("CONFIRMA", 1) + 7;
    DrawKey(rx, (int)fy - 1, "K");
    DrawUIText("VOLTA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
}

// ---------- menu inicial ----------

// Cenário da luta (céu, sol, chão), também usado de fundo no menu inicial.
static void DrawStage(const Light& light) {
    DrawRectangleGradientV(0, 0, SCREEN_W, HORIZON_Y, {70, 90, 150, 255}, {230, 170, 140, 255});
    // sol baixo no horizonte, do lado definido no JSON: brilho largo + disco (o chão cobre a metade de baixo)
    int sunX = light.side > 0 ? SCREEN_W - 40 : 40;
    DrawCircleGradient(sunX, HORIZON_Y - 4, 180, {255, 214, 150, 160}, {255, 190, 140, 0});
    DrawCircle(sunX, HORIZON_Y - 4, 13, {255, 238, 195, 255});
    // Chão com profundidade: começa no horizonte, ATRÁS dos lutadores (que pisam em GROUND_Y, dentro dele).
    // Assim quem cai deitado fica sobre o piso, com chão atrás, em vez de "equilibrado" na borda contra o céu.
    // Mais claro do lado do sol (contraste para a sombra) e mais escuro ao fundo (distância).
    Color floorLit = {140, 104, 78, 255}, floorDim = {88, 68, 58, 255};
    DrawRectangleGradientH(0, HORIZON_Y, SCREEN_W, SCREEN_H - HORIZON_Y,
                           light.side > 0 ? floorDim : floorLit, light.side > 0 ? floorLit : floorDim);
    DrawRectangleGradientV(0, HORIZON_Y, SCREEN_W, GROUND_Y - HORIZON_Y, {30, 20, 40, 90}, {30, 20, 40, 0});
    DrawRectangle(0, HORIZON_Y, SCREEN_W, 1, {40, 30, 25, 255});
}

struct TitleCard { const char* title; const char* line1; const char* line2; Color top, bottom; };
static const TitleCard CARDS[4] = {
    {"CONTRA CPU", "Jogue contra o computador.", "Ele defende, apara e rebate seus golpes!", {95, 120, 165, 255}, {40, 50, 85, 255}},
    {"2 JOGADORES", "Dois jogadores no mesmo teclado.", "P1: A/D/W/S + J/K    P2: setas + Numpad 1/2", {255, 205, 45, 255}, {235, 110, 30, 255}},
    {"OPÇÕES", "Janela, tela cheia, música,", "efeitos e rounds para vencer.", {150, 95, 190, 255}, {60, 35, 90, 255}},
    {"SAIR", "Fecha o jogo.", "Volte logo para a revanche!", {70, 160, 170, 255}, {25, 80, 95, 255}},
};

// A/D ou setas escolhem o cartão; J, Numpad 1 ou Enter confirmam.
static void UpdateTitle() {
    if (KeyLeft()) titleSel = (titleSel + 3) % 4;
    if (KeyRight()) titleSel = (titleSel + 1) % 4;
    if (KeyBack()) titleSel = 3;  // Esc no menu inicial vai para SAIR (não fecha direto)
    float k = std::min(1.0f, GetFrameTime() * 12);  // salta/recolhe em ~0,2 s
    for (int i = 0; i < 4; i++) titlePop[i] += ((i == titleSel) - titlePop[i]) * k;
    if (!KeyOk()) return;
    if (titleSel == 0 || titleSel == 1) {
        bool vsCpu = titleSel == 0;
        GoTo([vsCpu] { cpu = vsCpu; OpenSelect(); });
    } else if (titleSel == 2) {
        GoTo([] { OpenOptions(Screen::Title); });
    } else {
        GoTo([] { quit = true; });
    }
}

// Textura `t` posta no retângulo `art`, desenhada só dentro do quadrilátero p (tl, tr, br, bl): cada canto pega a
// coordenada da arte na sua posição, então a arte não deforma e é cortada exatamente pelas bordas inclinadas.
static void TexturedQuad(const Texture2D& t, Rectangle art, const Vector2 p[4], Color tint, bool flip) {
    rlSetTexture(t.id);
    rlBegin(RL_QUADS);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    for (int k : {0, 3, 2, 1}) {  // tl, bl, br, tr: a ordem que o raylib usa (anti-horário na tela)
        float u = (p[k].x - art.x) / art.width, v = (p[k].y - art.y) / art.height;
        rlTexCoord2f(flip ? 1 - u : u, v);
        rlVertex2f(p[k].x, p[k].y);
    }
    rlEnd();
    rlSetTexture(0);
}

// Partículas do cartão escolhido (só visual: nascem e andam no desenho, com o tempo do quadro).
// Tipo = cartão: 0 pixels vermelhos piscando (CPU), 1 faíscas azuis (TV), 2 faíscas amarelas (opções), 3 poeira dourada (sair).
struct Spark { Vector2 p, v; float age, life, seed; int kind; };
static std::vector<Spark> sparks;
static float sparkAcc = 0;

static float Rnd(float a, float b) { return a + (b - a) * GetRandomValue(0, 1000) / 1000.0f; }

static void DrawSparks(int card, const Vector2 q[4]) {
    float dt = GetFrameTime();
    for (sparkAcc += menu.rate[card] * dt; sparkAcc >= 1 && sparks.size() < 200; sparkAcc--) {
        float u = Rnd(0, 1), v = Rnd(0, 1);  // ponto qualquer dentro do cartão
        Vector2 a = {q[0].x + (q[1].x - q[0].x) * u, q[0].y}, b = {q[3].x + (q[2].x - q[3].x) * u, q[3].y};
        Vector2 vel[4] = {{Rnd(-5, 5), Rnd(-25, -10)}, {Rnd(-60, 60), Rnd(-70, 20)}, {Rnd(2, 10), Rnd(-10, -3)},
                          {Rnd(-30, 30), Rnd(-80, -40)}};
        float life = menu.life * (card == 1 || card == 2 ? Rnd(0.3f, 0.6f) : Rnd(0.7f, 1.0f));
        sparks.push_back({{a.x + (b.x - a.x) * v, a.y + (b.y - a.y) * v}, vel[card], 0, life, Rnd(0, 6.28f), card});
    }
    sparkAcc = std::min(sparkAcc, 1.0f);
    const float gravity[4] = {0, 120, 0, 160};
    BeginBlendMode(BLEND_ADDITIVE);
    for (Spark& s : sparks) {
        s.age += dt;
        s.v.y += gravity[s.kind] * dt;
        s.p.x += (s.v.x + (s.kind == 3 ? 8 * sinf(s.age * 3 + s.seed) : 0)) * dt;  // poeira balança
        s.p.y += s.v.y * dt;
        float alpha = std::max(0.0f, 1 - s.age / s.life);
        if (s.kind == 0 && sinf(s.age * 25 + s.seed) < 0) alpha *= 0.3f;
        Color c = Fade(menu.color[s.kind], alpha);
        if (s.kind == 1 || s.kind == 2)  // faísca = risquinho na direção em que voa
            DrawLineEx(s.p, {s.p.x - s.v.x * 0.04f, s.p.y - s.v.y * 0.04f}, 1, c);
        else
            DrawRectangleV(s.p, {s.kind == 0 ? 2.0f : 1.0f, s.kind == 0 ? 2.0f : 1.0f}, c);
    }
    EndBlendMode();
    sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& s) { return s.age >= s.life; }),
                 sparks.end());
}

// Logo flutuando (±2 px, ciclo de 2 s); a cada 3 s uma faixa de brilho diagonal atravessa as letras: a silhueta
// branca do logo desenhada só dentro da faixa (aditiva, some nas bordas da faixa).
static void DrawLogo() {
    float t = (float)GetTime(), w = logo.width / (float)PICTURE_RES, h = logo.height / (float)PICTURE_RES;
    Vector2 o = {6, 2 + 2 * sinf(t * PI)};
    DrawTexturePro(logo, {0, 0, (float)logo.width, (float)logo.height}, {o.x, o.y, w, h}, {0, 0}, 0, WHITE);
    float s = fmodf(t, 3) / 0.7f;  // atravessa em 0,7 s
    if (s >= 1 || !logoShine.id) return;
    const float BW = 9, SL = h * 0.5f, PAD = 1.0f / PICTURE_RES;  // meia largura, inclinação, borda transparente
    float cx = o.x - BW - SL + s * (w + 2 * BW + SL), y0 = o.y - PAD, y1 = o.y + h + PAD;
    Rectangle art = {o.x - PAD, y0, w + 2 * PAD, y1 - y0};
    BeginBlendMode(BLEND_ADDITIVE);
    rlSetTexture(logoShine.id);
    rlBegin(RL_QUADS);
    for (int half = 0; half < 2; half++) {  // duas metades: alfa 0 na borda da faixa, cheio no meio
        float xa = half ? 0 : -BW, xb = half ? BW : 0;
        Vector2 v[4] = {{cx + SL + xa, y0}, {cx + xa, y1}, {cx + xb, y1}, {cx + SL + xb, y0}};  // tl, bl, br, tr
        for (int k = 0; k < 4; k++) {
            rlColor4ub(255, 255, 255, (k < 2) == (half == 1) ? 210 : 0);
            rlTexCoord2f((v[k].x - art.x) / art.width, (v[k].y - art.y) / art.height);
            rlVertex2f(v[k].x, v[k].y);
        }
    }
    rlEnd();
    rlSetTexture(0);
    EndBlendMode();
}

// Menu no estilo da loja do Hi-Fi Rush: cartões inclinados, cada um com sua cena animada recortada pelas bordas
// inclinadas; o escolhido cresce, sobe, ganha sombra, parallax, borda pulsando e partículas; os outros escurecem.
static void DrawTitle() {
    DrawMenuBackground();

    const float W = 80, H = 104, SL = 16, GAP = 6, Y0 = 56;  // cartões um pouco mais baixos: espaço para o logo
    float x0 = (SCREEN_W - (4 * W + 3 * GAP + SL)) / 2, now = (float)GetTime();
    int order[4] = {0, 1, 2, 3};  // o escolhido por último: fica por cima dos vizinhos
    std::swap(order[titleSel], order[3]);
    std::sort(order, order + 3);
    Vector2 selQ[4];
    for (int i : order) {
        float pop = titlePop[i], sc = 1 + (menu.zoom - 1) * pop;
        float bx = x0 + i * (W + GAP);
        Vector2 c = {bx + (W + SL) / 2, Y0 + H / 2};
        auto P = [&](float x, float y) { return Vector2{c.x + (x - c.x) * sc, c.y + (y - c.y) * sc - 4 * pop}; };
        Vector2 q[4] = {P(bx + SL, Y0), P(bx + W + SL, Y0), P(bx + W, Y0 + H), P(bx, Y0 + H)};
        unsigned char dim = (unsigned char)(255 * menu.dim);
        Color tint = ColorLerp({dim, dim, dim, 255}, WHITE, pop);  // os outros ficam escurecidos
        auto quad = [&](const Vector2* v, Color a, Color b) { Quad(v[0], v[1], v[2], v[3], a, b); };
        rlDisableBackfaceCulling();
        if (pop > 0.01f) {  // sombra deslocada: o cartão "sai" da tela
            Vector2 sh[4];
            for (int k = 0; k < 4; k++) sh[k] = {q[k].x + 5 * pop, q[k].y + 6 * pop};
            quad(sh, Fade(BLACK, 0.5f * pop), Fade(BLACK, 0.5f * pop));
        }
        const auto& frames = cardFrames[i];
        if (frames.empty()) {  // sem cena: degradê de cima para baixo
            rlBegin(RL_TRIANGLES);
            for (int k : {0, 3, 2, 0, 2, 1}) {
                Color col = ColorTint(k < 2 ? CARDS[i].top : CARDS[i].bottom, tint);
                rlColor4ub(col.r, col.g, col.b, col.a);
                rlVertex2f(q[k].x, q[k].y);
            }
            rlEnd();
        }
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();

        if (!frames.empty()) {  // cena em loop (mais lenta nos apagados), cortada pelas bordas inclinadas
            float fps = i == titleSel ? menu.fps : menu.fpsDim;
            const Texture2D& art = frames[(int)(now * fps) % frames.size()];
            float side = H * sc + 2 * menu.parallax + 4;  // sobra em volta: o parallax nunca mostra a borda da cena
            Vector2 par = {menu.parallax * pop * sinf(now * 0.9f), 0.5f * menu.parallax * pop * cosf(now * 0.7f)};
            TexturedQuad(art, {c.x - side / 2 + par.x, c.y - 4 * pop - side / 2 + par.y, side, side}, q, tint, false);
        }

        // nome embaixo numa faixa preta inclinada; contorno do cartão
        float ly = q[3].y - 22 * sc;
        Band(q[3].x + 2, q[2].x - 2, ly, 15 * sc, SL * 15 / H, Fade(BLACK, 0.85f));
        // losango em todos os cartões, no fim da faixa e na altura do meio do nome; o nome encolhe para caber antes dele
        const char* t = CARDS[i].title;
        float midY = ly + 7.5f * sc, tx = q[3].x + 7 * sc;
        Vector2 d = {q[2].x - 7 * sc, midY};
        int size = 10;
        while (size > 7 && tx + TextW(t, size) > d.x - 8) size--;
        OutlinedText(t, (int)tx, (int)(midY - size / 2.0f), size, i == titleSel ? YELLOW : WHITE, 1);
        float dr = 3.5f + (i == titleSel ? 0.5f * sinf(now * 6) : 0);  // o do escolhido pulsa junto com a borda
        DrawPoly(d, 4, dr + 1.2f, 0, BLACK);
        DrawPoly(d, 4, dr, 0, i == titleSel ? Color{255, 60, 70, 255} : Color{190, 30, 45, 255});
        Color edge = i == titleSel ? Fade(WHITE, 0.65f + 0.35f * sinf(now * 6)) : BLACK;  // borda do escolhido pulsa
        for (int k = 0; k < 4; k++) DrawLineEx(q[k], q[(k + 1) % 4], 1.5f + pop, edge);
        if (i == titleSel) std::copy(q, q + 4, selQ);
    }
    DrawSparks(titleSel, selQ);

    // logo no canto (como o "STORE"), por último: nada passa por cima dele
    if (logo.id) {
        DrawCornerTitle("", "MENU INICIAL");
        DrawLogo();
    } else {
        DrawCornerTitle("DANIN FIGHTERS", "MENU INICIAL");
    }

    // caixa de descrição branca (como a da loja) e as teclas no canto
    Band(52, 336, 166, 30, 6, BLACK);
    Band(54, 334, 168, 26, 6, {245, 242, 235, 255});
    for (int k = 0; k < 2; k++) {
        const char* line = k ? CARDS[titleSel].line2 : CARDS[titleSel].line1;
        Text(line, SCREEN_W / 2 + 3 - TextW(line, 10) / 2, 171 + k * 11, 10, {30, 25, 35, 255});
    }
    if (GetTime() >= msgUntil) {  // crédito escondido: menor que a menor fonte (cada px dela = zoom-1 px do monitor, nítido)
        float k = std::max(1.0f, viewCam.zoom - 1) / viewCam.zoom, h = UITextHeight(1) * k;
        rlPushMatrix();
        rlTranslatef(3, SCREEN_H - 2 - h, 0);
        rlScalef(k, k, 1);
        DrawUIText("Developed by Lui Loureiro", {0, 0}, 1, Fade(WHITE, 0.22f));
        rlPopMatrix();
    }
}

// ---------- opções ----------

static void SaveOptions() {
    nlohmann::json j = {{"window_scale", opt.scale}, {"fullscreen", opt.fullscreen}, {"music", opt.music},
                        {"sfx", opt.sfx}, {"rounds_to_win", opt.wins}, {"cpu_level", opt.cpuLevel}, {"max_fps", opt.maxFps}, {"show_boxes", debug}};
    std::ofstream(SETTINGS) << j.dump(2);
}

static void LoadOptions() {
    try {
        std::ifstream f(SETTINGS);
        if (!f) return;  // primeira vez: valores padrão
        nlohmann::json j = nlohmann::json::parse(f);
        opt.scale = std::clamp(j.value("window_scale", opt.scale), 1, 8);
        opt.fullscreen = j.value("fullscreen", opt.fullscreen);
        opt.music = std::clamp(j.value("music", opt.music), 0, 100);
        opt.sfx = std::clamp(j.value("sfx", opt.sfx), 0, 100);
        opt.wins = std::clamp(j.value("rounds_to_win", opt.wins), 1, 3);
        opt.cpuLevel = std::clamp(j.value("cpu_level", opt.cpuLevel), 0, CPU_LEVEL_N - 1);
        opt.maxFps = j.value("max_fps", opt.maxFps);
        debug = j.value("show_boxes", debug);
    } catch (const std::exception& e) {
        TraceLog(LOG_WARNING, "OPCOES: %s ignorado (%s)", SETTINGS, e.what());
    }
}

// Maior zoom inteiro que cabe no monitor (com folga para a barra de tarefas).
static int MaxScale() {
    int m = GetCurrentMonitor();
    return std::max(1, std::min(GetMonitorWidth(m) / SCREEN_W, (GetMonitorHeight(m) - 80) / SCREEN_H));
}

// Tela cheia = janela sem borda do tamanho do monitor (não muda a resolução do monitor). Janela: zoom inteiro
// escolhido, centralizada.
// Quadros por segundo. Padrão (econômico, como um console): ~60, desenhando 1 quadro a cada N atualizações do
// monitor (165 Hz → 55 quadros, 144 → 72, 120 → 60, 60 → 60): ritmo sempre regular, e a interpolação mantém o
// movimento liso. MÁXIMO: um quadro por atualização do monitor (mais liso em 144/165 Hz, ~3x mais trabalho).
#ifndef PLATFORM_WEB
extern "C" void glfwSwapInterval(int interval);  // do GLFW que vem dentro da raylib
#endif
static int fpsMonitor = -1, fpsRefresh = 0;
static float framePeriod = 0;  // duração exata de um quadro (intervalo / taxa do monitor); 0 = desconhecida
static void ApplyFrameRate(bool force) {
#ifndef PLATFORM_WEB
    int m = GetCurrentMonitor(), hz = GetMonitorRefreshRate(m);
    if (!force && m == fpsMonitor && hz == fpsRefresh) return;
    fpsMonitor = m;
    fpsRefresh = hz;
    int interval = opt.maxFps || hz <= 0 ? 1 : std::max(1, (int)lroundf(hz / 60.0f));
    glfwSwapInterval(interval);
    framePeriod = hz > 0 ? (float)interval / hz : 0;
#endif
}

static void ApplyWindow() {
#ifdef PLATFORM_WEB
    return;  // na web o tamanho é o da página (UpdateDrawFrame)
#endif
    if (opt.fullscreen != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
    if (opt.fullscreen) return;
    opt.scale = std::min(opt.scale, MaxScale());
    int m = GetCurrentMonitor(), w = SCREEN_W * opt.scale, h = SCREEN_H * opt.scale;
    SetWindowSize(w, h);
    Vector2 mp = GetMonitorPosition(m);
    SetWindowPosition((int)mp.x + (GetMonitorWidth(m) - w) / 2, (int)mp.y + (GetMonitorHeight(m) - h) / 2);
}

enum { OPT_SCALE, OPT_FULL, OPT_FPS, OPT_MUSIC, OPT_SFX, OPT_ROUNDS, OPT_CPU, OPT_BOXES, OPT_BACK, OPT_N };
static const char* OPT_NAMES[OPT_N] = {"TAMANHO DA JANELA", "TELA CHEIA", "QUADROS POR SEGUNDO", "MÚSICA", "EFEITOS SONOROS",
                                       "ROUNDS PARA VENCER", "DIFICULDADE DA CPU", "MOSTRAR CAIXAS (F1)", "VOLTAR"};
static const char* CPU_LEVEL_NAMES[CPU_LEVEL_N] = {"FÁCIL", "MÉDIO", "DIFÍCIL", "AVANÇADO"};
static const char* OPT_HELP[OPT_N][2] = {
    {"Tamanho da janela em múltiplos da tela do jogo,", "sempre nítido (pixel inteiro)."},
    {"Ocupa o monitor inteiro, sem borda.", "A janela volta ao tamanho escolhido ao desligar."},
    {"ECONÔMICO: ~60 por segundo, leve em qualquer PC.", "MÁXIMO: um por atualização do monitor (144/165 Hz)."},
    {"Volume da música dos menus.", "0% = sem música."},
    {"Volume dos golpes e efeitos. O jogo ainda não tem", "sons: o volume fica guardado para quando entrarem."},
    {"Quantos rounds para levar a luta:", "1 = melhor de 1, 2 = melhor de 3, 3 = melhor de 5."},
    {"Fácil e médio para aprender; difícil e avançado", "reagem rápido, defendem, usam especial e punem."},
    {"Mostra onde cada um apanha (verde), acerta (vermelho)", "e apara (ciano). O mesmo que a tecla F1."},
    {"Volta para a tela anterior.", "As opções já ficam gravadas ao mudar."},
};

static std::string OptValue(int row) {
    switch (row) {
    case OPT_SCALE: return opt.fullscreen ? "(tela cheia)" : TextFormat("%dx  (%dx%d)", opt.scale, SCREEN_W * opt.scale, SCREEN_H * opt.scale);
    case OPT_FULL: return opt.fullscreen ? "SIM" : "NÃO";
    case OPT_FPS: return opt.maxFps ? TextFormat("MÁXIMO (%d)", GetMonitorRefreshRate(GetCurrentMonitor())) : "ECONÔMICO";
    case OPT_MUSIC: return TextFormat("%d%%", opt.music);
    case OPT_SFX: return TextFormat("%d%%", opt.sfx);
    case OPT_ROUNDS: return TextFormat("%d  (melhor de %d)", opt.wins, 2 * opt.wins - 1);
    case OPT_CPU: return CPU_LEVEL_NAMES[opt.cpuLevel];
    case OPT_BOXES: return debug ? "SIM" : "NÃO";
    default: return "";
    }
}

// W/S escolhem a linha; A/D mudam o valor; J/Enter liga/desliga ou VOLTAR; Esc volta. Grava a cada mudança.
static void UpdateOptions() {
    if (KeyBack() || (optSel == OPT_BACK && KeyOk())) {
        GoTo([] { screen = optionsFrom; });
        return;
    }
    if (KeyUp()) optSel = (optSel + OPT_N - 1) % OPT_N;
    if (KeyDown()) optSel = (optSel + 1) % OPT_N;
    int d = KeyRight() - KeyLeft();
    bool toggle = KeyOk() || d != 0;
    bool changed = true;
    switch (optSel) {
    case OPT_SCALE:
        if (d && !opt.fullscreen) { opt.scale = std::clamp(opt.scale + d, 1, MaxScale()); ApplyWindow(); }
        else changed = false;
        break;
    case OPT_FULL:
        if (toggle) { opt.fullscreen = !opt.fullscreen; ApplyWindow(); }
        else changed = false;
        break;
    case OPT_FPS:
        if (toggle) { opt.maxFps = !opt.maxFps; ApplyFrameRate(true); }
        else changed = false;
        break;
    case OPT_MUSIC: opt.music = std::clamp(opt.music + 10 * d, 0, 100); changed = d != 0; break;
    case OPT_SFX: opt.sfx = std::clamp(opt.sfx + 10 * d, 0, 100); changed = d != 0; break;
    case OPT_ROUNDS: opt.wins = std::clamp(opt.wins + d, 1, 3); changed = d != 0; break;
    case OPT_CPU: opt.cpuLevel = std::clamp(opt.cpuLevel + d, 0, CPU_LEVEL_N - 1); changed = d != 0; break;
    case OPT_BOXES:
        if (toggle) debug = !debug;
        else changed = false;
        break;
    default: changed = false;
    }
    if (changed) SaveOptions();
}

// Fundo dos menus: cenário da luta escurecido com faixas diagonais leves.
static void DrawMenuBackground() {
    DrawStage(roster.light);
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade({20, 10, 30, 255}, 0.55f));
    for (int i = 0; i < 12; i++)
        DrawLineEx({i * 40.0f, 0}, {i * 40.0f - 120, (float)SCREEN_H}, 10, Fade(WHITE, 0.03f));
}

// Faixa de título no canto, no estilo do menu inicial.
static void DrawCornerTitle(const char* title, const char* right) {
    if (*title) {  // vazio = só a faixa do outro canto (o menu inicial desenha o logo no lugar)
        Band(-12, 168, 6, 24, 7, {215, 35, 50, 255});
        Band(-12, 168, 30, 3, 7, BLACK);
        OutlinedText(title, 8, 8, 20, WHITE, 2);
    }
    Band(SCREEN_W - 120, SCREEN_W + 12, 6, 13, -7, Fade(BLACK, 0.8f));
    Band(SCREEN_W - 120, SCREEN_W + 12, 19, 2, -7, {215, 35, 50, 255});
    OutlinedText(right, SCREEN_W - 8 - TextW(right, 10), 8, 10, WHITE, 1);
}

static void DrawOptions() {
    DrawMenuBackground();
    DrawCornerTitle("OPÇÕES", optionsFrom == Screen::Fight ? "PAUSA" : "MENU INICIAL");
    const float X0 = 40, X1 = 344, Y0 = 36, ROW = 14;
    for (int i = 0; i < OPT_N; i++) {
        float y = Y0 + i * ROW + (i == OPT_BACK ? 4 : 0);
        bool sel = i == optSel;
        Band(X0, X1, y, ROW - 3, 5, sel ? Color{255, 215, 60, 255} : Fade(BLACK, 0.55f));
        Color text = sel ? BLACK : WHITE;
        Text(OPT_NAMES[i], (int)X0 + 10, (int)y + 2, 10, text);
        if (i == OPT_MUSIC || i == OPT_SFX) {  // barra de 10 pedaços
            int v = (i == OPT_MUSIC ? opt.music : opt.sfx) / 10;
            for (int k = 0; k < 10; k++)
                DrawRectangle((int)X1 - 136 + k * 7, (int)y + 3, 5, 7, k < v ? (sel ? BLACK : Color{255, 215, 60, 255}) : Fade(text, 0.25f));
        }
        std::string v = OptValue(i);
        if (!v.empty()) {
            bool arrows = i != OPT_FULL && i != OPT_FPS && i != OPT_BOXES && !(i == OPT_SCALE && opt.fullscreen);
            int vx = (int)X1 - 14 - TextW(v.c_str(), 10);
            if (i == OPT_MUSIC || i == OPT_SFX) vx = (int)X1 - 14 - 28;
            Text(v.c_str(), vx, (int)y + 2, 10, text);
            if (sel && arrows) {
                Text("<", vx - 10, (int)y + 2, 10, text);
                Text(">", (int)X1 - 10, (int)y + 2, 10, text);
            }
        }
    }
    Band(52, 336, 166, 30, 6, BLACK);
    Band(54, 334, 168, 26, 6, {245, 242, 235, 255});
    for (int k = 0; k < 2; k++)
        Text(OPT_HELP[optSel][k], SCREEN_W / 2 + 3 - TextW(OPT_HELP[optSel][k], 10) / 2, 171 + k * 11, 10, {30, 25, 35, 255});
    const char* hint = "W/S ESCOLHE   A/D MUDA   ESC VOLTA";
    OutlinedText(hint, SCREEN_W - 6 - TextW(hint, 10), SCREEN_H - 13, 10, WHITE, 1);
}

// Cortina: faixa preta inclinada com borda vermelha atravessa a tela da esquerda para a direita. Cobre (0..1),
// a tela troca por baixo, e descobre (1..2) continuando para o mesmo lado.
static void DrawTransition() {
    if (transT < 0) return;
    float k = transT < 1 ? transT : transT - 1;
    k = k * k * (3 - 2 * k);  // começa e termina suave
    const float SL = 60, x = -SL - 10 + (SCREEN_W + SL + 20) * k;
    if (transT < 1) {
        Band(-SL - 30, x, 0, SCREEN_H, SL, transColor);
        Band(x, x + 12, 0, SCREEN_H, SL, transEdge);
    } else {
        Band(x, SCREEN_W + SL + 30, 0, SCREEN_H, SL, transColor);
        Band(x - 12, x, 0, SCREEN_H, SL, transEdge);
    }
}

static void UpdateTransition() {
    if (transT < 0) return;
    transT += GetFrameTime() / 0.3f;  // 0,3 s para cobrir, 0,3 s para descobrir
    if (transT >= 1 && transDo) {
        auto change = std::move(transDo);
        transDo = nullptr;
        change();
        acc = 0;  // a luta nova não "recupera" o tempo da cortina
    }
    if (transT >= 2) {
        transT = -1;
        transColor = BLACK;
        transEdge = {215, 35, 50, 255};
    }
}

// ---------- tela de loading ----------
// Abre o jogo. Fila de tarefas sem threads (QueueRoster: mascotes primeiro, depois cada animação dos lutadores; por
// fim as cenas do menu): cada quadro roda tarefas por até ~12 ms e desenha. Entrada: wipe diagonal na cor do fundo
// + fade (0,3 s). Saída: os mascotes correm para fora pela direita, a cena vai para a cor do fundo e a cortina (na
// cor do fundo) revela o menu. Tudo em px inteiros da tela lógica. Cores em theme.h.
static const char* TIPS = "assets/data/loading_tips.json";   // frases e "scanlines" (liga/desliga)
static const char* LOADING_BG = "assets/ui/loading_bg.png";  // opcional: camada de trás do fundo
static std::vector<std::function<void()>> loadTasks;
static size_t loadDone = 0, loadTotal = 0;
static Roster loadRoster;                    // montado pela fila; vai para `roster` numa tarefa
static const SupportSet* loadPets = nullptr;  // mascotes já carregados (desenhados correndo)
static std::vector<std::string> tips;
static bool scanlines = true;
static Texture2D loadBg{};
static float loadT = 0, loadShown = 0, loadOutT = -1, petT = 0, runX = 0, dustAcc = 0;
struct Dust { Vector2 p, v; float age; };
static std::vector<Dust> dust;
const int LOAD_M = 12, LOAD_GROUND = 194, BAR_Y = 140, BAR_H = 9, BAR_SEGS = 46;  // margem, chão, barra

static void Loading_Begin() {
    screen = Screen::Loading;
    loadTasks.clear();
    loadDone = 0;
    loadPets = nullptr;
    loadT = loadShown = petT = runX = dustAcc = 0;
    loadOutT = -1;
    tips = {"Segure para trás para defender!"};
    try {
        std::ifstream f(TIPS);
        nlohmann::json j = f ? nlohmann::json::parse(f) : nlohmann::json::object();
        scanlines = j.value("scanlines", true);
        std::vector<std::string> t = j.value("tips", std::vector<std::string>{});
        if (!t.empty()) tips = t;
    } catch (const std::exception& e) {
        TraceLog(LOG_WARNING, "LOADING: %s ignorado (%s)", TIPS, e.what());
    }
    if (FileExists(LOADING_BG)) loadBg = LoadTexture(LOADING_BG);
    try {
        QueueRoster(DATA, loadTasks, loadRoster, loadPets);
        loadTasks.push_back([] {  // lutadores e mascotes prontos: entram no jogo
            UnloadRoster(roster);
            roster = std::move(loadRoster);
            loaded = true;
            loadPets = &roster.supports;
        });
        loadTasks.push_back(LoadStage);
        loadTasks.push_back(LoadStageList);
    } catch (const std::exception& e) {
        Say(std::string("Erro no JSON: ") + e.what());
        loadTasks.clear();
    }
    LoadMenuBase(loadRoster.chromaTolerance);  // logo já na primeira tela
    for (int i = 0; i < 4; i++) loadTasks.push_back([i] { LoadCard(i); });
    loadTotal = loadTasks.size();
}

// x do mascote k de n: entram correndo pela esquerda quando carregam, saem pela direita no fim.
static float LoadPetX(int k, int n) {
    float e = 1 - std::min(1.0f, petT / 0.7f), o = std::max(0.0f, loadOutT);
    return SCREEN_W / 2.0f + (k - (n - 1) / 2.0f) * 64 - 240 * e * e + 520 * o * o;
}

static void Loading_Update() {
    // tempo real (frases e corrida no ritmo certo mesmo com tarefa longa); o limite só evita pulo enorme
    float dt = std::min(GetFrameTime(), 0.25f);
    loadT += dt;
    if (loadT >= 0.3f) {  // depois da entrada: tarefas por até ~12 ms neste quadro (no mínimo uma)
        double t0 = GetTime();
        while (loadDone < loadTotal && GetTime() - t0 < 0.012) {
            try {
                loadTasks[loadDone++]();
            } catch (const std::exception& e) {  // descarta a fila: o que já carregou é liberado
                Say(std::string("Erro ao carregar: ") + e.what());
                loadPets = nullptr;
                loadTasks.clear();
                loadDone = loadTotal = 0;
            }
        }
        if (loadDone == loadTotal) loadTasks.clear();
    }
    float progress = loadTotal ? (float)loadDone / loadTotal : 1;
    loadShown += (progress - loadShown) * std::min(1.0f, dt * 8);  // a barra anda suave
    if (loadPets) petT += dt;
    runX += 40 * dt;

    // poeira: a cada ~0,08 s cada mascote solta 2-3 pixels nos pés, que somem em 0,3 s
    if (loadPets && (dustAcc += dt) >= 0.08f) {
        dustAcc = 0;
        int n = (int)loadPets->list.size();
        for (int k = 0; k < n; k++)
            for (int m = GetRandomValue(2, 3); m > 0; m--)
                dust.push_back({{LoadPetX(k, n) - (float)GetRandomValue(4, 10), (float)LOAD_GROUND - 1},
                                {-(float)GetRandomValue(15, 35), -(float)GetRandomValue(4, 14)}, 0});
    }
    for (Dust& d : dust) {
        d.age += dt;
        d.p.x += d.v.x * dt;
        d.p.y += d.v.y * dt;
        d.v.y += 40 * dt;
    }
    dust.erase(std::remove_if(dust.begin(), dust.end(), [](const Dust& d) { return d.age >= 0.3f; }), dust.end());

    // fim: tudo carregado, barra cheia e um mínimo de tela (não pisca) -> mascotes saem e a cortina revela o menu
    if (loadOutT < 0 && loadDone == loadTotal && loadShown > 0.995f && loadT > 1.2f) loadOutT = 0;
    if (loadOutT >= 0 && (loadOutT += dt) >= 1) {
        if (loadBg.id) UnloadTexture(loadBg), loadBg = {};
        dust.clear();
        screen = Screen::Title;
        transT = 1;  // só a metade que descobre
        transColor = Theme::Bg;
        transEdge = Theme::Gold;
    }
}

static unsigned Hash(int k, int seed) {
    unsigned h = (unsigned)k * 374761393u + (unsigned)seed * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Silhueta de casas da vila: uma por vaga de `slot` px, tamanho e telhado sorteados pelo índice (sempre iguais).
static void DrawHouses(float scroll, int slot, Color c, int seed, int minH, int maxH, bool windows) {
    int off = (int)floorf(scroll);
    for (int k = (int)floorf(scroll / slot) - 1; k * slot - off < SCREEN_W + slot; k++) {
        unsigned h = Hash(k, seed);
        int w = slot * (60 + (int)(h % 35)) / 100, hh = minH + (int)((h >> 8) % (maxH - minH + 1));
        int x = k * slot - off + (slot - w) / 2, y = LOAD_GROUND - hh, roof = 4 + (int)((h >> 16) % 8);
        DrawRectangle(x, y, w, hh, c);
        if ((h >> 4) & 1)  // duas águas
            DrawTriangle({(float)(x + w / 2), (float)(y - roof)}, {(float)(x - 2), (float)y}, {(float)(x + w + 2), (float)y}, c);
        else  // laje com caixa d'água
            DrawRectangle(x + w - 9, y - 4, 6, 4, c);
        if (windows && (h >> 12) % 3 == 0) DrawRectangle(x + w / 3, y + 6, 3, 3, Fade(Theme::Gold, 0.3f));
    }
}

static void Loading_Draw() {
    DrawRectangleGradientV(0, 0, SCREEN_W, SCREEN_H, Theme::Bg, {0x1C, 0x20, 0x3C, 255});
    if (loadBg.id) {  // imagem opcional como camada de trás, repetida na horizontal
        int w = std::max(1, loadBg.width * SCREEN_H / loadBg.height), off = (int)(runX * 0.25f) % w;
        for (int x = -off; x < SCREEN_W; x += w)
            DrawTexturePro(loadBg, {0, 0, (float)loadBg.width, (float)loadBg.height}, {(float)x, 0, (float)w, (float)SCREEN_H},
                           {0, 0}, 0, WHITE);
    } else {
        DrawHouses(runX * 0.25f, 30, {0x17, 0x1A, 0x32, 255}, 1, 22, 44, false);
    }
    DrawHouses(runX * 0.5f, 46, {0x11, 0x13, 0x27, 255}, 2, 14, 32, true);
    DrawRectangle(0, LOAD_GROUND, SCREEN_W, SCREEN_H - LOAD_GROUND, {0x0A, 0x0B, 0x17, 255});
    DrawRectangle(0, LOAD_GROUND, SCREEN_W, 1, Fade(Theme::InkDim, 0.7f));
    for (int x = -((int)runX % 24); x < SCREEN_W; x += 24) DrawRectangle(x, LOAD_GROUND + 5, 8, 1, Theme::Border);

    // logo flutuando 1 px (ciclo de 2 s)
    int bob = (int)lroundf(sinf(loadT * PI));
    if (logo.id) {
        int lh = 52, lw = logo.width * lh / logo.height;
        DrawTexturePro(logo, {0, 0, (float)logo.width, (float)logo.height},
                       {(float)((SCREEN_W - lw) / 2), (float)(18 + bob), (float)lw, (float)lh}, {0, 0}, 0, WHITE);
    } else {
        const char* t = "DANIN FIGHTERS";
        int x = (SCREEN_W - TextW(t, 20)) / 2, y = 34 + bob;
        Text(t, x + 2, y + 2, 20, Theme::Gold);  // sombra dourada
        OutlinedText(t, x, y, 20, Theme::Ink, 1);
    }

    // painel com a dica (troca a cada 2,5 s com fade de 0,2 s) e o ícone de um mascote
    Rectangle pr = {LOAD_M, 80, SCREEN_W - 2 * LOAD_M, 50};
    DrawRectangleRounded(pr, 6 / pr.height, 4, Theme::Panel);  // raio 3 px
    DrawRectangleRoundedLinesEx(pr, 6 / pr.height, 4, 1, Theme::Border);
    int idx = (int)(loadT / 2.5f) % (int)tips.size();
    float ph = fmodf(loadT, 2.5f), a = std::clamp(std::min(ph, 2.5f - ph) / 0.2f, 0.0f, 1.0f);
    const char* tip = tips[idx].c_str();
    int size = 10;
    while (size > 7 && TextW(tip, size) > pr.width - 44) size--;
    int gx = (SCREEN_W - (22 + TextW(tip, size))) / 2, cy = 105;
    const Texture2D* icon = loadPets && !loadPets->list.empty() ? &loadPets->list[idx % loadPets->list.size()].icon : nullptr;
    if (icon && icon->id) {
        DrawCircle(gx + 8, cy, 9, Fade(Theme::Border, a));
        DrawTexturePro(*icon, {0, 0, (float)icon->width, (float)icon->height}, {(float)gx, (float)(cy - 8), 16, 16}, {0, 0},
                       0, Fade(WHITE, a));
    } else {
        DrawPoly({(float)(gx + 8), (float)cy}, 4, 5, 0, Fade(Theme::Gold, a));
    }
    Text(tip, gx + 22, cy - size / 2, size, Fade(Theme::InkDim, a));

    // barra de energia: segmentos de 4 px com 1 px de vão, contorno escuro, vermelho -> dourado, brilho passando
    int bw = BAR_SEGS * 5 + 1, filled = (int)(loadShown * BAR_SEGS + 0.001f);
    DrawRectangle(LOAD_M - 1, BAR_Y - 1, bw + 2, BAR_H + 2, Theme::Border);
    DrawRectangle(LOAD_M, BAR_Y, bw, BAR_H, {4, 5, 12, 255});
    float shine = fmodf(loadT * 140, filled * 5 + 40.0f) - 20;
    for (int k = 0; k < BAR_SEGS; k++) {
        int x = LOAD_M + 1 + k * 5;
        Color c = Theme::Panel;
        if (k < filled) {
            c = ColorLerp(Theme::Red, Theme::Gold, (float)k / (BAR_SEGS - 1));
            float d = fabsf(k * 5 + 2 - shine);
            if (d < 12) c = ColorLerp(c, WHITE, 0.6f * (1 - d / 12));
        }
        DrawRectangle(x, BAR_Y + 1, 4, BAR_H - 2, c);
        if (k < filled) DrawRectangle(x, BAR_Y + 1, 4, 1, Fade(WHITE, 0.35f));
    }
    const char* pct = TextFormat("CARREGANDO... %d%%", (int)(loadShown * 100 + 0.5f));
    Text(pct, SCREEN_W - LOAD_M - TextW(pct, 10), BAR_Y + BAR_H / 2 - 5, 10, Theme::Ink);

    // mascotes correndo sobre o chão, com sombra e poeira
    if (loadPets) {
        int n = (int)loadPets->list.size();
        for (int k = 0; k < n; k++) {
            const SupportDef& s = loadPets->list[k];
            int x = (int)LoadPetX(k, n);
            DrawEllipse(x, LOAD_GROUND, 10, 2, Fade(BLACK, 0.4f));
            auto it = s.anims.find("run");
            if (it != s.anims.end() && !it->second.frames.empty()) {
                const Anim& an = it->second;
                int fr = (int)(loadT * an.fps) % (int)an.frames.size();
                DrawAnimFrame(an, fr, {(float)x, LOAD_GROUND + an.boxes[fr].sink}, false, false, 1);
            } else {  // sem arte: bolinha com a inicial
                DrawCircle(x, LOAD_GROUND - 8, 8, Theme::Gold);
                Text(TextFormat("%c", s.name[0]), x - 3, LOAD_GROUND - 13, 10, Theme::Bg);
            }
        }
    }
    for (const Dust& d : dust)
        DrawRectangle((int)d.p.x, (int)d.p.y, 1, 1, Fade(Theme::InkDim, 1 - d.age / 0.3f));

    DrawVignette(scanlines);

    if (loadT < 0.3f) {  // entrada: sai do preto e a faixa na cor do fundo atravessa para a direita
        float k = loadT / 0.3f;
        k = k * k * (3 - 2 * k);
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(BLACK, 1 - k));
        Band(-70 + (SCREEN_W + 140) * k, SCREEN_W + 100, 0, SCREEN_H, 60, Theme::Bg);
    }
    if (loadOutT > 0.8f) DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(Theme::Bg, (loadOutT - 0.8f) / 0.2f));
}

// ---------- arena (assets/data/stages.json) ----------

// Fundo = faixa crop_top..crop_bottom da imagem (16:9) reduzida com POINT. Fica PAR_M px maior que a tela de cada
// lado (e na mesma proporção em cima e embaixo): o parallax nunca mostra a borda da imagem.
static const char* STAGES = "assets/data/stages.json";
static const char* BALANCE = "assets/data/balance.json";
const float PAR_M = 8;
const int TV_W = 192, TV_H = 108;  // transmissão da TV: a cena em meia resolução
// Câmera vertical da luta: sobe (o cenário desce) quando o mais alto chega perto do HUD, até CAM_UP_MAX px.
const float CAM_UP_MAX = 100, CAM_TOP = 44;
struct Stage {
    std::string image, music;
    float cropTop = 448, cropBottom = 1600, floorY = 1500, parallax = 0.3f, brightness = 0.7f;
    Rectangle tv{};      // tela da TV em px da imagem original
    float tableRight = 0, tableTop = 0;  // mesa da esquerda (plataforma) em px da imagem: lateral direita e tampo (0 = sem)
    float k = 1, top = 0, left = 0;  // px lógicos por px da imagem; y do topo do fundo na tela; x da imagem na borda esquerda
    bool live = true;    // balance.json "tv_live": false = TV sem transmissão, só o brilho
    Light light;         // luz da luta: "light" da arena por cima da do characters.json
    float vignette = 0, tvGlow = 1;  // escurece cantos e teto (0..1); força do brilho da TV
    float extra = 0;     // px lógicos de imagem acima da faixa da luta (para a câmera subir no pulo)
    Texture2D bg{};
    Music song{};
    // Cenário em camadas com câmera ("layers" no JSON, ex.: Mercadão). Posições em px da imagem do fundo; cada camada
    // anda `parallax` x a câmera (fundo 0,5, animações 0,6, chão 1). O mundo vai de 0 a SCREEN_W + 2 x margin.
    bool layered = false;
    Color skyTop{}, skyBottom{};
    Anim clouds;
    float cloudSpeed = 0.05f;
    std::vector<float> cloudY;
    Texture2D back{}, front{};  // front = grade da cerca (por cima de quem está atrás dela)
    float backK = 1, backW = 0, wallY = 0, wallScreenY = 150, backPar = 0.5f, margin = 0, propPar = 0.6f;
    std::string title;
    Vector2 titleAt{};
    std::vector<Anim> shoppers;
    float shopSpeed = 0.4f, shopEvery[2] = {6, 12};
    int shopFps = 6;
    std::vector<Vector2> path;
    Anim vendor, crowd, pigeons;
    Vector2 vendorAt{}, crowdAt{}, pigeonAt{};
    int vendorFps = 4, crowdFps = 3, hypeFps = 10, hypeTicks = 60, pigeonFps = 6;
    float scare = 60, pigeonBack = 15, pigeonPar = 1;  // pombos no chão da luta: andam com o mundo
    Texture2D floor{};
    float floorTop = 185;
};
// Vida do cenário em camadas (por tick): cliente descendo a rampa, torcida, pombos e nuvens.
struct StageLife {
    int t = 0, shopWait = 0, shopSeg = 0, shopSheet = 0, shopT = 0, hype = 0;
    bool shopOn = false;
    Vector2 shop{};             // pés da cliente, px da imagem do fundo
    int pigeonState = 0, pigeonT = 0;  // 0 no chão, 1 voando, 2 longe (volta depois de pigeonBack s)
    Vector2 pigeonOff{};
    float cloudX[8] = {};
};
static StageLife life;
static Stage stage;
static int stageIdx = 0;  // arena escolhida (índice em stages.json)
static RenderTexture2D tvRT{};
static Vector3 tvColor = {110, 150, 230}, tvTarget = tvColor;  // cor média do que passa na TV (da imagem da sala)
static float tvFlick = 1, tvFlickTo = 1, tvPulse = 0, tvFlash = 0;
static int tvFrame = 0;

// Maior retângulo quase preto (canais < 40) da imagem = tela da TV. Linha a linha, altura das colunas escuras
// + pilha (maior retângulo de um histograma).
static Rectangle FindTvScreen(const Image& img) {
    Image im = ImageCopy(img);
    ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const Color* px = (const Color*)im.data;
    std::vector<int> h(im.width + 1, 0);  // h[width] = 0 fecha a pilha no fim da linha
    std::vector<std::pair<int, int>> st;
    Rectangle best{};
    for (int y = 0; y < im.height; y++) {
        for (int x = 0; x < im.width; x++) {
            Color c = px[y * im.width + x];
            h[x] = std::max({c.r, c.g, c.b}) < 40 ? h[x] + 1 : 0;
        }
        st.clear();
        for (int x = 0; x <= im.width; x++) {
            int s = x;
            while (!st.empty() && st.back().second >= h[x]) {
                auto [s0, hh] = st.back();
                st.pop_back();
                if ((float)hh * (x - s0) > best.width * best.height) best = {(float)s0, (float)(y - hh + 1), (float)(x - s0), (float)hh};
                s = s0;
            }
            st.push_back({s, h[x]});
        }
    }
    UnloadImage(im);
    return best;
}

static void UnloadStage() {
    for (Texture2D t : {stage.back, stage.front, stage.floor})
        if (t.id) UnloadTexture(t);
    for (Anim* a : {&stage.clouds, &stage.vendor, &stage.crowd, &stage.pigeons}) UnloadAnim(*a);
    for (Anim& a : stage.shoppers) UnloadAnim(a);
    if (stage.bg.id) UnloadTexture(stage.bg);
    if (IsMusicValid(stage.song)) UnloadMusicStream(stage.song);
    stage.bg = {};
    stage.song = {};
}

// Animação de cenário (cliente, vendedor, torcida, pombos, nuvens): grade rows x cols, pés como ponto fixo, "height"
// (corpo mais alto) ou "pixel_scale".
static Anim StageAnim(const nlohmann::ordered_json& a) {
    AnimOptions o;
    o.fps = a.value("fps", 6);
    o.layout.assign(std::max(1, a.value("rows", 1)), std::max(1, a.value("cols", 1)));
    o.feetAnchor = true;
    o.pixelScale = a.value("pixel_scale", 0.0f);
    std::vector<std::string> files = a.value("files", std::vector<std::string>{});
    if (files.empty()) files.push_back(a.value("file", std::string()));
    return LoadAnim(files, 1, a.value("height", 40), roster.chromaTolerance, o);
}

static Vector2 ReadXY(const nlohmann::ordered_json& j, const char* key) {
    std::vector<float> v = j.value(key, std::vector<float>{0, 0});
    return v.size() >= 2 ? Vector2{v[0], v[1]} : Vector2{};
}
static Color ReadColor(const nlohmann::ordered_json& j, const char* key, Color c) {
    std::vector<int> v = j.value(key, std::vector<int>{});
    return v.size() >= 3 ? Color{(unsigned char)v[0], (unsigned char)v[1], (unsigned char)v[2], 255} : c;
}

// "layers" do stages.json: céu, fundo (sem o magenta de baixo; escala = width_scale x a tela; o muro em wall_y da
// imagem fica em wall_screen_y), animações e chão em tile. Imagem que falta: a camada não aparece.
static void LoadLayers(Stage& s, const nlohmann::ordered_json& L) {
    using oj = nlohmann::ordered_json;
    int tol = roster.chromaTolerance;
    s.layered = true;
    const oj& sky = L.value("sky", oj::object());
    s.skyTop = ReadColor(sky, "top", {96, 160, 232, 255});
    s.skyBottom = ReadColor(sky, "bottom", {200, 226, 248, 255});
    if (sky.contains("clouds")) s.clouds = StageAnim(sky["clouds"]);
    s.cloudSpeed = sky.value("cloud_speed", s.cloudSpeed);
    s.cloudY = sky.value("cloud_y", std::vector<float>{20, 44, 30});
    const oj& bg = L.value("background", oj::object());
    std::string file = bg.value("file", std::string());
    s.backPar = bg.value("parallax", s.backPar);
    s.backW = SCREEN_W * std::max(1.0f, bg.value("width_scale", 1.4f));
    s.wallY = bg.value("wall_y", 0.0f);
    s.wallScreenY = bg.value("wall_screen_y", s.wallScreenY);
    s.title = bg.value("title", std::string());
    s.titleAt = ReadXY(bg, "title_at");
    s.brightness = std::clamp(bg.value("brightness", 1.0f), 0.0f, 1.0f);
    // a câmera vai até a borda do fundo: (largura do fundo - tela) / 2 de cada lado, dividido pelo parallax
    s.margin = (s.backW - SCREEN_W) / 2 / std::max(0.05f, s.backPar);
    if (Image img = FileExists(file.c_str()) ? LoadImage(file.c_str()) : Image{}; img.data) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        RemoveBackground(img, tol);
        s.backK = s.backW / img.width;
        ImageResizeNN(&img, (int)roundf(s.backW), (int)roundf(img.height * s.backK));
        s.back = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    file = bg.value("front", std::string());
    if (Image img = FileExists(file.c_str()) ? LoadImage(file.c_str()) : Image{}; img.data) {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        RemoveBackground(img, tol);
        ImageResizeNN(&img, (int)roundf(img.width * s.backK), (int)roundf(img.height * s.backK));
        s.front = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    s.propPar = L.value("props_parallax", s.propPar);
    if (L.contains("shopper")) {
        const oj& a = L["shopper"];
        std::vector<std::string> files = a.value("files", std::vector<std::string>{a.value("file", std::string())});
        for (const std::string& f : files) {  // variações: uma sheet cada, sorteada a cada cliente
            oj one = a;
            one["files"] = {f};
            s.shoppers.push_back(StageAnim(one));
        }
        s.shopSpeed = a.value("speed", s.shopSpeed);
        s.shopFps = a.value("fps", s.shopFps);
        std::vector<float> ev = a.value("every", std::vector<float>{6, 12});
        if (ev.size() >= 2) s.shopEvery[0] = ev[0], s.shopEvery[1] = std::max(ev[0], ev[1]);
        for (const auto& pt : a.value("path", oj::array())) s.path.push_back({pt[0].get<float>(), pt[1].get<float>()});
    }
    if (L.contains("vendor")) { s.vendor = StageAnim(L["vendor"]); s.vendorAt = ReadXY(L["vendor"], "at"); s.vendorFps = L["vendor"].value("fps", 4); }
    if (L.contains("crowd")) {
        const oj& a = L["crowd"];
        s.crowd = StageAnim(a);
        s.crowdAt = ReadXY(a, "at");
        s.crowdFps = a.value("fps", 3);
        s.hypeFps = a.value("hype_fps", 10);
        s.hypeTicks = a.value("hype_ticks", 60);
    }
    if (L.contains("pigeons")) {
        const oj& a = L["pigeons"];
        s.pigeons = StageAnim(a);
        s.pigeonAt = ReadXY(a, "at");
        s.pigeonFps = a.value("fps", 6);
        s.scare = a.value("scare", s.scare);
        s.pigeonBack = a.value("return", s.pigeonBack);
        s.pigeonPar = a.value("parallax", s.pigeonPar);
    }
    const oj& fl = L.value("floor", oj::object());
    s.floorTop = fl.value("top_y", s.floorTop);
    file = fl.value("file", std::string());
    if (Image img = FileExists(file.c_str()) ? LoadImage(file.c_str()) : Image{}; img.data) {  // opaca: só a calçada
        float k = fl.value("scale", 0.25f);
        ImageResizeNN(&img, (int)roundf(img.width * k), (int)roundf(img.height * k));
        s.floor = LoadTextureFromImage(img);
        UnloadImage(img);
    }
    s.extra = CAM_UP_MAX;  // o céu é desenhado: a câmera vertical pode subir tudo
}

// Arena 1 do stages.json + "tv_live" do balance.json. Sem "tv" no JSON: detecta na imagem e grava (editável).
static void LoadStage() {
    UnloadStage();
    Stage s;
    nlohmann::ordered_json all = nlohmann::ordered_json::object(), j = nlohmann::ordered_json::object();  // grava na ordem do arquivo
    try {
        std::ifstream f(STAGES);
        if (f) all = nlohmann::ordered_json::parse(f);
        if (all.contains("stages") && !all["stages"].empty()) j = all["stages"][std::min<size_t>(stageIdx, all["stages"].size() - 1)];
        s.image = j.value("image", std::string());
        s.music = j.value("music", std::string());
        s.cropTop = j.value("crop_top", s.cropTop);
        s.cropBottom = j.value("crop_bottom", s.cropBottom);
        s.floorY = j.value("floor_y", s.floorY);
        s.parallax = j.value("parallax", s.parallax);
        s.brightness = std::clamp(j.value("brightness", s.brightness), 0.0f, 1.0f);
        if (j.contains("table")) {
            s.tableRight = j["table"].value("right", 0.0f);
            s.tableTop = j["table"].value("top_y", 0.0f);
        }
        if (j.contains("tv")) s.tv = {j["tv"].value("x", 0.0f), j["tv"].value("y", 0.0f), j["tv"].value("w", 0.0f), j["tv"].value("h", 0.0f)};
        s.light = roster.light;
        if (j.contains("light")) {
            const auto& l = j["light"];
            if (l.contains("sun")) s.light.side = l["sun"] == "left" ? -1 : 1;
            s.light.shadowLength = l.value("shadow_length", s.light.shadowLength);
            s.light.shadowAlpha = std::clamp(l.value("shadow_alpha", s.light.shadowAlpha), 0.0f, 1.0f);
            s.light.shade = l.value("shade", s.light.shade);
            s.light.warm = l.value("warm", s.light.warm);
            s.light.rim = l.value("rim", s.light.rim);
        }
        s.vignette = std::clamp(j.value("vignette", 0.0f), 0.0f, 1.0f);
        s.tvGlow = std::max(0.0f, j.value("tv_glow", 1.0f));
        if (j.contains("layers")) {
            LoadLayers(s, j["layers"]);
            s.floorY = j.value("floor_y", 196.0f);
        }
        std::ifstream b(BALANCE);
        if (b) s.live = nlohmann::json::parse(b).value("tv_live", true);
    } catch (const std::exception& e) {
        Say(std::string("Erro na arena: ") + e.what());
    }
    Image img = !s.image.empty() && FileExists(s.image.c_str()) ? LoadImage(s.image.c_str()) : Image{};
    if (img.data) {
        if (s.tv.width <= 0 && !j.empty()) {
            s.tv = FindTvScreen(img);
            all["stages"][std::min<size_t>(stageIdx, all["stages"].size() - 1)]["tv"] = {{"x", (int)s.tv.x}, {"y", (int)s.tv.y}, {"w", (int)s.tv.width}, {"h", (int)s.tv.height}};
            std::ofstream(STAGES) << all.dump(2) << "\n";
            TraceLog(LOG_INFO, "ARENA: tela da TV detectada em %d,%d %dx%d (gravada no stages.json)", (int)s.tv.x,
                     (int)s.tv.y, (int)s.tv.width, (int)s.tv.height);
        }
        // faixa crop_top..crop_bottom em 16:9, centrada na horizontal (faixa mais baixa = mais zoom)
        float ch = std::max(1.0f, std::min(s.cropBottom, (float)img.height) - s.cropTop);
        float cw = std::min((float)img.width, ch * SCREEN_W / SCREEN_H);
        s.left = (img.width - cw) / 2;
        int w = (int)(SCREEN_W + 2 * PAR_M), h = (int)roundf(w * ch / cw);
        s.k = w / cw;
        s.top = floorf((SCREEN_H - h) / 2.0f);
        float up = std::min(s.cropTop, ceilf(CAM_UP_MAX / s.k));  // imagem acima da faixa: a câmera sobe no pulo
        s.extra = roundf(up * s.k);
        ImageCrop(&img, {s.left, s.cropTop - up, cw, ch + up});
        ImageResizeNN(&img, w, h + (int)s.extra);
        s.bg = LoadTextureFromImage(img);
        // cor média do que a TV mostra (a própria sala, escurecida e com o tom azulado da transmissão): calculada
        // uma vez aqui, sem ler a textura da placa de vídeo a cada quadro (isso travava a CPU esperando a GPU)
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        const Color* px = (const Color*)img.data;
        float r = 0, g = 0, b = 0, n = 0;
        for (int i = 0; i < img.width * img.height; i += 7, n++) r += px[i].r, g += px[i].g, b += px[i].b;
        float v = s.brightness / n;
        tvColor = tvTarget = {r * v * 195 / 255, g * v * 215 / 255, b * v};
        UnloadImage(img);
    }
    if (!s.music.empty() && FileExists(s.music.c_str())) s.song = LoadMusicStream(s.music.c_str());
    stage = s;
    GROUND_Y = stage.layered ? (int)stage.floorY : stage.bg.id ? (int)roundf(stage.top + (stage.floorY - stage.cropTop) * stage.k) : 200;
    arena = {};  // mundo = a tela; cenário em camadas: a tela + margem de cada lado (até a borda do fundo)
    if (stage.layered) arena.x1 = SCREEN_W + 2 * stage.margin;
    life = {};
    life.shopWait = 60;
    for (size_t i = 0; i < 8; i++) life.cloudX[i] = 40 + i * 150.0f;  // nuvens espalhadas
}

static bool HasArena() { return stage.bg.id || stage.layered; }

// Parallax: o fundo anda `parallax` x o deslocamento do meio dos lutadores (a "câmera"), suavizado até PAR_M.
static float ArenaShift(float alpha) {
    float mid = 0;
    for (const Fighter& f : p) mid += (f.prevX + (f.x - f.prevX) * alpha) / 2;
    return -PAR_M * tanhf(stage.parallax * (mid - SCREEN_W / 2.0f) / PAR_M);
}

// Mesa (plataforma) em px lógicos, acompanhando o parallax do fundo; chamada a cada tick.
static void UpdatePlatform() {
    if (!stage.bg.id || stage.tableTop <= 0) { platform = {}; return; }
    platform.x1 = ArenaShift(1) - PAR_M + (stage.tableRight - stage.left) * stage.k;
    platform.top = GROUND_Y - (stage.top + (stage.tableTop - stage.cropTop) * stage.k);
}

// ---------- escolha de arena ----------
// Depois do suporte (sem mascotes: depois dos lutadores). Esquerda: lista de faixas inclinadas com uma tira da arena
// e o nome (arenas do stages.json, vagas "EM BREVE" até STAGE_ROWS e ALEATÓRIO por último); a escolhida cresce, vai
// para a direita, ganha borda dourada e a etiqueta 1P. Direita: prévia grande com um jogo de câmeras (geral
// fechando devagar, close na TV, panorâmica no chão; 2,4 s cada, corte com flash), scanlines, cantos de visor,
// "CAM N" e REC piscando. Só o P1 escolhe; confirmar carrega a arena e vai para o VS.
struct StageInfo { std::string name; Texture2D preview{}; float k = 1; Rectangle crop{}, tv{}; };
static std::vector<StageInfo> stageList;
static int stageCursor = 0;
static double stageCamAt = 0;  // a câmera da prévia recomeça ao trocar o cursor
const int STAGE_ROWS = 5, PREVIEW_PX = 768;  // prévia: imagem inteira reduzida (a faixa da luta fica ~700 px)
const float SHOT_TIME = 2.4f;
static const Rectangle PREVIEW = {146, 41, 230, 129};

static const char* StageName() { return stageList.empty() ? "SALA" : stageList[stageIdx].name.c_str(); }

// Prévia do cenário em camadas: o fundo sobre a cor do céu, com o chão em tile embaixo do muro. crop = o que a luta
// mostra com a câmera no meio; "TV" do close = a torcida.
static Image LayeredPreview(const nlohmann::json& L, StageInfo& s) {
    nlohmann::json bg = L.value("background", nlohmann::json::object()), fl = L.value("floor", nlohmann::json::object());
    std::string file = bg.value("file", std::string()), ffile = fl.value("file", std::string());
    if (!FileExists(file.c_str())) return {};
    Image fg = LoadImage(file.c_str());
    ImageFormat(&fg, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    RemoveBackground(fg, roster.chromaTolerance);
    std::vector<int> c = L.value("sky", nlohmann::json::object()).value("bottom", std::vector<int>{200, 226, 248});
    Image img = GenImageColor(fg.width, fg.height, {(unsigned char)c[0], (unsigned char)c[1], (unsigned char)c[2], 255});
    float k = SCREEN_W * bg.value("width_scale", 1.4f) / fg.width, wy = bg.value("wall_y", 0.0f), wsy = bg.value("wall_screen_y", 150.0f);
    if (FileExists(ffile.c_str())) {  // chão: mesma escala da luta, convertida para px da imagem do fundo
        Image f = LoadImage(ffile.c_str());
        ImageFormat(&f, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        float fk = fl.value("scale", 0.25f) / k;
        ImageResizeNN(&f, std::max(1, (int)(f.width * fk)), std::max(1, (int)(f.height * fk)));
        float fy = wy + (fl.value("top_y", 185.0f) - wsy) / k;
        for (int x = 0; x < img.width; x += f.width) ImageDraw(&img, f, {0, 0, (float)f.width, (float)f.height}, {(float)x, fy, (float)f.width, (float)f.height}, WHITE);
        UnloadImage(f);
    }
    ImageDraw(&img, fg, {0, 0, (float)fg.width, (float)fg.height}, {0, 0, (float)fg.width, (float)fg.height}, WHITE);
    UnloadImage(fg);
    float cw = SCREEN_W / k, ch = SCREEN_H / k;
    s.crop = {(img.width - cw) / 2, std::max(0.0f, wy - wsy / k), cw, ch};
    std::vector<float> at = L.value("crowd", nlohmann::json::object()).value("at", std::vector<float>{img.width / 2.0f, wy});
    s.tv = {at[0], at[1] - 60, 1, 1};
    return img;
}

// Nome, faixa da luta e TV de cada arena + a prévia (imagem inteira com POINT). Sem imagem: só o nome.
static void LoadStageList() {
    for (StageInfo& s : stageList)
        if (s.preview.id) UnloadTexture(s.preview);
    stageList.clear();
    try {
        std::ifstream f(STAGES);
        nlohmann::json all = f ? nlohmann::json::parse(f) : nlohmann::json::object();
        for (const auto& j : all.value("stages", nlohmann::json::array())) {
            StageInfo s;
            s.name = j.value("name", j.value("id", std::string("ARENA")));
            std::string file = j.value("image", std::string());
            Image img = !file.empty() && FileExists(file.c_str()) ? LoadImage(file.c_str()) : Image{};
            if (!img.data && j.contains("layers")) img = LayeredPreview(j["layers"], s);  // cenário em camadas
            else if (img.data) {
                float top = j.value("crop_top", 0.0f), ch = std::min(j.value("crop_bottom", (float)img.height), (float)img.height) - top;
                float cw = std::min((float)img.width, ch * SCREEN_W / SCREEN_H);
                s.crop = {(img.width - cw) / 2, top, cw, ch};
                if (j.contains("tv")) s.tv = {j["tv"].value("x", 0.0f), j["tv"].value("y", 0.0f), j["tv"].value("w", 0.0f), j["tv"].value("h", 0.0f)};
            }
            if (img.data) {
                s.k = (float)PREVIEW_PX / img.width;
                ImageResizeNN(&img, PREVIEW_PX, (int)roundf(img.height * s.k));
                s.preview = LoadTextureFromImage(img);
                UnloadImage(img);
            }
            stageList.push_back(s);
        }
    } catch (const std::exception& e) {
        Say(std::string("Erro na arena: ") + e.what());
    }
    if (stageList.empty()) stageList.push_back({"SALA"});
    stageIdx = std::min(stageIdx, (int)stageList.size() - 1);
}

static int StageRows() { return std::max(STAGE_ROWS, (int)stageList.size() + 1); }
static bool IsRandomRow(int r) { return r == StageRows() - 1; }

static void OpenStageSelect() {
    selectStage = 2;
    stageCursor = stageIdx;
    selectAt = stageCamAt = GetTime();
    readyAt = 0;
}

// W/S (ou ↑/↓, direcional) movem; J confirma (EM BREVE não confirma); K/Esc volta aos suportes (sem mascotes, aos lutadores).
static void UpdateStageSelect() {
    if (KeyBack() || Press(0, KEY_K, PAD_BACK) || Press(1, KEY_KP_2, PAD_BACK)) {
        if (!roster.supports.list.empty()) GoTo([] { selectStage = 1; petPick[0] = petPick[1] = -1; readyAt = 0; selectAt = GetTime(); });
        else GoTo([] { selectStage = 0; pick[0] = pick[1] = -1; readyAt = 0; });
        return;
    }
    if (readyAt != 0) return;  // já confirmou: a cortina leva ao VS
    int n = StageRows(), d = KeyDown() - KeyUp();
    if (d) {
        stageCursor = (stageCursor + d + n) % n;
        stageCamAt = GetTime();
        PlaySfx("cursor");
    }
    bool locked = !IsRandomRow(stageCursor) && stageCursor >= (int)stageList.size();
    if (KeyOk() && !locked) {
        PlaySfx("confirmar");
        readyAt = GetTime();
        int k = IsRandomRow(stageCursor) ? GetRandomValue(0, (int)stageList.size() - 1) : stageCursor;
        GoTo([k] {
            if (k != stageIdx || !stage.bg.id) { stageIdx = k; LoadStage(); }
            OpenVersus();
        });
    }
}

// Janela 16:9 da câmera (px da imagem) no tempo t da prévia; shot = tomada atual.
static Rectangle StageCam(const StageInfo& s, float t, int& shot) {
    shot = (int)(t / SHOT_TIME) % 3;
    float u = fmodf(t, SHOT_TIME) / SHOT_TIME, e = u * u * (3 - 2 * u);
    Rectangle c = s.crop;
    Vector2 tv = s.tv.width > 0 ? Vector2{s.tv.x + s.tv.width / 2, s.tv.y + s.tv.height / 2} : Vector2{c.x + c.width / 2, c.y + c.height * 0.4f};
    struct { Vector2 a, b; float wa, wb; } k[3] = {
        {{c.x + c.width / 2, c.y + c.height / 2}, {c.x + c.width / 2, c.y + c.height / 2}, 1.0f, 0.86f},  // geral fechando
        {{tv.x - c.width * 0.04f, tv.y}, {tv.x + c.width * 0.04f, tv.y}, 0.42f, 0.36f},                  // close na TV
        {{c.x + c.width * 0.3f, c.y + c.height * 0.68f}, {c.x + c.width * 0.7f, c.y + c.height * 0.68f}, 0.5f, 0.5f},  // chão
    };
    float w = c.width * (k[shot].wa + (k[shot].wb - k[shot].wa) * e), h = w * SCREEN_H / SCREEN_W;
    float iw = s.preview.width / s.k, ih = s.preview.height / s.k;
    float x = std::clamp(k[shot].a.x + (k[shot].b.x - k[shot].a.x) * e - w / 2, 0.0f, iw - w);
    float y = std::clamp(k[shot].a.y + (k[shot].b.y - k[shot].a.y) * e - h / 2, 0.0f, ih - h);
    return {x, y, w, h};
}

// Recorte `win` (px da imagem) da prévia esticado no retângulo box, cortado pelo quadrilátero q.
static void DrawStageWindow(const StageInfo& s, Rectangle win, Rectangle box, const Vector2 q[4], Color tint) {
    float sc = box.width / (win.width * s.k);
    TexturedQuad(s.preview, {box.x - win.x * s.k * sc, box.y - win.y * s.k * sc, s.preview.width * sc, s.preview.height * sc}, q, tint, false);
}

static void DrawStatic(Rectangle r, float dim) {  // chuvisco de TV em blocos de 3 px
    for (float y = r.y; y < r.y + r.height; y += 3)
        for (float x = r.x; x < r.x + r.width; x += 3) {
            unsigned char v = (unsigned char)(GetRandomValue(20, 120) * dim);
            DrawRectangle((int)x, (int)y, 3, 3, {v, v, (unsigned char)std::min(255, v + 12), 255});
        }
}

static void DrawLock(float x, float y, int px, Color c) {  // cadeado 8x8 em blocos de px
    for (int r = 0; r < 8; r++)
        for (int k = 0; k < 8; k++)
            if (LOCK[r][k] == '#') DrawRectangle((int)x + k * px, (int)y + r * px, px, px, c);
}

static void DrawStageSelect() {
    DrawSelectBackground({8, 24, 40, 255}, {20, 90, 110, 255}, {120, 230, 255, 110}, 105);
    DrawSelectHeader("ESCOLHA A ARENA");
    float t = (float)(GetTime() - selectAt), now = (float)GetTime();
    int n = StageRows(), ns = (int)stageList.size();
    bool confirmed = readyAt != 0;

    // lista: faixas inclinadas em cascata pela esquerda
    const float X0 = 26, W = 100, H = 24, GAP = 5, SL = 6, Y0 = roundf(110 - (n * (H + GAP) - GAP) / 2.0f);
    for (int r = 0; r < n; r++) {
        bool sel = r == stageCursor, locked = r >= ns && !IsRandomRow(r);
        float e = EaseOut((t - r * 0.05f) / 0.25f), x = roundf(X0 + (sel ? 6 : 0) - (1 - e) * 160), y = Y0 + r * (H + GAP);
        float w = W + (sel ? 8 : 0);
        Band(x - 2, x + w + 2, y - 2, H + 4, SL, INK_DARK);
        if (sel) Band(x - 1, x + w + 1, y - 1, H + 2, SL, confirmed && fmodf(now * 10, 2) < 1 ? WHITE : Theme::Gold);
        Vector2 q[4] = {{x + SL, y}, {x + w + SL, y}, {x + w, y + H}, {x, y + H}};
        Color tint = sel ? WHITE : Color{120, 120, 140, 255};
        const char* label = r < ns ? stageList[r].name.c_str() : IsRandomRow(r) ? "ALEATÓRIO" : "EM BREVE";
        if (r < ns && stageList[r].preview.id) {  // tira da arena um pouco abaixo do meio (móveis e chão)
            const StageInfo& s = stageList[r];
            float ww = s.crop.width * 0.7f, wh = ww * H / (w + SL), cy = s.crop.y + s.crop.height * 0.62f;
            DrawStageWindow(s, {s.crop.x + s.crop.width * 0.15f, cy - wh / 2, ww, wh}, {x, y, w + SL, H}, q, tint);
        } else {
            FillQuad(q, IsRandomRow(r) ? Color{40, 36, 70, 255} : Theme::Panel);
            if (IsRandomRow(r))
                for (int k = 0; k < 3; k++)
                    DrawDisplayText("?", {x + w - 32 + k * 9, y + 3 + (k % 2) * 3}, 2, Grad::Gold, false, 1, sel ? 1 : 0.5f);
            else if (locked)
                DrawLock(x + w - 14, y + 4, 1, Fade(Theme::InkDim, sel ? 1 : 0.5f));
        }
        Band(x + 1, x + 10 + MeasureUIText(label, 1), y + H - 11, 10, 2, Fade(INK_DARK, 0.75f));  // nome embaixo
        DrawUIText(label, {x + 5, y + H - 10}, 1, locked ? Theme::InkDim : sel ? Theme::Gold : Theme::Ink);
        if (sel && e >= 1) {  // etiqueta 1P à esquerda da escolhida
            Band(x - 24, x - 5, y + 7, 10, 2, Theme::Red);
            DrawUIText("1P", {x - 19, y + 8}, 1, WHITE);
        }
    }

    // prévia: entra pela direita; cada tomada corta com flash
    Rectangle F = PREVIEW;
    F.x = roundf(F.x + (1 - EaseOut(t / 0.35f)) * 260);
    DrawRectangle((int)F.x - 3, (int)F.y - 3, (int)F.width + 6, (int)F.height + 6, INK_DARK);
    DrawRectangle((int)F.x - 1, (int)F.y - 1, (int)F.width + 2, (int)F.height + 2, Theme::Gold);
    Vector2 q[4] = {{F.x, F.y}, {F.x + F.width, F.y}, {F.x + F.width, F.y + F.height}, {F.x, F.y + F.height}};
    float ct = (float)(GetTime() - stageCamAt);
    int shot = 0;
    const char* name = stageCursor < ns ? stageList[stageCursor].name.c_str() : IsRandomRow(stageCursor) ? "ALEATÓRIO" : "EM BREVE";
    if (stageCursor < ns && stageList[stageCursor].preview.id) {
        const StageInfo& s = stageList[stageCursor];
        Rectangle win = StageCam(s, ct, shot);
        DrawStageWindow(s, win, F, q, WHITE);
        if (s.tv.width > 0) {  // TV ligada com chuvisco (na luta ela transmite a briga)
            float sc = F.width / win.width;
            Rectangle tv = GetCollisionRec(F, {F.x + (s.tv.x - win.x) * sc, F.y + (s.tv.y - win.y) * sc, s.tv.width * sc, s.tv.height * sc});
            if (tv.width > 0 && tv.height > 0) DrawStatic(tv, 0.6f);
        }
    } else {
        bool rnd = IsRandomRow(stageCursor);
        DrawStatic(F, rnd ? 1 : 0.45f);
        if (rnd) {
            Vector2 m = MeasureDisplayText("?", 6);
            DrawDisplayText("?", {roundf(F.x + F.width / 2 - m.x / 2), roundf(F.y + F.height / 2 - m.y / 2 - 8)}, 6, Grad::Gold, true);
        } else {
            DrawLock(F.x + F.width / 2 - 16, F.y + 28, 4, Theme::InkDim);
        }
    }
    for (float y = F.y; y < F.y + F.height; y += 2) DrawRectangle((int)F.x, (int)y, (int)F.width, 1, Fade(BLACK, 0.14f));  // scanlines
    float flash = 1 - fmodf(ct, SHOT_TIME) / 0.15f;  // corte entre tomadas (e ao trocar de arena)
    if (flash > 0) DrawRectangleRec(F, Fade(WHITE, 0.6f * flash));
    for (int c = 0; c < 4; c++) {  // cantos do visor
        float cx = c % 2 ? F.x + F.width - 6 : F.x + 5, cy = c / 2 ? F.y + F.height - 6 : F.y + 5;
        DrawRectangle((int)(cx - (c % 2 ? 7 : 0)), (int)cy, 8, 1, Fade(WHITE, 0.8f));
        DrawRectangle((int)cx, (int)(cy - (c / 2 ? 7 : 0)), 1, 8, Fade(WHITE, 0.8f));
    }
    DrawUIText(TextFormat("CAM %d", shot + 1), {F.x + 10, F.y + 9}, 1, WHITE, true);
    if (fmodf(now, 1) < 0.6f) DrawCircle((int)(F.x + F.width - 32), (int)F.y + 13, 2, Theme::Red);
    DrawUIText("REC", {F.x + F.width - 27, F.y + 9}, 1, WHITE, true);
    Vector2 m = MeasureDisplayText(name, 2);  // nome grande embaixo, numa faixa escura inclinada
    Band(F.x - 6, F.x + m.x + 16, F.y + F.height - 24, m.y + 8, 4, Fade(INK_DARK, 0.85f));
    DrawRectangle((int)F.x - 6, (int)(F.y + F.height - 16 + m.y), (int)m.x + 22, 1, Theme::Gold);
    DrawDisplayText(name, {F.x + 4, F.y + F.height - 20}, 2, Grad::White, true);

    // rodapé: teclas
    float fy = 196;
    int kx = 10;
    for (const char* k : {"W", "S"}) DrawKey(kx, (int)fy - 1, k), kx += 12;
    DrawUIText("MOVER", {(float)kx + 2, fy + 1}, 1, Theme::Ink, true);
    int rx = SCREEN_W - 10 - (12 + MeasureUIText("CONFIRMA", 1) + 8 + 12 + MeasureUIText("VOLTA", 1));
    DrawKey(rx, (int)fy - 1, "J");
    DrawUIText("CONFIRMA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
    rx += 13 + MeasureUIText("CONFIRMA", 1) + 7;
    DrawKey(rx, (int)fy - 1, "K");
    DrawUIText("VOLTA", {(float)rx + 13, fy + 1}, 1, Theme::Ink, true);
}

static bool HasTv() { return stage.bg.id && stage.tv.width > 0; }
static Rectangle TvRect(float shift) {
    return {shift - PAR_M + (stage.tv.x - stage.left) * stage.k, stage.top + (stage.tv.y - stage.cropTop) * stage.k,
            stage.tv.width * stage.k, stage.tv.height * stage.k};
}
static float TvPower() { return (0.5f + (tvColor.x + tvColor.y + tvColor.z) / 765) * tvFlick * (1 + tvPulse); }
static Color TvTint(float a) {  // cor da TV com o canal mais forte em 255 (tom, sem escurecer)
    float m = std::max({tvColor.x, tvColor.y, tvColor.z, 1.0f}) / 255;
    return {(unsigned char)(tvColor.x / m), (unsigned char)(tvColor.y / m), (unsigned char)(tvColor.z / m), (unsigned char)(255 * std::clamp(a, 0.0f, 1.0f))};
}

// Perto da TV o lutador pega um toque da cor dela (multiplicado, bem sutil).
static Light TvLight(const Light& light, const Fighter& f) {
    Light l = light;
    if (!HasTv()) return l;
    Rectangle r = TvRect(0);
    float k = std::max(0.0f, 1 - fabsf(f.x - (r.x + r.width / 2)) / 110) * 0.12f * TvPower();
    Color c = TvTint(1);
    l.tint = ColorLerp(WHITE, c, std::min(k, 0.2f));
    return l;
}

// Cenário em camadas: x/y na tela de um ponto da imagem do fundo numa camada com `par` de parallax.
// worldCam = câmera horizontal interpolada da luta (px inteiros da tela); viewPpu = escala do quadro atual
static float viewPpu = 1;
static float PropX(float ix, float par, float cam) { return (SCREEN_W - stage.backW) / 2 + ix * stage.backK - (cam - stage.margin) * par; }
static float PropY(float iy) { return stage.wallScreenY + (iy - stage.wallY) * stage.backK; }
static float Snap(float v) { return roundf(v * viewPpu) / viewPpu; }

// Por tick: cliente desce a rampa (caminho "path") e sai andando para a esquerda; pombos voam se alguém chega perto.
static void UpdateLife() {
    if (!stage.layered) return;
    life.t++;
    if (life.hype > 0) life.hype--;
    if (!life.shopOn && !stage.shoppers.empty() && stage.path.size() >= 2 && --life.shopWait <= 0) {
        life.shopOn = true;
        life.shop = stage.path[0];
        life.shopSeg = 1;
        life.shopT = 0;
        life.shopSheet = GetRandomValue(0, (int)stage.shoppers.size() - 1);
    }
    if (life.shopOn) {
        life.shopT++;
        float step = stage.shopSpeed / stage.backK;  // px da tela por tick -> px da imagem
        if (life.shopSeg < (int)stage.path.size()) {
            Vector2 to = stage.path[life.shopSeg], d = {to.x - life.shop.x, to.y - life.shop.y};
            float len = sqrtf(d.x * d.x + d.y * d.y);
            if (len <= step) { life.shop = to; life.shopSeg++; }
            else life.shop = {life.shop.x + d.x / len * step, life.shop.y + d.y / len * step};
        } else {
            life.shop.x -= step;  // calçada atrás do muro, para a esquerda até sair da tela
            if (PropX(life.shop.x, stage.propPar, arena.cam) < -40) {
                life.shopOn = false;
                life.shopWait = (int)(60 * Rnd(stage.shopEvery[0], stage.shopEvery[1]));
            }
        }
    }
    if (life.pigeonState == 0) {
        for (const Fighter& f : p)
            if (fabsf(f.x - arena.cam - PropX(stage.pigeonAt.x, stage.pigeonPar, arena.cam)) < stage.scare) life.pigeonState = 1, life.pigeonT = 0;
    } else if (life.pigeonState == 1) {  // voam para cima e para a direita, para fora da tela
        life.pigeonT++;
        life.pigeonOff = {life.pigeonOff.x + 1.4f, life.pigeonOff.y - 1.6f};
        if (PropY(stage.pigeonAt.y) + life.pigeonOff.y < -50) life.pigeonState = 2, life.pigeonT = 0;
    } else if (++life.pigeonT >= stage.pigeonBack * 60) {
        life.pigeonState = 0;
        life.pigeonOff = {};
    }
}

static void StageHype() { life.hype = stage.hypeTicks; }

static int LifeFrame(const Anim& a, int ticks, int fps) { return a.boxes.empty() ? 0 : ticks * fps / 60 % (int)a.boxes.size(); }
static void DrawProp(const Anim& a, int frame, Vector2 imgFeet, Vector2 off = {}, bool flip = false, float par = -1) {
    if (a.frames.empty()) return;
    float x = PropX(imgFeet.x, par < 0 ? stage.propPar : par, worldCam) + off.x;
    DrawAnimFrame(a, frame, {Snap(x), Snap(PropY(imgFeet.y) + off.y)}, flip, false, viewPpu);
}

// Camadas, de trás para a frente: céu (gradiente + nuvens, presos à tela), fundo, animações e chão em tile.
static void DrawLayers() {
    float cam = worldCam;
    DrawRectangleGradientV(0, (int)-camY - 1, SCREEN_W, SCREEN_H + 2, stage.skyTop, stage.skyBottom);
    if (int n = (int)stage.clouds.frames.size(); n && !stage.cloudY.empty()) {  // da direita para a esquerda, em loop
        float period = SCREEN_W + 140;
        for (size_t i = 0; i < stage.cloudY.size(); i++) {
            float x = fmodf(i * period / stage.cloudY.size() + 300 - life.t * stage.cloudSpeed, period);
            if (x < 0) x += period;
            DrawAnimFrame(stage.clouds, (int)i % n, {Snap(x - 70), Snap(stage.cloudY[i] - camY)}, false, false, viewPpu);
        }
    }
    unsigned char v = (unsigned char)(255 * stage.brightness);
    if (stage.back.id) DrawTextureV(stage.back, {Snap(PropX(0, stage.backPar, cam)), Snap(PropY(0))}, {v, v, v, 255});
    if (!stage.title.empty()) {  // nome na faixa vermelha, na fonte do jogo (branco, sombra preta)
        int w = MeasureUIText(stage.title.c_str(), 2), h = UITextHeight(2);
        Vector2 c = {roundf(PropX(stage.titleAt.x, stage.backPar, cam) - w / 2.0f), roundf(PropY(stage.titleAt.y) - h / 2.0f)};
        DrawUIText(stage.title.c_str(), {c.x + 1, c.y + 1}, 2, BLACK);
        DrawUIText(stage.title.c_str(), c, 2, WHITE);
    }
    if (life.shopOn) {
        const Anim& a = stage.shoppers[life.shopSheet];
        bool left = life.shopSeg >= (int)stage.path.size() || stage.path[life.shopSeg].x < life.shop.x;  // arte olha para a direita
        DrawProp(a, LifeFrame(a, life.shopT, stage.shopFps), life.shop, {}, left);
    }
    DrawProp(stage.crowd, LifeFrame(stage.crowd, life.t, life.hype > 0 ? stage.hypeFps : stage.crowdFps), stage.crowdAt);
    if (stage.front.id)  // grade da cerca por cima de quem está atrás dela (cliente na calçada, torcida)
        DrawTextureV(stage.front, {Snap(PropX(0, stage.backPar, cam)), Snap(PropY(0))}, {v, v, v, 255});
    DrawProp(stage.vendor, LifeFrame(stage.vendor, life.t, stage.vendorFps), stage.vendorAt);  // na frente do muro
    if (stage.floor.id)  // chão preso ao mundo (parallax 1), em tile infinito
        for (float x = -fmodf(cam, (float)stage.floor.width); x < SCREEN_W; x += stage.floor.width)
            DrawTextureV(stage.floor, {Snap(x), Snap(stage.floorTop)}, WHITE);
    if (life.pigeonState < 2 && stage.pigeons.boxes.size() >= 4) {  // no chão da luta: 1-2 ciscando; 3-4 voando
        int k = LifeFrame(stage.pigeons, life.pigeonState ? life.pigeonT : life.t, life.pigeonState ? 12 : stage.pigeonFps) % 2;
        DrawProp(stage.pigeons, life.pigeonState ? 2 + k : k, stage.pigeonAt, life.pigeonOff, false, stage.pigeonPar);
    }
    if (debug && stage.path.size() >= 2)  // F1: caminho da cliente na rampa
        for (size_t i = 0; i + 1 < stage.path.size(); i++)
            DrawLineEx({PropX(stage.path[i].x, stage.propPar, cam), PropY(stage.path[i].y)},
                       {PropX(stage.path[i + 1].x, stage.propPar, cam), PropY(stage.path[i + 1].y)}, 1, MAGENTA);
}

static void DrawArena(float shift, const Light& light) {
    if (stage.layered) { DrawLayers(); return; }
    if (!stage.bg.id) { DrawStage(light); return; }
    unsigned char v = (unsigned char)(255 * stage.brightness);  // fundo escurecido: os lutadores se destacam
    DrawTextureV(stage.bg, {shift - PAR_M, stage.top - stage.extra}, {v, v, v, 255});
    if (stage.vignette > 0) {  // teto e laterais mais escuros (presos à tela, não à câmera): o olho vai para o meio
        Color dark = Fade(BLACK, stage.vignette), none = Fade(BLACK, 0);
        int y0 = (int)-camY;
        DrawRectangleGradientV(0, y0, SCREEN_W, SCREEN_H / 3, dark, none);
        DrawRectangleGradientH(0, y0, SCREEN_W / 5, SCREEN_H, dark, none);
        DrawRectangleGradientH(SCREEN_W - SCREEN_W / 5, y0, SCREEN_W / 5, SCREEN_H, none, dark);
    }
}

// Lutadores, mascotes e efeitos (a luta e a transmissão da TV).
static void DrawFightScene(float alpha, float ppu, const Light& light, bool dbg) {
    // quem está apanhando fica na frente, para o corpo inteiro aparecer
    bool p0Hurt = p[0].act == Act::Hit || p[0].act == Act::KO;
    bool p1Hurt = p[1].act == Act::Hit || p[1].act == Act::KO;
    int front = p0Hurt && !p1Hurt ? 0 : 1;
    for (const Fighter& f : p) {  // sombras ficam embaixo de todos
        DrawFighter(f, false, alpha, ppu, light, true);
        DrawPet(f, false, alpha, ppu, light, true);
    }
    DrawFighter(p[1 - front], dbg, alpha, ppu, TvLight(light, p[1 - front]), false);
    DrawFighter(p[front], dbg, alpha, ppu, TvLight(light, p[front]), false);
    for (const Fighter& f : p) DrawPet(f, dbg, alpha, ppu, light, false);
    for (const Fighter& f : p) DrawEffects(f, dbg, alpha, ppu);
}

// Antes do desenho da tela: a cena (fundo + lutadores, sem HUD) vai para a textura da TV a cada 2 quadros (a TV
// mostra a transmissão; ela mesma não entra, então não há recursão) e a cor média dela guia o brilho.
static void UpdateTv(float alpha, const Light& light) {
    if (!HasTv()) return;
    float dt = std::min(GetFrameTime(), 0.1f);
    tvPulse = std::max(0.0f, tvPulse - dt * 3);
    tvFlash -= dt;
    if (GetRandomValue(0, 5) == 0) tvFlickTo = Rnd(0.8f, 1.0f);  // tremulação de TV de verdade
    tvFlick += (tvFlickTo - tvFlick) * 0.3f;
    float k = std::min(1.0f, dt * 6);
    tvColor = {tvColor.x + (tvTarget.x - tvColor.x) * k, tvColor.y + (tvTarget.y - tvColor.y) * k,
               tvColor.z + (tvTarget.z - tvColor.z) * k};
    if (!HasTv() || !stage.live || tvFrame++ % 3) return;  // transmissão a 20 quadros/s (TV antiga; metade do custo de 30)
    if (!tvRT.id) {
        tvRT = LoadRenderTexture(TV_W, TV_H);
        SetTextureFilter(tvRT.texture, TEXTURE_FILTER_BILINEAR);  // imagem de TV, levemente suave
    }
    float ppu = TV_W / (float)SCREEN_W;
    Light tv = light;  // na tela pequena luz e sombra nem aparecem: desenha sem elas (bem mais barato)
    tv.shade = tv.warm = tv.rim = tv.shadowAlpha = 0;
    BeginTextureMode(tvRT);
    ClearBackground(BLACK);
    BeginMode2D({{0, 0}, {0, 0}, 0, ppu});
    DrawArena(roundf(ArenaShift(alpha) * ppu) / ppu, tv);
    DrawFightScene(alpha, ppu, tv, false);
    EndMode2D();
    EndTextureMode();
}

static void TvHit() { tvFlash = 2 / 60.0f; tvPulse = 1; }  // golpe forte: flash branco de 2 quadros e o halo pulsa

// Brilho (aditivo, ~15%: halo em volta da tela + feixe em trapézio no chão) e a tela: transmissão com tom azulado,
// scanlines e cantos escuros; flash, estática e "K.O." piscando no K.O.; "▌▌" na pausa.
static void DrawTv(float shift, float ppu) {
    if (!HasTv()) return;
    Rectangle r = TvRect(shift);
    float a = 0.15f * TvPower() * stage.tvGlow;
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 1; i <= 4; i++)
        DrawRectangleRounded({r.x - 2.0f * i, r.y - 2.0f * i, r.width + 4.0f * i, r.height + 4.0f * i}, 0.25f, 4, TvTint(a * 0.45f * (1 - i / 5.0f)));
    float y0 = r.y + r.height, cx = r.x + r.width / 2;
    rlDisableBackfaceCulling();
    Quad({cx - r.width * 0.5f, y0}, {cx - r.width * 1.1f, (float)SCREEN_H}, {cx + r.width * 1.1f, (float)SCREEN_H},
         {cx + r.width * 0.5f, y0}, TvTint(a), TvTint(0));
    rlDrawRenderBatchActive();
    EndBlendMode();
    if (stage.live && tvRT.id) DrawTexturePro(tvRT.texture, {0, 0, (float)TV_W, -(float)TV_H}, r, {0, 0}, 0, {195, 215, 255, 255});
    if (koT >= 0 && koT < 0.5f)  // chiado de estática
        for (float y = r.y; y < r.y + r.height; y += 1)
            for (float x = r.x; x < r.x + r.width; x += 1) {
                unsigned char v = (unsigned char)GetRandomValue(30, 230);
                DrawRectangleRec({x, y, 1, 1}, {v, v, v, 255});
            }
    if (tvFlash > 0) DrawRectangleRec(r, WHITE);
    for (float y = r.y; y < r.y + r.height; y += 2 / ppu) DrawRectangleRec({r.x, y, r.width, 1 / ppu}, Fade(BLACK, 0.25f));
    float e = std::min(r.width, r.height) * 0.25f;  // cantos levemente escurecidos
    DrawRectangleGradientEx({r.x, r.y, e, r.height}, Fade(BLACK, 0.45f), Fade(BLACK, 0.45f), BLANK, BLANK);
    DrawRectangleGradientEx({r.x + r.width - e, r.y, e, r.height}, BLANK, BLANK, Fade(BLACK, 0.45f), Fade(BLACK, 0.45f));
    DrawRectangleGradientEx({r.x, r.y, r.width, e}, Fade(BLACK, 0.35f), BLANK, BLANK, Fade(BLACK, 0.35f));
    DrawRectangleGradientEx({r.x, r.y + r.height - e, r.width, e}, BLANK, Fade(BLACK, 0.35f), Fade(BLACK, 0.35f), BLANK);
    if (koT >= 0 && fmodf(koT, 0.5f) < 0.3f) {
        int w = MeasureUIText("K.O.", 2);
        DrawUIText("K.O.", {roundf(r.x + (r.width - w) / 2), roundf(r.y + (r.height - UITextHeight(2)) / 2)}, 2, {255, 70, 60, 255}, true);
    }
    if (paused) {  // ▌▌ no canto
        DrawRectangleRec({roundf(r.x + r.width - 10), roundf(r.y + 4), 2, 6}, WHITE);
        DrawRectangleRec({roundf(r.x + r.width - 6), roundf(r.y + 4), 2, 6}, WHITE);
    }
}

// Música dos menus (assets/audio/musica_menu.mp3, em loop): menu inicial, escolha, VS e opções abertas do menu.
// Entra e sai em 0,5 s (some durante o VS, antes da luta) e recomeça do início ao voltar. Volume = MÚSICA das opções.
static const char* MENU_MUSIC = "assets/audio/musica_menu.mp3";
static Music menuMusic{};
static float musicGain = 0, stageGain = 0;

static void FadeMusic(Music& m, float& gain, bool on) {
    if (!IsMusicValid(m)) return;
    float dt = std::min(GetFrameTime(), 0.1f);
    gain = std::clamp(gain + (on ? dt : -dt) / 0.5f, 0.0f, 1.0f);
    bool playing = IsMusicStreamPlaying(m);
    if (gain > 0 && !playing) PlayMusicStream(m);
    if (gain <= 0 && playing) StopMusicStream(m);
    SetMusicVolume(m, gain * opt.music / 100.0f);
    UpdateMusicStream(m);
}

// Música da arena ("music" no stages.json): na luta e nas opções abertas da pausa, do mesmo jeito.
static void UpdateMusic() {
    bool menus = screen == Screen::Title || screen == Screen::Select || screen == Screen::Versus ||
                 (screen == Screen::Options && optionsFrom == Screen::Title);
    bool fight = screen == Screen::Fight || (screen == Screen::Options && optionsFrom == Screen::Fight);
    FadeMusic(menuMusic, musicGain, menus);
    FadeMusic(stage.song, stageGain, fight);
}

static void UpdateDrawFrame() {
#ifdef PLATFORM_WEB
    double cw, ch;  // canvas na resolução real da tela (css x devicePixelRatio): zoom inteiro maior, nítido
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    int ww = (int)(cw * emscripten_get_device_pixel_ratio()), wh = (int)(ch * emscripten_get_device_pixel_ratio());
    if (ww > 0 && wh > 0 && (ww != GetScreenWidth() || wh != GetScreenHeight())) SetWindowSize(ww, wh);
#endif
    static double fpsCheck = 0;  // a cada 2 s: janela foi para outro monitor? (consultar o monitor é lento no Windows)
    if (GetTime() - fpsCheck > 2) { fpsCheck = GetTime(); ApplyFrameRate(false); }
    ReadPads();
    UpdateMusic();
    if (IsKeyPressed(KEY_F1)) { debug = !debug; SaveOptions(); }
    if (IsKeyPressed(KEY_F2)) { cpu = !cpu; Say(cpu ? "CPU ligada no P2" : "CPU desligada"); }
    if (IsKeyPressed(KEY_F5) && screen != Screen::Loading) Reload();
    bool fighting = loaded && screen == Screen::Fight && !paused && transT < 0;
    if (IsKeyPressed(KEY_R) && fighting) { PlaySfx("rewind"); StartFight(); }
    ReadInput();
    // depois do ReadInput: a luta nova zera o J que confirmou, ninguém começa socando. Na cortina, ninguém comanda.
    if (screen == Screen::Loading) Loading_Update();
    else if (loaded && transT < 0) {
        if (screen == Screen::Title) UpdateTitle();
        else if (screen == Screen::Select) {
            if (selectStage == 0) UpdateSelect();
            else if (selectStage == 1) UpdatePetSelect();
            else UpdateStageSelect();
        }
        else if (screen == Screen::Versus) UpdateVersus();
        else if (screen == Screen::Options) UpdateOptions();
        else if (paused) UpdatePause();
        else if (KeyBack()) { paused = true; pauseSel = 0; PlaySfx("pausa_vhs"); }
        else UpdateMenu();
    }
    UpdateTransition();

    // Tempo do quadro encaixado no período do monitor: o relógio oscila ±0,6 ms e, com a lógica a 60 e a tela a 60,
    // isso fazia quadros com 0 ou 2 ticks (um desenho da animação ficava 1 quadro a mais/a menos). Perto do período
    // (até 2 ms), usa o exato: 1 tick por quadro sempre a 60 Hz; a 55 (165 Hz), 1-1-...-2 em padrão fixo.
    float dt = GetFrameTime();
    if (framePeriod > 0 && fabsf(dt - framePeriod) < 0.002f) dt = framePeriod;
    acc += std::min(dt, 0.25f);
    while (acc >= 1.0 / 60) {
        acc -= 1.0 / 60;
        if (fighting) Tick();
    }
    if (koT >= 0 && fighting) {
        float before = koT;
        koT += GetFrameTime();
        if (MatchOver() && before < 1.2f && koT >= 1.2f) PlaySfx(p[Winner()].def->winVoice.c_str());  // junto da faixa VENCEU!
    }
    for (const std::string& n : sfxQueue) PlaySfx(n.c_str());  // sons da luta deste quadro
    sfxQueue.clear();

    // Cena 384x216 desenhada direto na tela com zoom inteiro (sem filtro, centralizada). Diferente de esticar uma
    // imagem 384x216, isso deixa o personagem andar em passos de 1 pixel de tela (1/3 de pixel do jogo a 3x).
    int s = std::max(1, std::min(GetScreenWidth() / SCREEN_W, GetScreenHeight() / SCREEN_H));
    int ox = (GetScreenWidth() - SCREEN_W * s) / 2, oy = (GetScreenHeight() - SCREEN_H * s) / 2;
    Camera2D cam = {{(float)ox, (float)oy}, {0, 0}, 0, (float)s};
    if ((screen == Screen::Fight && !paused) || screen == Screen::Versus) {  // tremida do K.O./VS, px inteiros
        Vector2 sh = screen == Screen::Versus ? VsShake() : KoShake();
        cam.offset.x += sh.x * s;
        cam.offset.y += sh.y * s;
    }
    float alpha = (float)(acc * 60);  // fração do próximo tick já passada
    if (loaded && screen == Screen::Fight) UpdateTv(alpha, stage.light);  // fora do BeginDrawing (tesoura da tela)
    BeginDrawing();
    ClearBackground(BLACK);
    viewCam = cam;
    viewClip = {(float)ox, (float)oy, (float)(SCREEN_W * s), (float)(SCREEN_H * s)};
    BeginScissorMode(ox, oy, SCREEN_W * s, SCREEN_H * s);
    BeginMode2D(cam);
    if (screen == Screen::Loading) {
        Loading_Draw();
        EndMode2D();
        EndScissorMode();
        EndDrawing();
        return;
    }
    if (loaded && screen != Screen::Fight) {
        if (screen == Screen::Title) DrawTitle();
        else if (screen == Screen::Select && selectStage == 0) DrawSelect();
        else if (screen == Screen::Select && selectStage == 1) DrawPetSelect();
        else if (screen == Screen::Select) DrawStageSelect();
        else if (screen == Screen::Versus) DrawVersus();
        else DrawOptions();
        if (GetTime() < msgUntil) Text(msg.c_str(), 4, SCREEN_H - 12, 10, WHITE);
        DrawTransition();
        EndMode2D();
        EndScissorMode();
        DrawTouch();
        EndDrawing();
        return;
    }
    const Light& light = HasArena() ? stage.light : roster.light;
    float shift = loaded ? roundf(ArenaShift(alpha) * s) / s : 0;  // encaixa na grade de pixels da tela
    if (loaded && !paused) {  // câmera vertical: segue o topo da cabeça mais alta, suave, em px inteiros da tela
        float top = SCREEN_H;
        for (const Fighter& f : p) top = std::min(top, GROUND_Y - (f.prevY + (f.y - f.prevY) * alpha) - f.def->height);
        float target = HasArena() ? std::clamp(CAM_TOP - top, 0.0f, std::min(CAM_UP_MAX, stage.extra)) : 0;
        camY += (target - camY) * std::min(1.0f, GetFrameTime() * (target > camY ? 10.0f : 5.0f));
    }
    float cy = roundf(camY * s) / s;
    viewPpu = (float)s;
    worldCam = loaded ? Snap(arena.prevCam + (arena.cam - arena.prevCam) * alpha) : 0;  // câmera horizontal (arena larga)
    rlPushMatrix();
    rlTranslatef(0, cy, 0);  // só o mundo (cenário, TV, lutadores); HUD e telas por cima ficam paradas
    float saveCam = camY;
    camY = cy;
    DrawArena(shift, light);
    camY = saveCam;
    if (loaded) {
        rlPushMatrix();
        rlTranslatef(-worldCam, 0, 0);  // lutadores, mascotes e efeitos em coordenadas do mundo
        DrawTv(shift, (float)s);
        DrawFightScene(alpha, (float)s, light, debug);
        if (debug && HasArena()) {  // F1: linha do chão (floor_y do stages.json) e limites da arena (vermelho)
            DrawRectangle((int)arena.x0, GROUND_Y, (int)(arena.x1 - arena.x0), 1, YELLOW);
            Text(TextFormat("floor_y %d", (int)stage.floorY), (int)worldCam + 4, GROUND_Y + 2, 10, YELLOW);
            DrawRectangle((int)arena.x0, 0, 1, GROUND_Y, RED);
            DrawRectangle((int)arena.x1 - 1, 0, 1, GROUND_Y, RED);
            if (platform.top > 0) {  // mesa: tampo e lateral
                DrawRectangleRec({0, GROUND_Y - platform.top, platform.x1, 1}, ORANGE);
                DrawRectangleRec({platform.x1, GROUND_Y - platform.top, 1, platform.top}, ORANGE);
            }
        }
        rlPopMatrix();
    }
    rlPopMatrix();
    if (loaded) {
        DrawKoArena(alpha, (float)s, light);
        DrawHud();
        if (koT >= 0 && koT < 2 / 60.0f) DrawRectangle(0, 0, SCREEN_W, SCREEN_H, WHITE);  // flash do K.O.
        if (paused) {
            DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(BLACK, 0.5f));
            DrawList("PAUSA", PAUSE_ITEMS, PAUSE_N, pauseSel, 60);
        }
    }
    if (!loaded || GetTime() < msgUntil) Text(msg.c_str(), 4, SCREEN_H - 12, 10, WHITE);
    DrawTransition();
    EndMode2D();
    EndScissorMode();
    DrawTouch();
    EndDrawing();
}

int main() {
    // rodando de build/Release/: procura os assets (e as opções) na raiz do projeto
    if (!FileExists(DATA)) ChangeDirectory(TextFormat("%s../..", GetApplicationDirectory()));
    LoadOptions();
#ifdef PLATFORM_WEB
    SetConfigFlags(FLAG_VSYNC_HINT);  // sem RESIZABLE: o raylib redimensionaria para a página sem o devicePixelRatio
    touchUi = EM_ASM_INT({ return ('ontouchstart' in window) || navigator.maxTouchPoints > 0; }) != 0;
#else
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
#endif
    InitWindow(SCREEN_W * opt.scale, SCREEN_H * opt.scale, "Danin Fighters");
    // lote de desenho com 3 buffers em rodízio (o padrão do raylib é 1 de 8192): trocar de shader (silhueta, borda
    // de luz) esvazia o lote; com um buffer só, o driver espera a placa de vídeo ou copia o buffer a cada troca
    rlRenderBatch batch = rlLoadRenderBatch(3, 4096);
    rlSetRenderBatchActive(&batch);
    SetWindowMinSize(SCREEN_W, SCREEN_H);
    LoadTypography();
    InitAudioDevice();
    if (FileExists(MENU_MUSIC)) menuMusic = LoadMusicStream(MENU_MUSIC);
    LoadSounds();
    SetExitKey(KEY_NULL);  // Esc volta entre as telas; sair é pelo menu (ou fechando a janela)
    ApplyWindow();
    ApplyFrameRate(true);
    Loading_Begin();
#ifdef PLATFORM_WEB
    emscripten_set_main_loop(UpdateDrawFrame, 0, 1);
#else
    while (!WindowShouldClose() && !quit) UpdateDrawFrame();
#endif
    loadTasks.clear();  // fechou no meio do loading: libera antes de fechar a janela
    UnloadRoster(loadRoster);
    if (loadBg.id) UnloadTexture(loadBg);
    UnloadRoster(roster);
    UnloadMenu();
    UnloadTypography();
    if (IsMusicValid(menuMusic)) UnloadMusicStream(menuMusic);
    for (auto& [name, snd] : sounds) UnloadSound(snd);
    UnloadStage();
    for (StageInfo& s : stageList) if (s.preview.id) UnloadTexture(s.preview);
    if (tvRT.id) UnloadRenderTexture(tvRT);
    CloseAudioDevice();
    rlSetRenderBatchActive(nullptr);
    rlUnloadRenderBatch(batch);
    CloseWindow();
    return 0;
}
