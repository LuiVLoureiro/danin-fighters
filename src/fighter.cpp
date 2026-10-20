#include "fighter.h"
#include "typography.h"
#include <cstring>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>

using json = nlohmann::json;

// ---------- dados ----------

static Move ReadMove(const json& j, Move m) {
    m.damage = j.value("damage", m.damage);
    m.hitHeight = std::clamp(j.value("hit_height", m.hitHeight), 0.0f, 1.0f);
    m.range = j.value("range", m.range);
    m.startup = std::max(0, j.value("startup", m.startup));
    m.active = std::max(1, j.value("active", m.active));
    m.recovery = std::max(0, j.value("recovery", m.recovery));
    std::string g = j.value("guard", std::string(m.guard == Guard::Low ? "low" : m.guard == Guard::High ? "high" : "mid"));
    if (g != "mid" && g != "low" && g != "high") throw std::runtime_error("guard '" + g + "' (use mid, low ou high)");
    m.guard = g == "low" ? Guard::Low : g == "high" ? Guard::High : Guard::Mid;
    m.sfx = j.value("sfx", m.sfx);
    m.voice = j.value("voice", m.voice);
    m.push = j.value("push", m.push);
    return m;
}

void UnloadRoster(Roster& r) {
    for (SupportDef& s : r.supports.list) {
        for (auto& [name, a] : s.anims) UnloadAnim(a);
        for (Texture2D t : {s.icon, s.portrait, s.card, s.stage})
            if (t.id) UnloadTexture(t);
    }
    r.supports.list.clear();
    for (CharDef& c : r.chars) {
        for (auto& [name, a] : c.anims) UnloadAnim(a);
        for (Texture2D t : {c.icon, c.portrait, c.card})
            if (t.id) UnloadTexture(t);
    }
    r.chars.clear();
}

static int ActiveFrame(const Anim& a);
static const Anim* FindAnim(const CharDef& d, const char* name);
static const Anim* StandAnim(const CharDef& d);
static bool ParryBox(const Fighter& f, Rectangle& box);

static float Median(std::vector<float> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Animação em loop (idle): mede cada quadro e aponta os que destoam da mediana — num loop, um quadro maior ou
// deslocado vira um "pulo" visível a cada volta. Também mede o salto ao trocar entre andar e parado.
static void ReportLoop(const CharDef& d, const Anim& an) {
    size_t n = an.boxes.size();
    std::vector<float> height(n), lean(n), width(n);
    for (size_t i = 0; i < n; i++) {
        const FrameBoxes& b = an.boxes[i];
        height[i] = an.bodyHeights[i];
        lean[i] = (b.body.x0 + b.body.x1) / 2 - b.feetX;  // centro do tronco em relação aos pés
        width[i] = b.right - b.left;
    }
    float mh = Median(height), ml = Median(lean), mw = Median(width);
    std::string bad;
    for (size_t i = 0; i < n; i++) {
        float dh = height[i] - mh, dl = lean[i] - ml, dw = width[i] - mw;
        bool off = fabsf(dh) > 2 || fabsf(dl) > 2 || fabsf(dw) > 4;
        if (off) bad += (bad.empty() ? "" : ", ") + std::to_string(i + 1);
        TraceLog(off ? LOG_WARNING : LOG_INFO, "IDLE: %s quadro %2d: altura %.1f (%+.1f)  tronco/pes %+.1f (%+.1f)  largura %.0f (%+.0f)%s",
                 d.id.c_str(), (int)i + 1, height[i], dh, lean[i], dl, width[i], dw, off ? "  <- DESTOA" : "");
    }
    TraceLog(LOG_INFO, "IDLE: %s mediana: altura %.1f, tronco/pes %+.1f, largura %.0f; destoam: %s", d.id.c_str(), mh, ml, mw,
             bad.empty() ? "nenhum" : bad.c_str());
    // troca andar <-> parado: o andar fica preso no tronco, o idle nos pés; o quanto pés e tronco "pulam"
    if (auto w = d.anims.find("walk"); w != d.anims.end() && !w->second.boxes.empty()) {
        const FrameBoxes& wb = w->second.boxes[0];
        TraceLog(LOG_INFO, "IDLE: %s ao parar/andar: pes pulam %.1f px, tronco pula %.1f px; altura andar %.1f x parado %.1f",
                 d.id.c_str(), wb.feetX, (an.boxes[0].body.x0 + an.boxes[0].body.x1) / 2, w->second.bodyHeights[0], height[0]);
    }
}

// Ordem das anims de um bloco "anims": quem copia a escala de outra ("scale_like") vem depois dela.
static std::vector<std::string> AnimOrder(const json& anims) {
    std::vector<std::string> names;
    for (int pass = 0; pass < 2; pass++)
        for (auto& [name, a] : anims.items())
            if ((pass == 0) == a.value("scale_like", "").empty()) names.push_back(name);
    return names;
}

// Uma anim de um personagem ou mascote. walkSpeed > 0: o "walk" sem fps ganha o ritmo calculado para o pé não deslizar.
static void LoadOneAnim(const std::string& owner, const std::string& name, const json& a, int height, int tol,
                        std::map<std::string, Anim>& out, float walkSpeed) {
    std::string like = a.value("scale_like", "");
    int fps = a.value("fps", 0);
    AnimOptions opt;
    opt.fps = fps > 0 ? fps : 10;
    opt.refFrame = a.value("ref_frame", -1);
    opt.pixelScale = a.value("pixel_scale", 0.0f);
    opt.frameScale = a.value("frame_scale", std::vector<float>{});
    opt.layout = a.value("layout", std::vector<int>{});
    if (a.contains("rows") || a.contains("cols"))  // grade regular: rows x cols em cada sheet
        opt.layout.assign(std::max(1, a.value("rows", 1)), std::max(1, a.value("cols", 1)));
    opt.feetAnchor = a.value("anchor", "") == std::string("feet");
    opt.refByWidth = a.value("scale_by", "") == std::string("width");
    if (!like.empty()) {
        auto src = out.find(like);
        if (src == out.end() || src->second.boxes.empty())
            throw std::runtime_error(owner + "/" + name + ": scale_like '" + like + "' nao carregou");
        // "scale_like_frame" (a partir de 1): o corpo fica com a altura desse quadro da outra anim
        // (golpe agachado = altura do último quadro de "crouch"); sem ele, a mediana
        int lf = a.value("scale_like_frame", 0);
        const std::vector<float>& hs = src->second.bodyHeights;
        if (lf < 0 || lf > (int)hs.size())
            throw std::runtime_error(owner + "/" + name + ": scale_like_frame " + std::to_string(lf) +
                                     " (existem " + std::to_string(hs.size()) + ")");
        opt.bodyTarget = lf ? hs[lf - 1] : src->second.bodyMedian;
    }
    std::vector<std::string> files = a.value("files", std::vector<std::string>{});
    if (files.empty()) files.push_back(a.value("file", ""));
    // "height" na anim = tamanho próprio (efeitos: projétil, impacto); padrão = altura do dono
    Anim& an = out[name] = LoadAnim(files, a.value("frames", 1), a.value("height", height), tol, opt);
    an.activeFrame = a.value("active_frame", -1);
    an.mirror = a.value("mirror", false);
    an.spin = a.value("spin", 0.0f);
    an.breath = std::max(0.0f, a.value("breath", 0.0f));
    an.breathPeak = a.value("breath_peak", 0.5f);
    // "sequence": ordem de tocar, com números de quadro a partir de 1 (repetir = segurar o quadro)
    for (int q : a.value("sequence", std::vector<int>{}))
        if (q >= 1 && q <= (int)an.boxes.size()) an.order.push_back(q - 1);
        else throw std::runtime_error(owner + "/" + name + ": sequence tem quadro " + std::to_string(q) +
                                      " (existem " + std::to_string(an.boxes.size()) + ")");
    if (an.breath > 0 && !an.frames.empty()) {  // fundo da onda = quadro mais baixo que toca
        an.breathBase = 1e9f;
        for (size_t k = 0; k < an.frames.size(); k++)
            if (an.order.empty() || std::find(an.order.begin(), an.order.end(), (int)k) != an.order.end())
                an.breathBase = std::min(an.breathBase, (float)an.frames[k].height);
    }
    // andar sem "fps" no JSON: ritmo calculado para o pé não deslizar.
    // 1 ciclo = 2 passos ⇒ distância por ciclo = 2 × passo ⇒ fps = velocidade × quadros / (2 × passo)
    if (name.rfind("walk", 0) == 0 && walkSpeed > 0 && fps <= 0 && an.stepLen > 0) {
        SetAnimFps(an, walkSpeed * an.frames.size() / (2 * an.stepLen));
        TraceLog(LOG_INFO, "ANDAR: %s passo %.0f px, %.0f px/s -> %.2f fps (%.2f passos/s)", owner.c_str(),
                 an.stepLen, walkSpeed, an.fps, walkSpeed / an.stepLen);
    }
}

// Dados de um mascote do supports.json (as imagens entram depois, em tarefas).
static SupportDef ReadSupport(const json& sj) {
    SupportDef s;
    s.id = sj.at("id").get<std::string>();
    s.name = sj.value("name", s.id);
    std::string type = sj.value("type", std::string("bark"));
    if (type != "bark" && type != "attack" && type != "heal")
        throw std::runtime_error(s.id + ": type '" + type + "' (use bark, attack ou heal)");
    s.kind = type == "attack" ? PetKind::Attack : type == "heal" ? PetKind::Heal : PetKind::Bark;
    s.sfx = sj.value("sfx", s.sfx);
    s.cost = std::max(0, sj.value("cost", s.cost));
    s.cooldown = std::max(0, sj.value("cooldown", s.cooldown));
    s.maxUses = std::max(1, sj.value("max_uses_per_round", s.maxUses));
    s.runSpeed = std::max(1.0f, sj.value("run_speed", s.runSpeed));
    s.stopOffset = sj.value("stop_offset", s.stopOffset);
    s.actionTicks = std::max(1, sj.value("action_ticks", s.actionTicks));
    if (sj.contains("push")) {
        const json& p = sj.at("push");
        s.pushMax = p.value("max", s.pushMax);
        s.pushDamage = std::max(0, p.value("damage", s.pushDamage));
        s.pushRadius = std::max(1.0f, p.value("radius", s.pushRadius));
        s.friction = std::clamp(p.value("friction", s.friction), 0.0f, 0.99f);
    }
    if (sj.contains("attack")) s.attack = ReadMove(sj.at("attack"), s.attack);
    s.activeFrames = std::max(1, sj.value("active_frames", s.activeFrames));
    if (sj.contains("hitbox")) {
        std::vector<float> hb = sj.at("hitbox").get<std::vector<float>>();
        if (hb.size() == 4) s.hitbox = {hb[0], hb[1], hb[2], hb[3]};
    }
    if (sj.contains("heal")) {
        s.healTotal = std::max(0.0f, sj.at("heal").value("total", s.healTotal));
        s.healTicks = std::max(1, sj.at("heal").value("ticks", s.healTicks));
    }
    s.description = sj.value("description", "");
    s.shortText = sj.value("short", s.description);
    TraceLog(LOG_INFO, "MASCOTES: %s (%s) custo %d, espera %d ticks, %d por round", s.id.c_str(), type.c_str(), s.cost,
             s.cooldown, s.maxUses);
    return s;
}

// Dados de um personagem do characters.json (as imagens entram depois, em tarefas).
static CharDef ReadChar(const json& c, const json& j) {
    CharDef d;
    d.id = c.at("id").get<std::string>();
    d.name = c.value("name", d.id);
    d.height = std::max(8, c.value("height", d.height));
    if (c.contains("height_cm"))  // altura real: px = cm x "px_per_cm" (topo do JSON)
        d.height = std::max(8, (int)lroundf(c.at("height_cm").get<float>() * j.value("px_per_cm", 0.66f)));
    d.idleCall = std::max(0, c.value("idle_call", 0));
    d.health = std::max(1, c.value("health", d.health));
    d.steps = c.value("steps", d.steps);
    if (c.contains("voice")) {
        d.hurtVoice = c["voice"].value("hurt", d.hurtVoice);
        d.koVoice = c["voice"].value("ko", d.koVoice);
        d.winVoice = c["voice"].value("win", d.winVoice);
        d.voiceVolume = std::clamp(c["voice"].value("volume", d.voiceVolume), 0.0f, 1.0f);
    }
    d.walkSpeed = c.value("walk_speed", d.walkSpeed);
    d.style = c.value("style", "");
    d.specialName = c.value("special_name", "");
    if (c.contains("stats")) {
        const json& st = c.at("stats");
        const char* keys[3] = {"strength", "speed", "reach"};
        for (int k = 0; k < 3; k++) d.stats[k] = std::clamp(st.value(keys[k], d.stats[k]), 0, 5);
    }
    d.blockChip = std::clamp(c.value("block_chip", j.value("block_chip", d.blockChip)), 0.0f, 1.0f);
    if (j.contains("guard")) {
        const json& g = j.at("guard");
        d.guardPerDamage = std::max(0.0f, g.value("per_damage", d.guardPerDamage));
        d.guardBreak = std::max(1, (int)lroundf(60 * g.value("break", d.guardBreak / 60.0f)));
        d.guardDelay = std::max(0, (int)lroundf(60 * g.value("regen_delay", d.guardDelay / 60.0f)));
        d.guardRegen = std::max(0.0f, g.value("regen", d.guardRegen));
    }
    if (j.contains("repeat_penalty")) {
        const json& rp = j.at("repeat_penalty");
        d.repeatStep = std::clamp(rp.value("step", d.repeatStep), 0.0f, 1.0f);
        d.repeatMin = std::clamp(rp.value("min", d.repeatMin), 0.0f, 1.0f);
        d.repeatReset = std::max(1, (int)lroundf(60 * rp.value("reset", d.repeatReset / 60.0f)));
    }
    if (c.contains("punch")) d.punch = ReadMove(c.at("punch"), d.punch);
    if (c.contains("kick")) d.kick = ReadMove(c.at("kick"), d.kick);
    d.punchBare = c.contains("punch_unarmed") ? ReadMove(c.at("punch_unarmed"), d.punch) : d.punch;
    d.kickBare = c.contains("kick_unarmed") ? ReadMove(c.at("kick_unarmed"), d.kick) : d.kick;
    if (c.contains("air_attack")) d.airAttack = ReadMove(c.at("air_attack"), d.airAttack);
    if ((d.hasCrouchPunch = c.contains("crouch_punch"))) d.crouchPunch = ReadMove(c.at("crouch_punch"), d.crouchPunch);
    if ((d.hasCrouchKick = c.contains("crouch_kick"))) d.crouchKick = ReadMove(c.at("crouch_kick"), d.crouchKick);
    if (c.contains("dash")) {
        const json& dj = c.at("dash");
        d.dash.on = true;
        d.dash.speed = dj.value("speed", d.dash.speed);
        d.dash.damage = std::max(0, dj.value("damage", d.dash.damage));
        d.dash.push = dj.value("push", d.dash.push);
        d.dash.maxTicks = std::max(1, dj.value("max_ticks", d.dash.maxTicks));
        d.dash.startup = std::max(0, dj.value("startup", d.dash.startup));
        d.dash.recovery = std::max(1, dj.value("recovery", d.dash.recovery));
        d.dash.halfCircle = dj.value("half_circle", false);
    }
    if (c.contains("punch2")) { d.punch2 = ReadMove(c.at("punch2"), d.punch2); d.chainLen = 2; }
    if (c.contains("punch3") && d.chainLen == 2) { d.punch3 = ReadMove(c.at("punch3"), d.punch3); d.chainLen = 3; }
    if ((d.hasSuper = c.contains("super"))) {
        const json& sj = c.at("super");
        d.super = ReadMove(sj, d.super);
        d.superSpeed = sj.value("speed", d.superSpeed);
        d.superHits = std::max(1, sj.value("hits", d.superHits));
        d.superEvery = std::max(1, sj.value("hit_every", d.superEvery));
        d.dizzyTicks = std::max(0, sj.value("dizzy", d.dizzyTicks));
    }
    if ((d.hasSpecial = c.contains("special"))) {
        d.special = ReadMove(c.at("special"), d.special);
        d.shotSpeed = c.at("special").value("speed", d.shotSpeed);
        // errou (projétil ainda voando, sem acertar nem ser defendido): fica mais tempo parado, punível
        d.specialWhiff = std::max(d.special.recovery, c.at("special").value("whiff_recovery", d.special.recovery + 18));
        d.launchSfx = c.at("special").value("launch_sfx", d.launchSfx);
        d.shotFromGround = c.at("special").value("from", std::string()) == "ground";
        d.dropsWeapon = c.at("special").value("drops_weapon", false);
    }
    if ((d.spray.on = c.contains("spray"))) {
        const json& s = c.at("spray");
        CharDef::Spray& sp = d.spray;
        sp.move = ReadMove(s, sp.move);
        sp.hitEvery = std::max(1, s.value("hit_every", sp.hitEvery));
        sp.puffEvery = std::max(1, s.value("puff_every", sp.puffEvery));
        sp.drain = std::max(0.01f, s.value("drain", sp.drain));
        sp.recharge = std::max(0.0f, s.value("recharge", sp.recharge));
        sp.restart = std::clamp(s.value("restart", sp.restart), 0.0f, 100.0f);
    }
    if ((d.suitcase.on = c.contains("suitcase"))) {
        const json& s = c.at("suitcase");
        CharDef::Suitcase& su = d.suitcase;
        su.move = ReadMove(s, su.move);
        su.speed = s.value("speed", su.speed);
        su.spin = s.value("spin", su.spin);
        su.angles = s.value("angles", su.angles);
        su.cooldown = std::max(0, s.value("cooldown", su.cooldown));
    }
    if (d.hasSpecial) {
        const json& s = c.at("special");
        d.shotGravity = std::max(0.0f, s.value("gravity", 0.0f));
        d.shotLift = s.value("lift", 0.0f);
        d.specialCooldown = std::max(0, s.value("cooldown", 0));
        if (s.contains("cloud")) {
            d.cloudTicks = std::max(1, s["cloud"].value("ticks", d.cloudTicks));
            d.cloudDamage = std::max(0, s["cloud"].value("damage", d.cloudDamage));
            d.cloudEvery = std::max(1, s["cloud"].value("hit_every", d.cloudEvery));
        }
    }
    if (c.contains("taunt")) {
        const json& s = c.at("taunt");
        d.tauntText = s.value("text", std::string());
        d.tauntChance = std::clamp(s.value("chance", d.tauntChance), 0.0f, 1.0f);
        d.tauntTicks = std::max(1, s.value("ticks", d.tauntTicks));
        d.tauntGap = std::max(0, s.value("gap", d.tauntGap));
    }
    if ((d.parry.on = c.contains("parry"))) {
        const json& pj = c.at("parry");
        CharDef::Parry& p = d.parry;
        p.startup = std::max(0, pj.value("startup", p.startup));
        p.active = std::max(1, pj.value("active", p.active));
        p.recovery = std::max(0, pj.value("recovery", p.recovery));
        p.whiffRecovery = std::max(0, pj.value("whiff_recovery", p.whiffRecovery));
        p.cooldown = std::max(0, pj.value("cooldown", p.cooldown));
        p.hitstop = std::max(0, pj.value("hitstop", p.hitstop));
        p.stunBonus = std::max(0, pj.value("hitstun_bonus", p.stunBonus));
        p.reflectSpeed = pj.value("reflect_speed", p.reflectSpeed);
        p.voice = pj.value("voice", p.voice);
    }
    if (c.contains("jump")) {
        const json& jp = c.at("jump");
        d.jumpHeight = std::max(1.0f, jp.value("height", d.jumpHeight));
        d.jumpRise = std::max(1, jp.value("rise", d.jumpRise));
        d.jumpFall = std::max(1, jp.value("fall", d.jumpFall));
        d.jumpSpeed = jp.value("speed", d.jumpSpeed);
        d.doubleJump = std::max(0.0f, jp.value("double", d.doubleJump));
    }
    return d;
}

// Depois das anims de um personagem: ícone, retrato, colisão do corpo e o relatório das caixas no console.
static void FinishChar(CharDef& d, const json& c, int tol) {
    d.icon = LoadPicture(c.value("icon", ""), tol, ICON_D * PICTURE_RES, ICON_D * PICTURE_RES, true);
    // "portrait_head" = altura da cabeça (topo do crânio, sem cabelo armado, ao queixo) em px da imagem original: cada IA enquadra de um
    // jeito (close ou de longe), então a escala vem da cabeça; sem ele, o retrato só cabe na caixa
    float head = c.value("portrait_head", 0.0f);
    // arte própria da tela de escolha, se existir: assets/ui/portraits/<id>_big.png (busto) e <id>_icon.png
    std::string big = "assets/ui/portraits/" + d.id + "_big.png", card = "assets/ui/portraits/" + d.id + "_icon.png";
    if (FileExists(big.c_str()))
        d.portrait = LoadPicture(big, tol, PORTRAIT_W * PICTURE_RES, PORTRAIT_H * PICTURE_RES, false);
    else
        d.portrait = LoadPicture(c.value("portrait", ""), tol, PORTRAIT_W * PICTURE_RES,
                                 PORTRAIT_H * PICTURE_RES, false, head > 0 ? (float)PORTRAIT_HEAD * PICTURE_RES / head : 0);
    // cartão da grade: quadrado do topo do desenho (cabeça), sem a máscara redonda
    d.card = LoadPicture(FileExists(card.c_str()) ? card : c.value("icon", ""), tol, 28 * PICTURE_RES, 28 * PICTURE_RES,
                         true, 0, 0.5f, 1, false);
    // colisão do corpo = corpo medido no quadro parado do andar (sem andar: o 1º quadro tocado do idle)
    if (const Anim* st = StandAnim(d)) {
        const FrameBoxes& b = st->boxes[st->order.empty() ? 0 : st->order[0]];
        d.pushBack = -b.body.x0;
        d.pushFront = std::max(b.body.x1, b.front);
        TraceLog(LOG_INFO, "CAIXAS: %s corpo %.0f px para tras, %.0f px para a frente, altura %.0f", d.id.c_str(), d.pushBack,
                 d.pushFront, b.body.y1);
    }
    if (auto cr = d.anims.find("crouch"); cr != d.anims.end())
        for (size_t i = 0; i < cr->second.boxes.size(); i++)
            TraceLog(LOG_INFO, "CAIXAS: %s agachar quadro %d altura %.0f", d.id.c_str(), (int)i, cr->second.boxes[i].body.y1);
    if (auto id = d.anims.find("idle"); id != d.anims.end()) ReportLoop(d, id->second);
    if (const Anim* pa = d.parry.on ? FindAnim(d, "parry") : nullptr) {
        Fighter probe = MakeFighter(d, 100, WHITE);  // olhando para a direita
        probe.act = Act::Parry;
        probe.t = d.parry.startup;
        Rectangle pb;
        if (ParryBox(probe, pb))
            TraceLog(LOG_INFO, "CAIXAS: %s parry de %.0f a %.0f px a frente, altura %.0f-%.0f", d.id.c_str(),
                     pb.x - probe.x, pb.x + pb.width - probe.x, GROUND_Y - pb.y - pb.height, GROUND_Y - pb.y);
    }
    for (auto& [name, an] : d.anims)  // quadros em que um pedaço solto fica abaixo do corpo
        for (size_t i = 0; i < an.boxes.size(); i++)
            if (an.boxes[i].sink >= 1)
                TraceLog(LOG_INFO, "CAIXAS: %s %s quadro %d: corpo apoiado no chao, pedaco solto %.0f px abaixo",
                         d.id.c_str(), name.c_str(), (int)i + 1, an.boxes[i].sink);
    for (auto& [name, an] : d.anims)
        for (size_t i = 0; i < an.boxes.size(); i++) {
            const FrameBoxes& b = an.boxes[i];
            if (b.hasHit && (int)i == ActiveFrame(an) && (name == "punch" || name == "kick" || name == "air_attack" || name == "special" || name == "parry" ||
                                       name == "crouch_punch" || name == "crouch_kick"))
                TraceLog(LOG_INFO, "CAIXAS: %s %s acerta de %.0f a %.0f px a frente, altura %.0f-%.0f", d.id.c_str(),
                         name.c_str(), b.hit.x0, b.hit.x1, b.hit.y0, b.hit.y1);
        }
}

// Tudo que a fila carrega mora aqui até a entrega (erro no meio: o destrutor libera o que já carregou).
struct RosterJob {
    Roster r;
    json j, sj;
    bool delivered = false;
    ~RosterJob() {
        if (!delivered) UnloadRoster(r);
    }
};

void QueueRoster(const char* path, std::vector<std::function<void()>>& tasks, Roster& out, const SupportSet*& pets) {
    auto job = std::make_shared<RosterJob>();
    std::ifstream file(path);
    if (!file) throw std::runtime_error(std::string("arquivo nao encontrado: ") + path);
    job->j = json::parse(file);
    Roster& r = job->r;
    const json& j = job->j;
    r.chromaTolerance = j.value("chroma_tolerance", r.chromaTolerance);
    if (j.contains("light")) {
        const json& l = j.at("light");
        r.light.side = l.value("sun", std::string("right")) == "left" ? -1 : 1;
        r.light.shadowLength = l.value("shadow_length", r.light.shadowLength);
        r.light.shadowAlpha = std::clamp(l.value("shadow_alpha", r.light.shadowAlpha), 0.0f, 1.0f);
    }
    for (const json& c : j.at("characters")) r.chars.push_back(ReadChar(c, j));
    auto find = [&](const char* key) {
        std::string id = j.value(key, "");
        for (size_t i = 0; i < r.chars.size(); i++)
            if (r.chars[i].id == id) return (int)i;
        throw std::runtime_error(std::string(key) + " = '" + id + "' nao esta em characters");
    };
    r.p1 = find("p1");
    r.p2 = find("p2");
    // assets/data/supports.json (na mesma pasta do characters.json). Sem o arquivo: jogo sem mascotes.
    std::string dir(path);
    std::string sp = dir.substr(0, dir.find_last_of("/\\") + 1) + "supports.json";
    if (std::ifstream sf(sp); sf) {
        job->sj = json::parse(sf);
        SupportSet& ss = r.supports;
        ss.maxMeter = std::max(1.0f, job->sj.value("max_meter", ss.maxMeter));
        ss.gainHit = std::max(0.0f, job->sj.value("gain_on_hit", ss.gainHit));
        ss.gainTaken = std::max(0.0f, job->sj.value("gain_on_damage_taken", ss.gainTaken));
        if (job->sj.contains("comeback")) {
            ss.comebackBelow = std::clamp(job->sj["comeback"].value("below", ss.comebackBelow), 0.0f, 1.0f);
            ss.comebackGain = std::max(1.0f, job->sj["comeback"].value("gain", ss.comebackGain));
        }
        for (const json& sj : job->sj.at("supports")) ss.list.push_back(ReadSupport(sj));
    } else {
        TraceLog(LOG_WARNING, "MASCOTES: %s nao encontrado, jogo sem mascotes", sp.c_str());
    }
    out.chromaTolerance = r.chromaTolerance;  // já vale antes da entrega (logo da tela de loading)
    int tol = r.chromaTolerance;

    // mascotes primeiro: a tela de loading desenha eles correndo enquanto o resto carrega
    for (size_t i = 0; i < r.supports.list.size(); i++) {
        const json& sj = job->sj.at("supports")[i];
        // "height" = altura (px) do corpo mais alto da corrida; "scale" multiplica (ajuste fino)
        int height = std::max(4, (int)lroundf(sj.value("height", 32.0f) * sj.value("scale", 1.0f)));
        if (sj.contains("anims"))
            for (const std::string& n : AnimOrder(sj.at("anims")))
                tasks.push_back([job, i, n, height, tol] {
                    SupportDef& s = job->r.supports.list[i];
                    LoadOneAnim(s.id, n, job->sj.at("supports")[i].at("anims").at(n), height, tol, s.anims, 0);
                });
        tasks.push_back([job, i, tol, height] {
            SupportDef& s = job->r.supports.list[i];
            const json& sj = job->sj.at("supports")[i];
            s.icon = LoadPicture(sj.value("icon", ""), tol, ICON_D * PICTURE_RES, ICON_D * PICTURE_RES, true, 0,
                                 sj.value("icon_focus", 0.5f), sj.value("icon_size", 1.0f));
            // arte grande: "portrait" (padrão = a mesma imagem do ícone, inteira)
            s.portrait = LoadPicture(sj.value("portrait", sj.value("icon", "")), tol, PET_ART_W * PICTURE_RES,
                                     PET_ART_H * PICTURE_RES, false);
            // tela de escolha: rosto quadrado sem máscara redonda e a pose parada com a altura da corrida em 2x
            s.card = LoadPicture(sj.value("icon", ""), tol, 34 * PICTURE_RES, 34 * PICTURE_RES, true, 0,
                                 sj.value("icon_focus", 0.5f), sj.value("icon_size", 1.0f), false);
            s.stage = LoadPicture(sj.value("portrait", sj.value("icon", "")), tol, 110 * PICTURE_RES,
                                  2 * height * PICTURE_RES, false);
        });
    }
    tasks.push_back([job, &pets] { pets = &job->r.supports; });
    for (size_t i = 0; i < r.chars.size(); i++) {
        const json& c = j.at("characters")[i];
        if (c.contains("anims"))
            for (const std::string& n : AnimOrder(c.at("anims")))
                tasks.push_back([job, i, n, tol] {
                    CharDef& d = job->r.chars[i];
                    LoadOneAnim(d.id, n, job->j.at("characters")[i].at("anims").at(n), d.height, tol, d.anims,
                                d.walkSpeed);
                });
        tasks.push_back([job, i, tol] { FinishChar(job->r.chars[i], job->j.at("characters")[i], tol); });
    }
    tasks.push_back([job, &out, &pets] {
        out = std::move(job->r);
        job->delivered = true;
        pets = &out.supports;
    });
}

bool LoadRoster(const char* path, Roster& out, std::string& err) {
    try {
        std::vector<std::function<void()>> tasks;
        const SupportSet* pets = nullptr;
        Roster r;
        QueueRoster(path, tasks, r, pets);
        for (auto& t : tasks) t();
        out = std::move(r);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// ---------- luta ----------

const int HIT_STUN = 15, BLOCK_STUN = 8;  // ticks atordoado depois de apanhar / defender
const int JUMP_PREP = 3, JUMP_LAND = 4;   // ticks no chão antes de sair e depois de aterrissar (50 e 67 ms)
const int MOTION_WINDOW = 10;             // especial: → até 10 ticks depois do ↓, soco até 10 ticks depois do → (167 ms)

static bool IsAttack(Act a) {
    return a == Act::Punch || a == Act::Kick || a == Act::AirAttack || a == Act::CrouchPunch || a == Act::CrouchKick;
}
static const Move& Attack(const Fighter& f) {
    if (f.act == Act::Punch && f.chain > 0) return f.chain == 1 ? f.def->punch2 : f.def->punch3;
    if (f.act == Act::Super) return f.def->super;
    if (f.act == Act::Suitcase) return f.def->suitcase.move;
    if (f.act == Act::Spray) return f.def->spray.move;
    return f.act == Act::Punch ? (f.unarmed ? f.def->punchBare : f.def->punch) : f.act == Act::AirAttack ? f.def->airAttack
         : f.act == Act::CrouchPunch ? f.def->crouchPunch : f.act == Act::CrouchKick ? f.def->crouchKick
         : f.act == Act::Special ? f.def->special : f.unarmed ? f.def->kickBare : f.def->kick;
}

static const Anim* FindAnim(const CharDef& d, const char* name) {
    auto it = d.anims.find(name);
    return it != d.anims.end() && !it->second.boxes.empty() ? &it->second : nullptr;
}

// Pose parada de referência: quadros do andar; personagem ainda sem andar usa o idle.
static const Anim* StandAnim(const CharDef& d) {
    const Anim* a = FindAnim(d, "walk");
    return a ? a : FindAnim(d, "idle");
}

// Anim do lutador: sem a arma, "<nome>_unarmed" quando existe (idle, soco, chute sem o lápis).
static const Anim* FighterAnim(const Fighter& f, const std::string& name) {
    const Anim* a = f.unarmed ? FindAnim(*f.def, (name + "_unarmed").c_str()) : nullptr;
    return a ? a : FindAnim(*f.def, name.c_str());
}

// Pegar a arma do chão: a anim "pickup" uma vez (sem ela, meio segundo).
static int PickupTicks(const CharDef& d) {
    const Anim* a = FindAnim(d, "pickup");
    return a ? std::max(1, AnimTicks(*a, (int)(a->order.empty() ? a->boxes.size() : a->order.size()))) : 30;
}

static int ActiveFrame(const Anim& a) {
    int n = (int)a.boxes.size();
    return a.activeFrame >= 0 && a.activeFrame < n ? a.activeFrame : n / 2;
}

// Animação e quadro do estado atual. Lógica e desenho usam esta MESMA escolha: as caixas medidas na arte são
// sempre as do quadro que está na tela. Sem arte para o estado: quadro 0 do andar (`shove` = passinho à frente).
static const Anim* CurrentAnim(const Fighter& f, int& frame, float& shove) {
    bool attacking = IsAttack(f.act) || f.act == Act::Special || f.act == Act::Super || f.act == Act::Suitcase;  // especiais: mesmo tempo, o corpo não acerta
    bool call = f.def->idleCall > 0 && f.idleT >= f.def->idleCall && FindAnim(*f.def, "idle_call");
    const char* name = f.act == Act::Punch ? (f.chain == 0 ? "punch" : f.chain == 1 ? "punch2" : "punch3")
                     : f.act == Act::Suitcase ? "suitcase" : f.act == Act::Spray ? "spray"
                     : f.act == Act::Super ? "super" : f.act == Act::Dizzy ? "dizzy"
                     : f.act == Act::Dash ? (f.t < 0 && FindAnim(*f.def, "dash_end") ? "dash_end"
                                             : f.t <= f.def->dash.startup && FindAnim(*f.def, "dash_start") ? "dash_start" : "dash") : f.act == Act::Kick ? "kick" : f.act == Act::Special ? "special" : f.act == Act::Parry ? "parry"
                     : f.act == Act::CrouchPunch ? "crouch_punch" : f.act == Act::CrouchKick ? "crouch_kick"
                     : f.act == Act::AirAttack ? "air_attack" : f.act == Act::Hit ? "hit"
                     : f.act == Act::Block ? (f.blockLow ? (FindAnim(*f.def, "block_low") ? "block_low" : "crouch") : "block")
                     : f.act == Act::KO ? "ko" : f.act == Act::Jump ? "jump" : f.act == Act::Pickup ? "pickup"
                     // com "block"/"block_low" na arte, recuar (andando ou agachado) já mostra a guarda (escudo)
                     : f.crouching ? (f.holdBack && FindAnim(*f.def, "block_low") ? "block_low" : "crouch")
                     : f.walking && f.holdBack && FindAnim(*f.def, "block") ? "block"
                     : f.walking ? (FindAnim(*f.def, "walk") ? "walk" : "idle")  // sem andar ainda: o idle anda junto
                     : f.airT < 0 ? (call ? "idle_call" : "idle") : "";  // idle só parado no chão
    if (!FighterAnim(f, name)) {  // golpes novos sem arte própria: a arte mais parecida que existir
        if (!strcmp(name, "punch2") || !strcmp(name, "punch3")) name = "punch";
        else if (!strcmp(name, "super")) name = "special";
        else if (!strcmp(name, "dizzy")) name = "hit";
        else if (!strcmp(name, "dash")) name = FindAnim(*f.def, "walk") ? "walk" : "idle";
    }
    const Anim* anim = FighterAnim(f, name);
    frame = 0;
    shove = 0;
    if (!anim) {
        if (attacking) shove = 5;
        anim = FighterAnim(f, FindAnim(*f.def, "walk") ? "walk" : "idle");  // = StandAnim, sem a arma se for o caso
        if (anim && !anim->order.empty()) frame = anim->order[0];
        return anim;
    }
    // com "sequence", a animação tem tantos passos quanto a sequência; no fim cada passo vira o quadro dela
    int n = anim->order.empty() ? (int)anim->boxes.size() : (int)anim->order.size();
    if (attacking) {
        // quadros antes do `active_frame` cobrem o startup, ele fica na tela enquanto acerta, o resto é recovery
        const Move& m = Attack(f);
        int af = ActiveFrame(*anim), after = n - af - 1;
        if (f.t < m.startup) frame = f.t * af / std::max(1, m.startup);
        else if (f.t < m.startup + m.active || after == 0) frame = af;
        else frame = af + 1 + std::min(after - 1, (f.t - m.startup - m.active) * after / std::max(1, m.recovery));
    } else if (!strcmp(name, "dash_start")) {  // preparo: toca uma vez no tempo do startup
        frame = std::min(n - 1, f.t * n / (f.def->dash.startup + 1));
    } else if (!strcmp(name, "dash_end")) {    // recovery: toca uma vez no tempo do recovery
        frame = std::min(n - 1, (f.t + f.def->dash.recovery) * n / f.def->dash.recovery);
    } else if (f.act == Act::Dash) {
        int run = f.t - f.def->dash.startup;  // preparo e recovery: 1º quadro
        frame = run > 0 ? AnimStep(*anim, 2 * run) % n : 0;  // correndo: o ciclo do andar (ou da corrida) no dobro do ritmo
    } else if (f.act == Act::Spray) {  // 1º quadro = entrada (e recovery); borrifando, os outros em loop
        int run = f.t - f.def->spray.move.startup;
        frame = run > 0 && n > 1 ? 1 + AnimStep(*anim, run - 1) % (n - 1) : 0;
    } else if (call && !strcmp(name, "idle_call")) {
        frame = AnimStep(*anim, f.idleT - f.def->idleCall) % n;  // começa do 1º quadro
    } else if (f.act == Act::Dizzy) {
        frame = AnimStep(*anim, f.animT) % n;  // tonto em loop
    } else if (f.act == Act::Hit) {
        frame = std::clamp((HIT_STUN - f.t) * n / HIT_STUN, 0, n - 1);  // toca uma vez; atordoamento extra segura o 1º
    } else if (f.act == Act::Parry) {
        // antes do `active_frame` = preparo, ele = aparando, o seguinte = sucesso, o último = recovery do erro
        const CharDef::Parry& p = f.def->parry;
        int af = ActiveFrame(*anim);
        if (f.t < p.startup) frame = f.t * af / std::max(1, p.startup);
        else if (f.t < p.startup + p.active) frame = af;
        else frame = f.hitDone ? std::min(af + 1, n - 1) : n - 1;
    } else if (f.act == Act::Jump) {
        // 1º quadro = impulso, 2 últimos = aterrissagem, os do meio cobrem o tempo no ar
        int air = f.def->jumpRise + f.def->jumpFall;
        if (n < 4) frame = std::min(n - 1, f.t * n / (JUMP_PREP + air + JUMP_LAND));
        else if (f.t < JUMP_PREP) frame = 0;
        else if (f.airT < 0) frame = f.landT > JUMP_LAND / 2 ? n - 2 : n - 1;
        else frame = 1 + std::min(n - 4, f.airT * (n - 3) / air);
    } else if (!strcmp(name, "block") || !strcmp(name, "block_low")) {  // ergue a guarda uma vez e segura
        int t = f.act == Act::Block ? 1 << 20 : f.crouching ? f.crouchT : f.walkT;
        frame = std::min(n - 1, AnimStep(*anim, t));
    } else if (f.crouching) {
        frame = std::min(n - 1, AnimStep(*anim, f.crouchT));  // desce e fica no último quadro
    } else if (f.walking) {
        frame = AnimStep(*anim, f.walkT) % n;
        if (f.holdBack) frame = n - 1 - frame;  // andando de costas: ciclo ao contrário, senão o pé "patina"
    } else if (f.act == Act::Pickup) {
        frame = std::min(n - 1, AnimStep(*anim, f.t));
    } else if (f.act == Act::KO) {
        frame = std::min(n - 1, AnimStep(*anim, f.animT));  // cai uma vez e fica no último quadro
    } else {
        frame = AnimStep(*anim, f.animT) % n;  // idle/defesa em loop, pelo relógio do próprio lutador
    }
    if (!anim->order.empty()) frame = anim->order[frame % n];
    return anim;
}

// Colisão do corpo, medida no quadro parado da arte (sem arte, 20% da altura para cada lado):
// para a frente vai até a guarda (punhos), para trás até o fim do tronco (sacola pendurada não empurra ninguém).
static float PushFront(const Fighter& f) { return f.def->pushFront > 0 ? f.def->pushFront : f.def->height * 0.2f; }
static float PushBack(const Fighter& f) { return f.def->pushBack > 0 ? f.def->pushBack : f.def->height * 0.2f; }
static float PushRight(const Fighter& f) { return f.facing > 0 ? PushFront(f) : PushBack(f); }
static float PushLeft(const Fighter& f) { return f.facing > 0 ? PushBack(f) : PushFront(f); }

// x dos pés onde o quadro fica: o desenho inteiro dentro da tela (braço aberto encostado na parede).
// As caixas usam este mesmo x, então desenho e caixas nunca se separam.
// y da base do recorte: o CORPO fica apoiado no chão (pedaço solto abaixo dele, como óculos caindo, vai para
// baixo da linha do chão em vez de deixar o corpo flutuando).
int GROUND_Y = 200;
Platform platform;
Arena arena;
static float WallR() { return arena.cam + SCREEN_W; }  // borda direita da tela (a esquerda é arena.cam)

static float PushLeft(const Fighter& f);
static float PushRight(const Fighter& f);
// Meio entre as bordas dos dois corpos (não dos pés): com os dois encostados nas bordas da tela a câmera fica parada
// (pelo meio dos pés, corpos de larguras diferentes faziam a câmera escorregar até o fim do mundo).
void UpdateCamera(const Fighter& a, const Fighter& b) {
    float l = std::min(a.x - PushLeft(a), b.x - PushLeft(b)), r = std::max(a.x + PushRight(a), b.x + PushRight(b));
    arena.cam = std::clamp((l + r - SCREEN_W) / 2, arena.x0, std::max(arena.x0, arena.x1 - SCREEN_W));
}
std::vector<std::string> sfxQueue;
static void Sfx(const std::string& name) { if (!name.empty()) sfxQueue.push_back(name); }

// Altura do piso sob o lutador: tampo da plataforma (pés sobre ela) ou chão.
static float FloorAt(const Fighter& f) { return platform.top > 0 && f.x < platform.x1 ? platform.top : 0; }

static float FeetY(const Anim& a, int frame, float y) { return GROUND_Y - y + a.boxes[frame % a.boxes.size()].sink; }

// Para que lado o desenho da animação olha: o do lutador, ou o contrário se a arte foi feita espelhada.
static int Face(const Fighter& f, const Anim& a) { return a.mirror ? -f.facing : f.facing; }

static float FrameX(const Fighter& f, const Anim& a, int frame, float x) {
    const FrameBoxes& b = a.boxes[frame % a.boxes.size()];
    int face = Face(f, a);
    float l = face > 0 ? x + b.left : x - b.right, r = face > 0 ? x + b.right : x - b.left;
    if (l < arena.cam) return x - (l - arena.cam);
    if (r > WallR()) return x - (r - WallR());
    return x;
}

// Caixa da arte (olhando para a direita, y a partir dos pés) -> retângulo na tela.
static Rectangle ToWorld(const Box& b, float x, float feetY, int facing) {
    float xa = x + facing * b.x0, xb = x + facing * b.x1;
    return {std::min(xa, xb), feetY - b.y1, fabsf(xb - xa), b.y1 - b.y0};
}

// Onde o lutador pode apanhar: tronco desenhado no quadro atual + o que estiver à frente dele na altura dos
// braços (guarda, braço ou perna esticados). Inclina, agacha e encolhe junto com a arte.
static Rectangle Hurtbox(const Fighter& f) {
    int frame;
    float shove;
    if (const Anim* a = CurrentAnim(f, frame, shove)) {
        const FrameBoxes& b = a->boxes[frame % a->boxes.size()];
        Box hurt = b.body;
        hurt.x1 = std::max(hurt.x1, b.front);
        return ToWorld(hurt, FrameX(f, *a, frame, f.x + f.facing * shove), FeetY(*a, frame, f.y), Face(f, *a));
    }
    float h = f.crouching ? f.def->height * 0.65f : (float)f.def->height;
    return {f.x - PushLeft(f), GROUND_Y - f.y - h, PushLeft(f) + PushRight(f), h};
}

// Área de acerto, só nos ticks `active`: o membro desenhado no quadro de impacto (punho, bolsa, pé).
// Sem arte do golpe: faixa de `range` px à frente do corpo na altura `hit_height` (valores do JSON).
static bool ActiveHitbox(const Fighter& f, Rectangle& box) {
    if (!IsAttack(f.act)) return false;
    const Move& m = Attack(f);
    if (f.t < m.startup || f.t >= m.startup + m.active) return false;
    int frame;
    float shove;
    const Anim* a = CurrentAnim(f, frame, shove);
    if (a && shove == 0) {
        const FrameBoxes& b = a->boxes[frame % a->boxes.size()];
        if (b.hasHit) {
            box = ToWorld(b.hit, FrameX(f, *a, frame, f.x), FeetY(*a, frame, f.y), Face(f, *a));
            return true;
        }
    }
    float front = f.x + f.facing * (PushFront(f) - 4);
    float y = GROUND_Y - f.y - f.def->height * m.hitHeight;
    box = {f.facing > 0 ? front : front - m.range, y - 6, (float)m.range, 12};
    return true;
}

// Até onde o golpe alcança a partir do centro do corpo (a CPU usa para decidir quando atacar).
static float Reach(const Fighter& f, bool punch) {
    if (const Anim* a = FighterAnim(f, punch ? "punch" : "kick")) {
        const FrameBoxes& b = a->boxes[ActiveFrame(*a)];
        if (b.hasHit) return b.hit.x1;
    }
    bool bare = f.unarmed;
    return PushFront(f) - 4 + (punch ? (bare ? f.def->punchBare : f.def->punch) : (bare ? f.def->kickBare : f.def->kick)).range;
}

Fighter MakeFighter(const CharDef& def, float x, Color color) {
    Fighter f{};
    f.def = &def;
    f.color = color;
    f.x = x;
    f.facing = x < (arena.x0 + arena.x1) / 2 ? 1 : -1;
    f.hp = def.health;
    f.act = Act::Free;
    f.airT = -1;
    f.prevX = x;
    f.downT = f.motionT = f.backT = 1000;  // nenhum comando em andamento
    f.petChoice = -1;
    f.slideFriction = 0.9f;
    f.fxT = -1;
    f.djT = -1;
    f.clashT = -1;
    f.tapT = 1000;  // nenhum toque para a frente ainda (senão o 1º toque do round já virava arrancada)
    f.jumpScale = 1;
    f.spamMove = -1;
    f.warnT = -1;
    f.guardT = 1 << 20;
    f.spamT = 1 << 20;
    f.spamScale = 1;
    f.spray = 100;
    f.tauntT = -1;
    f.tauntGapT = 1 << 20;
    return f;
}

// Altura no pulo após `t` ticks no ar. Subida e descida são meias-parábolas exatas:
// sobe `height` em `rise` ticks (g = 2H/rise²) e desce em `fall` ticks (g = 2H/fall², mais forte).
// Depois do fim da descida a parábola continua para baixo (pulou da plataforma e cai até o chão).
static float JumpArc(const CharDef& d, int t) {
    float H = d.jumpHeight;
    if (t <= d.jumpRise) { float s = 1 - (float)t / d.jumpRise; return H * (1 - s * s); }
    float s = (float)(t - d.jumpRise) / d.jumpFall;
    return H * (1 - s * s);
}
float JumpY(const CharDef& d, int t) { return std::max(0.0f, JumpArc(d, t)); }

static void Fire(Fighter& f);
static void FireSuper(Fighter& f);
static void ThrowClothes(Fighter& f);
static void SpawnPuff(Fighter& f);
static int FxTicks(const CharDef& d);

// Golpe começando (f.act já é ele): o mesmo de novo dentro de repeatReset ticks conta como repetição e tira dano
// (100%, 80%, 60%... até repeatMin); outro golpe zera. Conta o uso, acertando ou não: spammar não compensa.
static void StartMove(Fighter& f) {
    const CharDef& d = *f.def;
    int id = (int)f.act * 4 + (f.act == Act::Punch ? f.chain : 0);  // cada soco da corrente conta separado
    f.spamCount = id == f.spamMove && f.spamT <= d.repeatReset ? f.spamCount + 1 : 0;
    f.spamMove = id;
    f.spamT = 0;
    f.spamScale = std::max(d.repeatMin, 1 - d.repeatStep * f.spamCount);
    // balão de provocação: sempre no especial; nos outros golpes com `tauntChance`, longe do último balão
    if (!d.tauntText.empty() && (f.act == Act::Special || (f.tauntGapT >= d.tauntGap && GetRandomValue(1, 100) <= d.tauntChance * 100))) {
        f.tauntT = 0;
        f.tauntGapT = 0;
    }
}
static int Damage(const Fighter& f, int base) { return std::max(1, (int)lroundf(base * f.spamScale)); }

// Arma solta (lápis arremessado): quica para trás e cai até o piso embaixo dela (chão ou tampo da mesa).
static void UpdateWeapon(Fighter& f) {
    auto& w = f.weapon;
    if (!w.on) return;
    w.t++;
    if (w.down) return;
    w.x = std::clamp(w.x + w.vx, arena.cam + 8, WallR() - 8);
    w.vh -= 0.25f;
    w.h += w.vh;
    float floor = platform.top > 0 && w.x < platform.x1 ? platform.top : 0;
    if (w.vh < 0 && w.h <= floor) { w.h = floor; w.down = true; Sfx("queda"); }
}

static void UpdateOne(Fighter& f, const Input& in, const Fighter& foe) {
    f.animT++;
    UpdateWeapon(f);
    if (f.flash > 0) f.flash--;
    f.x += f.push;
    f.push = fabsf(f.push) < 0.1f ? 0 : f.push * 0.7f;
    f.x += f.slide;  // latido: empurrão longo, que vai parando pelo atrito
    f.slide = fabsf(f.slide) < 0.05f ? 0 : f.slide * f.slideFriction;
    if (f.airT < 0 && f.y > FloorAt(f)) {  // saiu da plataforma: cai do alto da parábola, no pulo
        f.airT = f.def->jumpRise;
        f.jumpBase = f.y - f.def->jumpHeight;
        f.jumpScale = 1;
        f.doubleUsed = false;
        f.vx = 0;
        if (f.act == Act::Free) { f.act = Act::Jump; f.t = JUMP_PREP + 1; f.airUsed = false; }
    }
    // no ar (vale também para quem apanhou no ar): altura exata pela curva, sem erro acumulado; na descida
    // aterrissa no piso embaixo dele (chão ou plataforma)
    if (f.airT >= 0) {
        f.airT++;
        f.x += f.vx;
        f.y = f.jumpBase + JumpArc(*f.def, f.airT) * f.jumpScale;
        if (f.airT > f.def->jumpRise && f.y <= FloorAt(f)) { f.y = FloorAt(f); f.vx = 0; f.airT = -1; f.landT = JUMP_LAND; }
    } else if (f.landT > 0) {
        f.landT--;
    }
    int dir = in.right - in.left;
    // comando do especial: ↓, depois → (para quem está olhando; ↘ no meio vale), depois soco
    f.downT = in.down ? 0 : f.downT + 1;
    f.motionT = dir == f.facing && f.downT <= MOTION_WINDOW ? 0 : f.motionT + 1;
    f.backT = dir == -f.facing && f.downT <= MOTION_WINDOW ? 0 : f.backT + 1;  // ↙ junto também vale
    // arrancada: tocar para a frente 2 vezes em até 12 ticks
    bool fwdPress = dir == f.facing && f.lastDir != f.facing;
    bool dashCmd = fwdPress && f.tapT <= 12;
    f.tapT = fwdPress ? 0 : std::min(f.tapT + 1, 1000);
    f.lastDir = dir;
    // furacão: ←, ↓, → (meia-lua para a frente; ↙ e ↘ no meio valem), cada passo em até 12 ticks; chute em até 10
    f.hcfT++;
    if (dir == -f.facing && !in.down) { f.hcfStage = 1; f.hcfT = 0; }
    else if (f.hcfStage == 1 && in.down) { f.hcfStage = 2; f.hcfT = 0; }
    else if (f.hcfStage == 2 && dir == f.facing) { f.hcfStage = 3; f.hcfT = 0; }
    if ((f.hcfStage < 3 && f.hcfT > 12) || (f.hcfStage == 3 && f.hcfT > MOTION_WINDOW)) f.hcfStage = 0;
    if (in.cmd == 1) f.motionT = 0;  // controle de toque: o botão do golpe vem com o comando pronto
    if (in.cmd == 2) f.backT = 0;
    if (in.cmd == 3) { f.hcfStage = 3; f.hcfT = 0; }
    dashCmd |= in.cmd == 4;
    f.punchBufT = in.punch ? 0 : std::min(f.punchBufT + 1, 1000);
    if (f.parryCD > 0) f.parryCD--;
    if (f.suitcaseCD > 0) f.suitcaseCD--;
    if (f.specialCD > 0) f.specialCD--;
    if (f.tauntT >= 0 && ++f.tauntT >= f.def->tauntTicks) f.tauntT = -1;
    if (f.tauntGapT < (1 << 20)) f.tauntGapT++;
    if (f.def->spray.on && !(f.act == Act::Spray && f.t > f.def->spray.move.startup)) {  // recarrega fora do borrifo
        f.spray = std::min(100.0f, f.spray + f.def->spray.recharge);
        if (f.spray >= f.def->spray.restart) f.sprayLock = false;
    }
    if (f.fxT >= 0 && ++f.fxT >= FxTicks(*f.def)) f.fxT = -1;
    if (f.djT >= 0 && ++f.djT >= 18) f.djT = -1;
    if (f.clashT >= 0 && ++f.clashT >= 20) f.clashT = -1;
    if (f.spamT < (1 << 20)) f.spamT++;
    if (f.warnT >= 0 && ++f.warnT >= 75) f.warnT = -1;
    if (f.guardT < (1 << 20) && ++f.guardT > f.def->guardDelay) f.guard = std::max(0.0f, f.guard - f.def->guardRegen / 60);
    f.walking = false;
    f.crouching = false;
    switch (f.act) {
    case Act::Jump:
        if (++f.t == JUMP_PREP) {
            f.airT = 0;
            f.jumpBase = f.y;
            f.vx = f.jumpDir * f.def->jumpSpeed / 60;
            f.airUsed = false;
            f.jumpScale = 1;
            f.doubleUsed = false;
        } else if (f.t > JUMP_PREP && f.airT < 0 && f.landT == 0) {
            f.act = Act::Free;
        } else if (f.airT >= 0 && in.up && !f.upHeld && !f.doubleUsed && f.def->doubleJump > 0) {
            // pulo duplo: nova parábola a partir de onde está, na direção segurada agora; anel de ar no lugar
            f.doubleUsed = true;
            f.jumpBase = f.y;
            f.airT = 0;
            f.jumpScale = f.def->doubleJump;
            f.vx = dir * f.def->jumpSpeed / 60;
            f.airUsed = false;
            f.djT = 0;
            f.djX = f.x;
            f.djY = f.y;
            Sfx("whoosh");
        } else if (f.airT >= 0 && !f.airUsed && (in.punch || in.kick)) {
            f.act = Act::AirAttack;  // golpe no ar: um por pulo; a parábola continua igual
            StartMove(f);
            Sfx("whoosh");
            Sfx(f.def->airAttack.voice);
            f.t = 0;
            f.hitDone = false;
            f.airUsed = true;
        }
        break;
    case Act::AirAttack: {
        const Move& m = f.def->airAttack;
        // aterrissou (golpe cortado) ou terminou ainda no ar: volta ao pulo, que cuida da queda e da aterrissagem
        if (f.airT < 0 || ++f.t >= m.startup + m.active + m.recovery) { f.act = Act::Jump; f.t = JUMP_PREP + 1; }
        break;
    }
    case Act::Block:
        f.blockLow = in.down;  // na defesa dá para trocar entre em pé e agachado
        f.crouching = f.blockLow;
        if (--f.t <= 0) f.act = Act::Free;
        break;
    case Act::Hit:
        if (--f.t <= 0) f.act = Act::Free;
        break;
    case Act::Dash:  // t 1..startup = preparo parado; depois corre; t < 0 = recovery parado (punível)
        if (f.t < 0) {
            if (++f.t < 0) break;
            int frame;  // acabou: a fumaça cobre o desenho do recovery (o carro some)
            float shove;
            const Anim* a = FindAnim(*f.def, "smoke") ? CurrentAnim(f, frame, shove) : nullptr;
            if (a) {
                const FrameBoxes& b = a->boxes[frame % a->boxes.size()];
                f.fxT = 0;
                f.fxX = f.x + Face(f, *a) * (b.left + b.right) / 2;
                f.fxY = GROUND_Y - f.y - b.body.y1 / 2;
            }
            f.act = Act::Free;
            break;
        }
        if (dir == -f.facing || ++f.t >= f.def->dash.startup + f.def->dash.maxTicks) {  // trás freia
            f.t = -f.def->dash.recovery;
            break;
        }
        if (f.t > f.def->dash.startup)
            f.x += f.facing * f.def->dash.speed / 60;  // bate na borda ou no oponente: DashContact, depois do Separate
        break;
    case Act::Dizzy:
        if (--f.t <= 0) f.act = Act::Free;
        break;
    case Act::Spray: {  // t <= startup = entrada; borrifa enquanto segura o soco e tem perfume; t < 0 = recovery
        const CharDef::Spray& s = f.def->spray;
        if (f.t < 0) { if (++f.t >= 0) f.act = Act::Free; break; }
        if (++f.t <= s.move.startup) break;
        if (!in.punchHeld || f.sprayLock) { f.t = -std::max(1, s.move.recovery); break; }
        f.spray = std::max(0.0f, f.spray - s.drain);
        if (f.spray <= 0) f.sprayLock = true;  // vazio: este é o último tick borrifando
        if ((f.t - s.move.startup - 1) % s.puffEvery == 0) SpawnPuff(f);
        break;
    }
    case Act::Super: {
        const Move& m = f.def->super;
        if (++f.t == std::max(1, m.startup)) FireSuper(f);
        if (f.t >= m.startup + m.active + m.recovery) { f.act = Act::Dizzy; f.t = f.def->dizzyTicks; }  // tonto: punível
        break;
    }
    case Act::Punch:
    case Act::Kick:
    case Act::Special:
    case Act::Suitcase:
    case Act::CrouchPunch:
    case Act::CrouchKick: {
        const Move& m = Attack(f);
        // corrente: o soco acertou e J foi apertado (até 10 ticks antes): sai o próximo da sequência na hora
        if (f.act == Act::Punch && f.chainOk && f.punchBufT <= 10 && f.chain + 1 < f.def->chainLen) {
            f.chain++;
            f.t = 0;
            f.hitDone = f.chainOk = false;
            StartMove(f);
            Sfx("whoosh");
            Sfx(Attack(f).voice);
            break;
        }
        // golpe agachado: continua abaixado (caixa de dano baixa) e, segurando baixo, volta direto ao agachado
        f.crouching = f.act == Act::CrouchPunch || f.act == Act::CrouchKick;
        if (++f.t == std::max(1, m.startup)) {  // solta no quadro de lançamento
            if (f.act == Act::Special) Fire(f);
            if (f.act == Act::Suitcase) ThrowClothes(f);
        }
        // especial: recovery longo enquanto o projétil não acertou nem foi defendido (errar é punível)
        int rec = f.act == Act::Special && !f.shotLanded ? f.def->specialWhiff : m.recovery;
        if (f.t >= m.startup + m.active + rec) f.act = Act::Free;
        break;
    }
    case Act::Free:
        if (f.airT >= 0) break;  // atordoamento acabou no ar: só cai
        f.facing = foe.x >= f.x ? 1 : -1;
        if (f.weapon.down && fabsf(f.y - f.weapon.h) < 1 && fabsf(f.x - f.weapon.x) < 16) {  // passou pela arma: pega
            f.act = Act::Pickup;
            f.t = 0;
            f.facing = f.weapon.x >= f.x ? 1 : -1;
            break;
        }
        if (in.punch && f.backT <= MOTION_WINDOW && f.def->parry.on && f.parryCD == 0) {
            f.act = Act::Parry;
            f.t = 0;
            f.hitDone = false;  // aqui: true = já aparou algo
        } else if (in.punch && f.motionT <= MOTION_WINDOW && f.def->hasSpecial && !f.shot.on && !f.unarmed && f.specialCD == 0) {  // um projétil por vez; senão, soco
            f.act = Act::Special;
            f.t = 0;
            f.shotLanded = false;
            f.specialCD = f.def->specialCooldown;
            StartMove(f);
            Sfx(f.def->special.voice);
        } else if (in.kick && f.hcfStage == 3 && f.def->dash.on && f.def->dash.halfCircle) {  // carro: ← ↓ → chute
            f.act = Act::Dash;
            f.t = 0;
            f.hcfStage = 0;
            StartMove(f);
        } else if (in.kick && f.motionT <= MOTION_WINDOW && f.def->suitcase.on && f.suitcaseCD == 0) {  // mala: ↓ → chute
            f.act = Act::Suitcase;
            f.t = 0;
            f.suitcaseCD = f.def->suitcase.cooldown;
            StartMove(f);
            Sfx(f.def->suitcase.move.voice);
        } else if (in.kick && f.hcfStage == 3 && f.def->hasSuper && !f.shot.on) {  // furacão (um projétil por vez)
            f.act = Act::Super;
            f.t = 0;
            f.hcfStage = 0;
            StartMove(f);
            Sfx(f.def->super.voice);
        } else if (dashCmd && f.def->dash.on && !f.def->dash.halfCircle) {
            f.act = Act::Dash;
            f.t = 0;
            StartMove(f);
            Sfx("whoosh");
        } else if (in.punch && in.down && f.def->spray.on && !f.sprayLock) {  // borrifo: ↓ + soco, segurando o soco
            f.act = Act::Spray;
            f.t = 0;
            StartMove(f);
            Sfx(f.def->spray.move.voice);
        } else if (in.punch || in.kick) {
            Sfx("whoosh");
            f.chain = 0;
            f.chainOk = false;
            bool low = in.down && (in.punch ? f.def->hasCrouchPunch : f.def->hasCrouchKick);
            if (low && f.unarmed && !FindAnim(*f.def, in.punch ? "crouch_punch_unarmed" : "crouch_kick_unarmed"))
                low = false;  // sem a arma e sem arte agachada sem ela: levanta e golpeia (tapinha/empurrão)
            f.act = low ? (in.punch ? Act::CrouchPunch : Act::CrouchKick) : in.punch ? Act::Punch : Act::Kick;
            f.crouching = low;  // já no 1º tick do golpe agachado a caixa de dano fica baixa
            StartMove(f);
            Sfx(Attack(f).voice);
            f.t = 0;
            f.hitDone = false;
        } else if (in.up) {
            f.act = Act::Jump;  // direção fica travada no impulso
            f.t = 0;
            f.jumpDir = dir;
        } else if (in.down) {
            f.crouching = true;  // agachado não anda; segurar para trás junto defende
        } else if (dir) {
            f.x += dir * f.def->walkSpeed / 60;
            f.walking = true;
        }
        break;
    case Act::Parry: {
        const CharDef::Parry& p = f.def->parry;
        if (++f.t >= p.startup + p.active + (f.hitDone ? p.recovery : p.whiffRecovery)) {
            f.act = Act::Free;
            if (!f.hitDone) f.parryCD = p.cooldown;  // errou: não dá para tentar de novo logo
        }
        break;
    }
    case Act::Pickup:
        if (++f.t >= PickupTicks(*f.def)) { f.act = Act::Free; f.unarmed = false; f.weapon = {}; }
        break;
    case Act::KO: {  // corpo bate no chão quando a queda chega ao último quadro
        const Anim* a = FindAnim(*f.def, "ko");
        if (f.animT == (a && a->fps > 0 ? AnimTicks(*a, (int)a->boxes.size() - 1) : 30)) Sfx("queda");
        break;
    }
    }
    f.walkT = f.walking ? f.walkT + 1 : 0;
    if (const Anim* w = f.walking ? FindAnim(*f.def, "walk") : nullptr) {  // passo no pé que encosta (quadro 0 e metade)
        int n = (int)w->boxes.size(), a = AnimStep(*w, f.walkT - 1) % n, b = AnimStep(*w, f.walkT) % n;
        if (a != b && (b == 0 || b == n / 2)) Sfx(f.def->steps + "_" + std::to_string(GetRandomValue(1, 3)));
    }
    f.crouchT = f.crouching ? f.crouchT + 1 : 0;
    f.holdBack = dir == -f.facing;
    f.upHeld = in.up;
    // idle_call: parada no chão, sem comando e sem luta acontecendo (oponente sem golpe, nenhum projétil)
    bool quiet = f.act == Act::Free && f.airT < 0 && !dir && !in.down && !in.up && !in.punch && !in.kick && !in.punchHeld &&
                 !in.support && (foe.act == Act::Free || foe.act == Act::Jump) && !f.shot.on && !foe.shot.on;
    f.idleT = quiet ? f.idleT + 1 : 0;
}

// Limite da esquerda: borda da tela ou, abaixo do tampo, a lateral da plataforma (os pés não entram embaixo dela;
// o corpo fica na frente da mesa). Pelo centro dos pés, como o apoio em FloorAt: sair andando não dá tranco.
// Não depende do x atual: encostado exatamente na lateral ainda conta como parede (senão o empurrão do outro jogava
// o lutador para baixo do tampo, e um K.O. ali o deixava deitado em cima da mesa).
static float MinX(const Fighter& f) {
    return platform.top > 0 && f.y < platform.top ? std::max(platform.x1, arena.cam + PushLeft(f)) : arena.cam + PushLeft(f);
}

// Altura do topo da caixa de dano acima dos pés (em pé, agachado: a do quadro atual).
static float HurtTop(const Fighter& f) { return GROUND_Y - f.y - Hurtbox(f).y; }

// Pular por cima: só quem, ao encostar no outro, está com os pés acima da caixa de dano INTEIRA dele (pulo normal
// passa por cima de quem está agachado; de quem está em pé, só com o pulo duplo). Então atravessa e, ao
// aterrissar, cai do outro lado (no sentido em que ia), mesmo encostado na parede. Pés abaixo da cabeça: bate e volta.
static void PassOver(Fighter& f, Fighter& o, float gap) {
    bool touching = fabsf(f.x - o.x) < gap;
    if (f.airT >= 0) {
        if (!f.passDir && touching && f.y - o.y > HurtTop(o)) f.passDir = f.x <= o.x ? 1 : -1;
        else if (f.passDir && !touching && (o.x - f.x) * f.passDir < 0) f.passDir = 0;  // já passou: segue normal
    } else if (f.passDir) {  // aterrissou: do outro lado de quem pulou por cima
        if (touching) f.x = o.x + f.passDir * gap;
        f.passDir = 0;
    }
}

// Não se atravessam e não saem da tela (nem entram na plataforma por baixo); quem está na parede empurra o outro.
// Usa a largura do corpo medida na arte: os corpos encostam, só braço/perna esticados passam por cima.
static void Separate(Fighter& a, Fighter& b) {
    float gap0 = a.x <= b.x ? PushRight(a) + PushLeft(b) : PushRight(b) + PushLeft(a);
    PassOver(a, b, gap0);
    PassOver(b, a, gap0);
    Fighter& l = a.x <= b.x ? a : b;
    Fighter& r = a.x <= b.x ? b : a;
    float gap = PushRight(l) + PushLeft(r);  // distância mínima entre os dois centros
    bool over = (a.passDir && a.airT >= 0) || (b.passDir && b.airT >= 0);
    float overlap = gap - (r.x - l.x);
    if (overlap > 0 && !over) { l.x -= overlap / 2; r.x += overlap / 2; }
    l.x = std::clamp(l.x, MinX(l), WallR() - PushRight(l));
    r.x = std::clamp(r.x, MinX(r), WallR() - PushRight(r));
    if (r.x - l.x < gap && !over) {
        if (l.x <= MinX(l)) r.x = l.x + gap;
        else l.x = r.x - gap;
    }
}

static bool Hits(const Fighter& at, const Fighter& df) {
    Rectangle box;
    return !at.hitDone && ActiveHitbox(at, box) && CheckCollisionRecs(box, Hurtbox(df));
}

// Aviso no HUD do lutador (o jogo mostra o texto ao lado da barra dele).
static void Warn(Fighter& f, int kind, int pct = 0) {
    f.warnT = 0;
    f.warnKind = kind;
    f.warnPct = pct;
}

// dir = para onde o golpe empurra (+1 direita). Defende quem segura para trás no chão (ou já está defendendo),
// na altura certa: golpe baixo só agachado, golpe no ar (alto) só em pé, o resto dos dois jeitos.
static void ApplyHit(int dir, Fighter& df, int damage, Guard guard, int& hitstop, const std::string& sfx, float push = 3) {
    bool guarding = df.act == Act::Block || (df.act == Act::Free && df.holdBack && df.airT < 0);
    bool low = df.crouching;
    bool blocked = guarding && !(guard == Guard::Low && !low) && !(guard == Guard::High && low);
    df.blockLow = blocked && low;
    Sfx(blocked ? "defesa" : !sfx.empty() ? sfx : damage >= 9 ? "soco_forte" : "soco_fraco");
    bool broke = false;  // barra de guarda: cada golpe defendido enche; cheia, a guarda quebra e ele fica aberto
    if (blocked && df.def->guardPerDamage > 0) {
        df.guard += damage * df.def->guardPerDamage;
        df.guardT = 0;
        if ((broke = df.guard >= 100)) df.guard = 0;
    }
    if (blocked) {
        // dano defendido relativo à barra de quem defende: (dano em % da barra) x blockChip x vida máxima.
        // A fração que não fecha 1 ponto fica guardada; defesa nunca derruba (no mínimo 1 de vida).
        df.chipRest += damage / 100.0f * df.def->health * df.def->blockChip;
        damage = std::min((int)df.chipRest, df.hp - 1);
        df.chipRest -= (int)df.chipRest;
    }
    df.hp = std::max(0, df.hp - damage);
    df.act = df.hp == 0 ? Act::KO : blocked && !broke ? Act::Block : Act::Hit;
    df.t = broke ? df.def->guardBreak : blocked ? BLOCK_STUN : HIT_STUN;
    df.push = dir * push;
    df.flash = blocked && !broke ? 0 : 2;
    if (broke) { df.blockLow = false; Sfx("tilintar"); Warn(df, 2); }
    if (df.act == Act::KO) { Sfx("ko"); Sfx(df.def->koVoice); }
    else if (!blocked) Sfx(df.def->hurtVoice);
    if (df.act == Act::KO) { df.y = FloorAt(df); df.vx = 0; df.airT = -1; df.animT = 0; }  // cai direto no piso; queda do 1º quadro
    // golpe forte (9+, sem defender) ou K.O. com a arma na mão: ela voa e cai no chão
    if (df.def->dropsWeapon && !df.unarmed && (df.act == Act::KO || (!blocked && damage >= 9))) {
        df.unarmed = true;
        df.weapon = {true, false, df.x, df.y + df.def->height * 0.6f, dir * 1.5f, 2.5f, dir, 0};
    }
    hitstop = 4;
}

// ---------- projétil do especial ----------

static int ShotFrame(const Anim& a, int t) { return AnimStep(a, t) % (int)a.boxes.size(); }

// Onde o projétil acerta: o "tronco" medido no quadro (a bola com a cruz; o rastro fino atrás não acerta),
// centrado em (x, y). Sem arte: quadrado de 12 px.
static const char* ShotAnimName(const Shot& s, bool impact) {
    if (s.kind == 2) return impact ? "cloud" : "projectile";
    return s.kind == 1 ? (impact ? "super_impact" : "super_projectile") : (impact ? "impact" : "projectile");
}

// Nuvem da bomba: o desenho inteiro do quadro, centrado em (x, y). Sem arte: 48 x 36 px.
static Rectangle CloudBox(const Shot& s) {
    const Anim* a = FindAnim(*s.art, "cloud");
    if (!a) return {s.x - 24, s.y - 18, 48, 36};
    const FrameBoxes& b = a->boxes[AnimStep(*a, std::max(0, s.impactT)) % a->boxes.size()];
    return ToWorld({b.left, b.body.y0, b.right, b.body.y1}, s.x, s.y + (b.body.y0 + b.body.y1) / 2, s.dir);
}

static Rectangle ShotBox(const Shot& s) {
    if (s.kind == 1 && !FindAnim(*s.art, "super_projectile")) return {s.x - 14, s.y - 22, 28, 44};  // redemoinho sem arte
    if (const Anim* a = FindAnim(*s.art, ShotAnimName(s, false))) {
        const Box& b = a->boxes[ShotFrame(*a, s.t)].body;
        return ToWorld(b, s.x, s.y + (b.y0 + b.y1) / 2, s.dir);
    }
    return {s.x - 6, s.y - 6, 12, 12};
}

// Ponta da mão no quadro atual: frente do membro medido na arte (punho, frasco), na altura do meio dele.
// Sem arte: à frente do corpo, na altura `hitHeight`.
static Vector2 HandTip(const Fighter& f, float hitHeight) {
    Vector2 p = {f.x + f.facing * (PushFront(f) + 4), GROUND_Y - f.y - f.def->height * hitHeight};
    int frame;
    float shove;
    const Anim* a = CurrentAnim(f, frame, shove);
    if (a && shove == 0 && a->boxes[frame % a->boxes.size()].hasHit) {
        Rectangle r = ToWorld(a->boxes[frame % a->boxes.size()].hit, FrameX(f, *a, frame, f.x), FeetY(*a, frame, f.y), Face(f, *a));
        p = {f.facing > 0 ? r.x + r.width : r.x, r.y + r.height / 2};
    }
    return p;
}

// Solta o projétil da mão: ponta do membro medida no quadro de lançamento (`active_frame` da anim `special`).
static void Fire(Fighter& f) {
    Vector2 h = HandTip(f, f.def->special.hitHeight);
    float hx = h.x, hy = h.y;
    const Anim* pa = FindAnim(*f.def, "projectile");
    if (f.def->shotFromGround && pa) {  // pés do projétil no piso, logo à frente do corpo
        const Box& b = pa->boxes[0].body;
        hx = f.x + f.facing * PushFront(f);
        hy = GROUND_Y - f.y - (b.y0 + b.y1) / 2;
    }
    f.shot = {true, hx, hx, hy, f.facing, 0, -1, f.def->shotSpeed, Damage(f, f.def->special.damage), f.def};
    f.shot.prevY = hy;
    if (f.def->shotGravity > 0) { f.shot.kind = 2; f.shot.vy = -f.def->shotLift; }  // bomba em arco
    f.unarmed = f.def->dropsWeapon;
    Sfx(f.def->launchSfx);
}

// Furacão: redemoinho no chão à frente, na altura do corpo; acerta várias vezes enquanto avança.
static void FireSuper(Fighter& f) {
    float x = f.x + f.facing * (PushFront(f) + 12), y = GROUND_Y - f.y - 24;
    f.shot = {true, x, x, y, f.facing, 0, -1, f.def->superSpeed, Damage(f, f.def->super.damage), f.def, 1, f.def->superHits, 0, 0, y};
    Sfx("whoosh");
}

static int ImpactTicks(const Shot& s) {
    const CharDef& d = *s.art;
    if (s.kind == 2) return d.cloudTicks;
    const Anim* a = FindAnim(d, ShotAnimName(s, true));
    return a ? AnimTicks(*a, (int)a->boxes.size()) : 12;
}

// ---------- parry ----------

// Efeito por cima do lutador: brilho do parry, ou a fumaça do fim da arrancada (carro do Lui).
static const char* FxName(const CharDef& d) { return d.parry.on ? "parry_flash" : "smoke"; }

static int FxTicks(const CharDef& d) {
    const Anim* a = FindAnim(d, FxName(d));
    return a ? AnimTicks(*a, (int)a->boxes.size()) : 12;
}

// Caixa do parry, só nos ticks `active`, medida no quadro `active_frame` da anim `parry`: do centro do tronco até
// a frente do desenho (bolsa erguida + mão), de 30% a 90% da altura — a bolsa larga encostada no peito conta como
// tronco na medição dos golpes, por isso a caixa é a metade da frente do corpo. Golpe nessa faixa que chegaria ao
// corpo passa antes por ela. Sem arte: faixa de 22 px à frente do corpo, 30%–90% da altura.
static bool ParryBox(const Fighter& f, Rectangle& box) {
    const CharDef::Parry& p = f.def->parry;
    if (f.act != Act::Parry || f.hitDone || f.t < p.startup || f.t >= p.startup + p.active) return false;
    int frame;
    float shove;
    const Anim* a = CurrentAnim(f, frame, shove);
    if (a && a == FindAnim(*f.def, "parry")) {
        const FrameBoxes& b = a->boxes[frame % a->boxes.size()];
        Box bx = {(b.body.x0 + b.body.x1) / 2, b.body.y1 * 0.3f, std::max(b.right, b.front), b.body.y1 * 0.9f};
        box = ToWorld(bx, FrameX(f, *a, frame, f.x), FeetY(*a, frame, f.y), Face(f, *a));
    } else {
        float h = (float)f.def->height;
        box = ToWorld({PushFront(f) - 4, h * 0.3f, PushFront(f) + 18, h * 0.9f}, f.x, GROUND_Y - f.y, f.facing);
    }
    return true;
}

// Aparou: pula para o quadro de sucesso com o recovery curto, brilho na bolsa e hitstop longo.
static void ParrySuccess(Fighter& p, Rectangle box, int& hitstop) {
    p.hitDone = true;
    p.t = p.def->parry.startup + p.def->parry.active;
    p.fxT = 0;
    p.fxX = box.x + box.width / 2;
    p.fxY = box.y + box.height / 2;
    hitstop = p.def->parry.hitstop;
    Sfx("tilintar");
    Sfx(p.def->parry.voice);
}

// Golpe corpo a corpo que toca a caixa do parry (mesmo que também toque o corpo: o parry vence): o golpe não
// acerta e o atacante leva o próprio dano, empurrado para trás e atordoado por mais tempo.
static void TryParry(Fighter& p, Fighter& at, int& hitstop) {
    Rectangle pb, hb;
    if (at.hitDone || !ParryBox(p, pb) || !ActiveHitbox(at, hb) || !CheckCollisionRecs(pb, hb)) return;
    at.hitDone = true;
    ApplyHit(p.facing, at, Attack(at).damage, Guard::Mid, hitstop, Attack(at).sfx);
    if (at.act == Act::Hit) at.t += p.def->parry.stunBonus;
    ParrySuccess(p, pb, hitstop);
}

// A arma do projétil vira item solto: quica para trás de onde acertou ou bateu na parede. É de quem está sem ela
// (rebatida pelo parry, o projétil passou para o outro).
static void DropWeapon(Fighter& f, Fighter& foe, const Shot& s) {
    Fighter& o = f.def == s.art && f.unarmed ? f : foe;
    o.weapon = {true, false, s.x, GROUND_Y - s.y, -s.dir * 1.0f, 2.0f, s.dir, 0};
}

// Bomba estourou: vira nuvem parada ali; no piso (floorY > 0), pousada nele. Conta como acerto para o recovery.
static void Burst(Fighter& f, Shot& s, float floorY) {
    s.impactT = 0;
    f.shotLanded = true;
    float h = 36;
    if (const Anim* a = FindAnim(*s.art, "cloud"))
        for (const FrameBoxes& b : a->boxes) h = std::max(h, b.body.y1);
    if (floorY > 0) s.y = s.prevY = floorY - h / 2;
}

// Voa em linha reta; acerta quem estiver na frente (defesa vale), para ali e toca o impacto; some fora da tela.
// Na caixa de um parry: volta mais rápido e passa a ser de quem aparou. canHit = false depois do K.O.
static void UpdateShot(Fighter& f, Fighter& foe, int& hitstop, bool canHit) {
    Shot& s = f.shot;
    if (!s.on) return;
    if (s.impactT >= 0) {
        // nuvem da bomba: dano a cada cloudEvery ticks em quem estiver dentro (quem jogou não sofre)
        if (s.kind == 2 && canHit && foe.act != Act::KO && s.art->cloudDamage > 0 && s.impactT % s.art->cloudEvery == 0 &&
            CheckCollisionRecs(CloudBox(s), Hurtbox(foe))) {
            int noStop = 0;  // gás: sem pausa de acerto, senão a luta engasga a cada tique
            ApplyHit(foe.x >= s.x ? 1 : -1, foe, s.art->cloudDamage, Guard::Mid, noStop, s.art->special.sfx);
        }
        if (++s.impactT >= ImpactTicks(s)) s.on = false;
        return;
    }
    s.t++;
    s.x += s.dir * s.speed / 60;
    if (s.kind == 2) {  // bomba: arco com gravidade; no piso (chão ou tampo da mesa) estoura
        s.vy += s.art->shotGravity;
        s.y += s.vy;
        float floor = GROUND_Y - (platform.top > 0 && s.x < platform.x1 ? platform.top : 0);
        Rectangle b = ShotBox(s);
        if (s.vy > 0 && b.y + b.height >= floor) { Burst(f, s, floor); return; }
    }
    if (s.hitCD > 0) s.hitCD--;
    if (s.art->dropsWeapon && (s.x < arena.cam + 8 || s.x > WallR() - 8)) { s.on = false; DropWeapon(f, foe, s); return; }  // parede
    if (s.x < arena.cam - 40 || s.x > WallR() + 40) { s.on = false; return; }
    Rectangle pb;
    if (canHit && ParryBox(foe, pb) && CheckCollisionRecs(ShotBox(s), pb)) {
        s.dir = -s.dir;
        s.speed *= foe.def->parry.reflectSpeed;
        foe.shot = s;  // shortcut: apaga um projétil próprio de quem aparou, se houver (hoje ninguém tem os dois)
        s.on = false;
        ParrySuccess(foe, pb, hitstop);
    } else if (canHit && s.kind == 1 && s.hitCD == 0 && CheckCollisionRecs(ShotBox(s), Hurtbox(foe))) {
        // furacão: vários acertos seguidos enquanto passa pelo oponente; o último estoura
        ApplyHit(s.dir, foe, s.damage, Guard::Mid, hitstop, s.art->super.sfx);
        foe.push = s.dir * 1.5f;
        s.hitCD = s.art->superEvery;
        if (--s.hitsLeft <= 0) s.impactT = 0;
    } else if (canHit && s.kind == 2 && foe.act != Act::KO && CheckCollisionRecs(ShotBox(s), Hurtbox(foe))) {
        Burst(f, s, 0);  // bateu no oponente: estoura ali, o dano vem da nuvem
    } else if (canHit && s.kind == 0 && CheckCollisionRecs(ShotBox(s), Hurtbox(foe))) {
        ApplyHit(s.dir, foe, s.damage, Guard::Mid, hitstop, s.art->special.sfx);
        s.impactT = 0;
        f.shotLanded = true;  // acertou ou foi defendido: recovery normal
        if (s.art == f.def && s.damage < s.art->special.damage) Warn(f, 1, 100 - 100 * s.damage / s.art->special.damage);
        if (s.art->dropsWeapon) { s.on = false; DropWeapon(f, foe, s); }
    }
}

// Choque: dois projéteis voando que se encontram na horizontal (em qualquer altura: lobo no chão e cruz na mão
// também) se anulam no meio (estouro desenhado, som, hitstop curto). A arma
// arremessada (lápis) cai no chão em vez de sumir. Conta como "não errou" para o recovery do especial.
static void ClashShots(Fighter& a, Fighter& b, int& hitstop) {
    Shot &sa = a.shot, &sb = b.shot;
    if (!sa.on || !sb.on || sa.impactT >= 0 || sb.impactT >= 0) return;
    Rectangle ra = ShotBox(sa), rb = ShotBox(sb);
    bool crossed = (sa.prevX - sb.prevX) * (sa.x - sb.x) < 0;  // passaram um pelo outro neste tick
    if (!crossed && (ra.x > rb.x + rb.width || rb.x > ra.x + ra.width)) return;
    a.clashT = 0;
    a.clashX = (ra.x + ra.width / 2 + rb.x + rb.width / 2) / 2;
    a.clashY = (ra.y + ra.height / 2 + rb.y + rb.height / 2) / 2;
    for (Fighter* f : {&a, &b}) {
        f->shot.on = false;
        f->shotLanded = true;
        if (f->shot.art->dropsWeapon) DropWeapon(*f, f == &a ? b : a, f->shot);
    }
    Sfx("defesa");
    hitstop = std::max(hitstop, 5);
}

// ---------- Thays: borrifo e roupas da mala ----------

const int PUFF_TICKS = 36;  // vida de uma baforada do borrifo

static Prop* FreeProp(Fighter& f) {
    for (Prop& p : f.props)
        if (!p.kind) return &p;
    return nullptr;  // shortcut: 12 objetos por lutador; cheio, o novo não sai
}

// Mala: uma roupa por ângulo do leque, da frente do corpo na altura `hit_height`. Quadro da anim "clothes" = item.
static void ThrowClothes(Fighter& f) {
    const CharDef::Suitcase& s = f.def->suitcase;
    float x = f.x + f.facing * (PushFront(f) + 4), y = GROUND_Y - f.y - f.def->height * s.move.hitHeight;
    int item = 0;
    for (float deg : s.angles)
        if (Prop* p = FreeProp(f)) {
            float r = deg * DEG2RAD;
            *p = {1, x, y, x, y, f.facing * s.speed * cosf(r), s.speed * sinf(r), 0, 0, item++, Damage(f, s.move.damage)};
        }
    Sfx("whoosh");
}

// Baforada do borrifo: sai do frasco num cone curto (15 graus para cada lado) e vai parando.
static void SpawnPuff(Fighter& f) {
    Prop* p = FreeProp(f);
    if (!p) return;
    Vector2 n = HandTip(f, f.def->spray.move.hitHeight);
    float r = GetRandomValue(-15, 15) * DEG2RAD, v = 1.0f + GetRandomValue(0, 5) / 10.0f;
    *p = {2, n.x, n.y, n.x, n.y, f.facing * v * cosf(r), v * sinf(r) - 0.2f, 0, 0, 0, 0};
}

// Cone do borrifo (só borrifando): da ponta do frasco até `range` px à frente, 60% disso de altura.
static bool SprayBox(const Fighter& f, Rectangle& box) {
    const CharDef::Spray& s = f.def->spray;
    if (f.act != Act::Spray || f.t <= s.move.startup) return false;
    Vector2 n = HandTip(f, s.move.hitHeight);
    float len = (float)s.move.range, h = len * 0.6f;
    box = {f.facing > 0 ? n.x : n.x - len, n.y - h / 2, len, h};
    return true;
}

// Borrifo: dano a cada `hit_every` ticks em quem estiver no cone (defensável), sem pausa de acerto.
static void SprayHit(Fighter& f, Fighter& foe) {
    const CharDef::Spray& s = f.def->spray;
    Rectangle box;
    if (!SprayBox(f, box) || foe.act == Act::KO || (f.t - s.move.startup - 1) % s.hitEvery != 0 ||
        !CheckCollisionRecs(box, Hurtbox(foe)))
        return;
    int noStop = 0;
    ApplyHit(f.facing, foe, Damage(f, s.move.damage), s.move.guard, noStop, s.move.sfx, s.move.push);
}

// Caixa da roupa: 80% do desenho do item, centrada nela. Sem arte: 10 px.
static Rectangle PropBox(const Fighter& f, const Prop& p) {
    float w = 10, h = 10;
    if (const Anim* a = FindAnim(*f.def, "clothes")) {
        const FrameBoxes& b = a->boxes[p.item % a->boxes.size()];
        w = (b.right - b.left) * 0.8f;
        h = b.body.y1 * 0.8f;
    }
    return {p.x - w / 2, p.y - h / 2, w, h};
}

// Roupas voam girando e somem ao acertar (defensável) ou sair da tela; baforadas vão parando e somem.
static void UpdateProps(Fighter& f, Fighter& foe, int& hitstop, bool canHit) {
    const CharDef::Suitcase& s = f.def->suitcase;
    for (Prop& p : f.props) {
        if (!p.kind) continue;
        p.t++;
        p.x += p.vx;
        p.y += p.vy;
        if (p.kind == 2) {
            p.vx *= 0.95f;
            if (p.t >= PUFF_TICKS) p.kind = 0;
            continue;
        }
        p.ang += (p.vx > 0 ? 1 : -1) * s.spin;
        if (p.x < arena.cam - 30 || p.x > WallR() + 30 || p.y < -60 || p.y > GROUND_Y + 10) { p.kind = 0; continue; }
        if (canHit && foe.act != Act::KO && CheckCollisionRecs(PropBox(f, p), Hurtbox(foe))) {
            ApplyHit(p.vx > 0 ? 1 : -1, foe, p.damage, s.move.guard, hitstop, s.move.sfx, s.move.push);
            p.kind = 0;
        }
    }
}

// ---------- mascotes ----------

static const SupportDef* PetDef(const Fighter& f) { return f.pets && f.pet.id >= 0 ? &f.pets->list[f.pet.id] : nullptr; }

static const Anim* PetAnim(const SupportDef& s, const char* name) {
    auto it = s.anims.find(name);
    return it != s.anims.end() && !it->second.boxes.empty() ? &it->second : nullptr;
}

// A ação toca uma vez (a cura em loop até o fim da cura). Sem arte, dura action_ticks.
static int PetActionTicks(const SupportDef& s) {
    const Anim* a = PetAnim(s, "action");
    int t = a ? std::max(1, AnimTicks(*a, (int)a->boxes.size())) : s.actionTicks;
    return s.kind == PetKind::Heal ? std::max(t, s.healTicks) : t;
}

static int PetActionFrame(const SupportDef& s, const Anim& a, int t) {
    int n = (int)a.boxes.size(), f = AnimStep(a, t);
    return s.kind == PetKind::Heal ? f % n : std::min(f, n - 1);
}

// Tick (na ação) do quadro `active_frame`: o latido sai nele; o arranhão vale dele por `active_frames` quadros.
static int PetTriggerTick(const SupportDef& s) {
    const Anim* a = PetAnim(s, "action");
    return a ? AnimTicks(*a, ActiveFrame(*a)) : s.actionTicks / 3;
}
static int PetActiveEnd(const SupportDef& s) {
    const Anim* a = PetAnim(s, "action");
    return a ? AnimTicks(*a, ActiveFrame(*a) + s.activeFrames) : s.actionTicks * 2 / 3;
}

static int PetFxTicks(const SupportDef& s) {
    const Anim* a = PetAnim(s, "fx");
    return a ? std::max(1, AnimTicks(*a, (int)a->boxes.size())) : 18;
}

// Boca da Pérola: frente do desenho do latido, na altura de 60% do bicho.
static Vector2 PetMouth(const SupportDef& s, const Pet& p) {
    const Anim* a = PetAnim(s, "action");
    float front = 14, h = 30;
    if (a) {
        const FrameBoxes& b = a->boxes[ActiveFrame(*a)];
        front = b.right;
        h = b.body.y1;
    }
    return {p.x + p.dir * front, GROUND_Y - h * 0.6f};
}

// Arranhão da Milka (só na janela ativa): a pata medida na arte do quadro atual; sem arte, "hitbox" do JSON.
static bool PetHitbox(const Fighter& f, Rectangle& box) {
    const SupportDef* s = PetDef(f);
    const Pet& p = f.pet;
    if (!s || s->kind != PetKind::Attack || p.phase != 1 || p.t < PetTriggerTick(*s) || p.t >= PetActiveEnd(*s)) return false;
    const Anim* a = PetAnim(*s, "action");
    Box hb = s->hitbox;
    float feet = (float)GROUND_Y;
    if (a) {
        const FrameBoxes& b = a->boxes[PetActionFrame(*s, *a, p.t)];
        if (b.hasHit) hb = b.hit;
        feet += b.sink;
    }
    box = ToWorld(hb, p.x, feet, p.dir);
    return true;
}

bool CanCallPet(const Fighter& f) {
    if (!f.pets || f.petChoice < 0 || f.petChoice >= (int)f.pets->list.size()) return false;
    const SupportDef& s = f.pets->list[f.petChoice];
    return f.pet.id < 0 && f.act != Act::Hit && f.act != Act::KO && f.meter >= s.cost && f.petCD == 0 && f.petUses < s.maxUses;
}

// Entra correndo pela borda da tela atrás de quem chamou.
static void CallPet(Fighter& f) {
    if (!CanCallPet(f)) return;
    const SupportDef& s = f.pets->list[f.petChoice];
    f.meter -= s.cost;
    f.petUses++;
    f.petCD = s.cooldown;
    f.pet = Pet{};
    f.pet.id = f.petChoice;
    f.pet.dir = f.facing;
    f.pet.x = f.pet.prevX = f.facing > 0 ? arena.cam - 30 : WallR() + 30;
}

// Chegando → ação → indo embora. canAct = false depois do K.O.: só corre, sem efeito.
static void UpdatePet(Fighter& f, Fighter& foe, int& hitstop, bool canAct) {
    if (f.petCD > 0) f.petCD--;
    Pet& p = f.pet;
    const SupportDef* s = PetDef(f);
    if (!s) return;
    if (p.fxT >= 0 && ++p.fxT >= PetFxTicks(*s) && !(s->kind == PetKind::Heal && p.phase == 1)) p.fxT = -1;
    p.prevX = p.x;
    p.t++;
    float step = s->runSpeed / 60;
    if (p.phase == 0) {
        // alvo: perto de quem chamou (latido à frente, cura atrás) ou do oponente (arranhão), atualizado a cada tick
        float target = (s->kind == PetKind::Attack ? foe.x : f.x) + p.dir * s->stopOffset;
        p.x += p.dir * step;
        if ((target - p.x) * p.dir <= 0) {
            p.x = target;
            p.phase = 1;
            p.t = 0;
            if (s->kind != PetKind::Bark && canAct) Sfx(s->sfx);  // o latido soa junto da onda
            if (s->kind == PetKind::Heal && canAct) { p.fxT = 0; p.fxX = f.x; p.fxY = (float)GROUND_Y; Sfx("tilintar"); }
        }
    } else if (p.phase == 1) {
        if (canAct && s->kind == PetKind::Bark && !p.done && p.t >= PetTriggerTick(*s)) {
            // latido: onda na boca e empurrão para longe, mais forte quanto mais perto (some no alcance); sem dano
            p.done = true;
            Sfx(s->sfx);
            Vector2 m = PetMouth(*s, p);
            p.fxT = 0; p.fxX = m.x; p.fxY = m.y;
            float k = std::clamp(1 - fabsf(foe.x - m.x) / s->pushRadius, 0.0f, 1.0f);
            if (k > 0) {
                if (s->pushDamage > 0 && foe.act != Act::KO) ApplyHit(foe.x >= m.x ? 1 : -1, foe, s->pushDamage, Guard::Mid, hitstop, s->attack.sfx);
                foe.slide = (foe.x >= m.x ? 1 : -1) * s->pushMax * k;
                foe.slideFriction = s->friction;
            }
        }
        Rectangle hb;
        if (canAct && s->kind == PetKind::Attack && !p.done && PetHitbox(f, hb)) {
            Rectangle hurt = Hurtbox(foe);
            if (CheckCollisionRecs(hb, hurt)) {  // arranhão: dano pequeno, defensável (regras normais de defesa)
                p.done = true;
                ApplyHit(p.dir, foe, s->attack.damage, s->attack.guard, hitstop, s->attack.sfx);
                Rectangle o = GetCollisionRec(hb, hurt);
                p.fxT = 0; p.fxX = o.x + o.width / 2; p.fxY = o.y + o.height / 2;
            }
        }
        if (canAct && s->kind == PetKind::Heal && p.t <= s->healTicks && f.act != Act::KO) {
            // cura gradual: heal_total espalhado em heal_ticks; nunca passa da vida máxima
            p.healRest += s->healTotal / s->healTicks;
            int add = (int)p.healRest;
            p.healRest -= add;
            f.hp = std::min(f.def->health, f.hp + add);
            p.fxX = f.x;  // o brilho acompanha quem é curado
        }
        if (p.t >= PetActionTicks(*s)) {
            p.phase = 2;
            p.t = 0;
            p.dir = -p.dir;  // volta por onde veio
            if (s->kind == PetKind::Heal) p.fxT = -1;
        }
    } else {
        p.x += p.dir * step;
        if (p.x < arena.cam - 40 || p.x > WallR() + 40) p.id = -1;
    }
}

// Medidor: ganha por dano dado e (um pouco mais) por dano levado; cada round começa em 0. Reação de quem está
// perdendo: abaixo de `comeback.below` da vida, ganha `comeback.gain` vezes mais (dando e levando).
static float Comeback(const Fighter& f) {
    return f.hp < f.def->health * f.pets->comebackBelow ? f.pets->comebackGain : 1.0f;
}
static void GainMeter(Fighter& at, Fighter& df, int damage) {
    if (damage <= 0) return;
    if (at.pets) at.meter = std::min(at.pets->maxMeter, at.meter + damage * at.pets->gainHit * Comeback(at));
    if (df.pets) df.meter = std::min(df.pets->maxMeter, df.meter + damage * df.pets->gainTaken * Comeback(df));
}

// Canto: o empurrão do golpe em quem está encostado na parede (ou na lateral da mesa) vai para quem bateu, se ele
// estiver perto (golpe corpo a corpo; projétil de longe não empurra o atirador). Ninguém fica preso no canto.
const float CORNER_PUSH_RANGE = 100;
static void CornerPush(Fighter& df, Fighter& at) {
    bool wall = (df.push < 0 && df.x <= MinX(df) + 1) || (df.push > 0 && df.x >= WallR() - PushRight(df) - 1);
    if (!wall || fabsf(at.x - df.x) > CORNER_PUSH_RANGE) return;
    at.push -= df.push;
    df.push = 0;
}

// Depois do K.O. ninguém mais apanha e ninguém mais comanda: o perdedor toca a queda (e desliza com o
// empurrão do golpe); o vencedor termina o que fazia (golpe, pulo) e volta a ficar parado, respirando.
void UpdateAfterKO(Fighter& a, Fighter& b) {
    UpdateOne(a, {}, b);
    UpdateOne(b, {}, a);
    Separate(a, b);
    int noHitstop = 0;
    UpdateShot(a, b, noHitstop, false);
    UpdateShot(b, a, noHitstop, false);
    UpdatePet(a, b, noHitstop, false);
    UpdatePet(b, a, noHitstop, false);
    UpdateProps(a, b, noHitstop, false);
    UpdateProps(b, a, noHitstop, false);
    UpdateCamera(a, b);
}

// Arrancada: encostou no oponente = tromba (dano leve + empurrão, a corrida acaba); bateu na borda = para.
static void DashContact(Fighter& f, Fighter& foe, int& hitstop) {
    if (f.act != Act::Dash || f.t <= f.def->dash.startup) return;  // só correndo (nem no preparo, nem no recovery)
    // encosta pela frente do desenho (para-choque do carro), não só pelo corpo
    float reach = PushFront(f);
    int frame;
    float shove;
    if (const Anim* a = CurrentAnim(f, frame, shove)) {  // arte espelhada ("mirror"): a frente é o outro lado do desenho
        const FrameBoxes& b = a->boxes[frame % a->boxes.size()];
        reach = std::max(reach, (a->mirror ? -b.left : b.right) - 4);
    }
    float gap = reach + (f.facing > 0 ? PushLeft(foe) : PushRight(foe));
    bool ahead = (foe.x - f.x) * f.facing > 0;
    if (ahead && fabsf(foe.x - f.x) <= gap + 0.5f && foe.act != Act::KO && fabsf(foe.y - f.y) < foe.def->height * 0.5f) {
        f.t = -f.def->dash.recovery;
        ApplyHit(f.facing, foe, Damage(f, f.def->dash.damage), Guard::Mid, hitstop, "soco_fraco");
        foe.push = f.facing * f.def->dash.push;
    } else if (f.x <= MinX(f) + 0.5f || f.x >= WallR() - PushRight(f) - 0.5f) {
        f.t = -f.def->dash.recovery;
    }
}

void UpdateFight(Fighter& a, Fighter& b, const Input& ia, const Input& ib, int& hitstop) {
    int hpA = a.hp, hpB = b.hp;  // para o medidor: quanto cada um tirou do outro neste tick
    UpdateOne(a, ia, b);
    UpdateOne(b, ib, a);
    Separate(a, b);
    DashContact(a, b, hitstop);
    DashContact(b, a, hitstop);
    TryParry(a, b, hitstop);  // antes dos acertos: golpe aparado não acerta mais
    TryParry(b, a, hitstop);
    // decide os dois acertos antes de aplicar, para golpes simultâneos trocarem
    int da = Hits(a, b) ? Damage(a, Attack(a).damage) : 0;  // golpe repetido seguido: dano menor
    int db = Hits(b, a) ? Damage(b, Attack(b).damage) : 0;
    Guard ga = Attack(a).guard, gb = Attack(b).guard;  // antes de aplicar: o acerto muda o estado de quem apanha
    std::string sa = Attack(a).sfx, sb = Attack(b).sfx;
    float pa = Attack(a).push, pb = Attack(b).push;
    if (da) { a.hitDone = true; ApplyHit(a.facing, b, da, ga, hitstop, sa, pa); a.chainOk = b.act == Act::Hit || b.act == Act::KO; }
    if (db) { b.hitDone = true; ApplyHit(b.facing, a, db, gb, hitstop, sb, pb); b.chainOk = a.act == Act::Hit || a.act == Act::KO; }
    SprayHit(a, b);
    SprayHit(b, a);
    if (da && a.spamScale < 1) Warn(a, 1, (int)lroundf(100 * (1 - a.spamScale)));  // golpe repetido: dano menor
    if (db && b.spamScale < 1) Warn(b, 1, (int)lroundf(100 * (1 - b.spamScale)));
    UpdateShot(a, b, hitstop, b.act != Act::KO);
    UpdateShot(b, a, hitstop, a.act != Act::KO);
    ClashShots(a, b, hitstop);
    UpdateProps(a, b, hitstop, b.act != Act::KO);
    UpdateProps(b, a, hitstop, a.act != Act::KO);
    if (ia.support) CallPet(a);
    if (ib.support) CallPet(b);
    UpdatePet(a, b, hitstop, b.act != Act::KO && a.act != Act::KO);
    UpdatePet(b, a, hitstop, a.act != Act::KO && b.act != Act::KO);
    GainMeter(a, b, hpB - b.hp);
    GainMeter(b, a, hpA - a.hp);
    CornerPush(a, b);
    CornerPush(b, a);
    UpdateCamera(a, b);
}

// Níveis da CPU (OPÇÕES): think = ticks entre decisões; o resto em %: atacar no alcance, recuar, defender,
// aparar golpe / projétil, especial de longe (por decisão) e chamar o mascote (por tick). punish = bate no
// recovery de quem errou o golpe. MÉDIO = a IA de antes.
// shortcut: tabela fixa; quando entrar a rede neural adaptativa, ela vira mais um nível que substitui esta função.
struct CpuLevel { int thinkMin, thinkMax, attack, retreat, block, parry, parryShot, special, pet; bool punish; };
static const CpuLevel CPU_LEVELS[CPU_LEVEL_N] = {
    {24, 45, 35, 25, 20, 0, 0, 0, 1, false},     // FÁCIL: lenta, ataca pouco, quase não defende, nunca apara
    {10, 25, 60, 15, 50, 25, 35, 0, 2, false},   // MÉDIO
    {6, 14, 75, 10, 75, 35, 45, 25, 2, true},    // DIFÍCIL: rápida, defende muito, especial, pune erros
    {3, 8, 85, 5, 92, 50, 60, 40, 3, true},      // AVANÇADO: reage quase na hora e quase sempre defende
};

// IA simples: aproxima, ataca no alcance, às vezes recua ou defende; quanto mais alto o nível, mais rápida e certeira.
Input CpuInput(const Fighter& me, const Fighter& foe, int level) {
    const CpuLevel& L = CPU_LEVELS[std::clamp(level, 0, CPU_LEVEL_N - 1)];
    static int timer = 0, motion = 0;  // motion: ticks que faltam do comando ↓ → soco do especial
    static Input plan{};
    static bool blocker = false;
    Input in = plan;
    bool foeRight = foe.x > me.x;
    Rectangle foeHurt = Hurtbox(foe);  // do meu centro até a borda mais perto de onde o outro apanha
    float dist = foeRight ? foeHurt.x - me.x : me.x - (foeHurt.x + foeHurt.width);
    bool canPunch = dist < Reach(me, true) - 2, canKick = dist < Reach(me, false) - 2;
    if (--timer <= 0) {
        timer = GetRandomValue(L.thinkMin, L.thinkMax);
        blocker = GetRandomValue(0, 99) < L.block;
        int roll = GetRandomValue(0, 99);
        plan = {};
        in = {};
        if ((canPunch || canKick) && roll < L.attack) {
            if (canPunch && (!canKick || roll < L.attack / 2)) in.punch = true;
            else in.kick = true;
        } else if (me.def->hasSpecial && !me.shot.on && !me.unarmed && dist > 80 && GetRandomValue(0, 99) < L.special) {
            motion = 3;
        } else if (roll < L.retreat) {
            (foeRight ? plan.left : plan.right) = true;  // recua
        } else {
            (foeRight ? plan.right : plan.left) = true;  // aproxima
        }
        in.left = plan.left;
        in.right = plan.right;
    }
    if (motion > 0 && me.act == Act::Free) {  // especial: ↓, ↓→, → + soco, um passo por tick
        bool fwdR = foeRight;
        in = {};
        in.down = motion >= 2;
        (fwdR ? in.right : in.left) = motion <= 2;
        in.punch = motion == 1;
        motion--;
    } else {
        motion = 0;
    }
    // pune: o golpe do outro já passou da parte ativa e ele está no alcance
    if (L.punish && me.act == Act::Free && IsAttack(foe.act) && foe.t >= Attack(foe).startup + Attack(foe).active && canPunch)
        in = {false, false, true, false, false, false, 0};
    // sem a arma (lápis arremessado): vai buscar; em cima da mesa, pula quando chega perto
    if (me.weapon.on && me.weapon.down && motion == 0) {
        in = {};
        in.right = me.weapon.x > me.x;
        in.left = !in.right;
        in.up = me.weapon.h > me.y && fabsf(me.weapon.x - me.x) < 50;
    }
    bool shotNear = foe.shot.on && foe.shot.impactT < 0 && fabsf(foe.shot.x - me.x) < 90;
    if (blocker && ((dist < 70 && IsAttack(foe.act)) || shotNear)) {
        in.left = foeRight;  // segura para trás
        in.right = !foeRight;
        in.down = IsAttack(foe.act) && Attack(foe).guard == Guard::Low;  // agacha contra golpe baixo, em pé contra o resto
    }
    // parry (quem tem): às vezes, logo no começo do golpe ou com o projétil a ~6 ticks de chegar.
    // Chance baixa de propósito: a CPU não pode aparar tudo, senão fica impossível para criança.
    static bool parryShot = false;
    if (foe.shot.on && foe.shot.t == 1) parryShot = GetRandomValue(0, 99) < L.parryShot;  // decide uma vez por projétil
    Rectangle soon = foe.shot.on ? ShotBox(foe.shot) : Rectangle{};
    soon.x += foe.shot.dir * foe.shot.speed / 60 * 6;
    bool shotSoon = foe.shot.on && foe.shot.impactT < 0 && parryShot && CheckCollisionRecs(soon, Hurtbox(me));
    bool meleeStart = dist < 70 && IsAttack(foe.act) && foe.t == 0 && GetRandomValue(0, 99) < L.parry;
    // mascote: de vez em quando, se fizer sentido (cura com pouca vida, latido com o outro perto, arranhão sempre)
    if (CanCallPet(me) && GetRandomValue(0, 99) < L.pet) {
        PetKind kind = me.pets->list[me.petChoice].kind;
        bool sensible = kind == PetKind::Attack || (kind == PetKind::Bark && dist < 90) ||
                        (kind == PetKind::Heal && me.hp * 10 < me.def->health * 6);
        if (sensible) in.support = 1;
    }
    if (me.def->parry.on && me.act == Act::Free && me.parryCD == 0 && (shotSoon || meleeStart)) {
        in = {foeRight, !foeRight, true, false, true, false, 0};  // ↙ + soco
        parryShot = false;
    }
    return in;
}

// ---------- desenho ----------

// Arma solta, atrás dos lutadores: girando (anim "projectile") enquanto cai; caída, quadro 1 de "dropped" no piso.
// Enquanto a dona pega, só a arte do "pickup" mostra o lápis.
static void DrawWeapon(const Fighter& f, float pixelsPerUnit) {
    const auto& w = f.weapon;
    if (!w.on || f.act == Act::Pickup) return;
    const Anim* a = FindAnim(*f.def, w.down ? "dropped" : "projectile");
    if (!a || a->frames.empty()) { DrawRectangle((int)w.x - 10, (int)(GROUND_Y - w.h) - 3, 20, 3, YELLOW); return; }
    int frame = w.down ? 0 : ShotFrame(*a, w.t);
    const Box& b = a->boxes[frame].body;
    float y = GROUND_Y - w.h + (w.down ? a->boxes[frame].sink : (b.y0 + b.y1) / 2);
    DrawAnimFrame(*a, frame, {w.x, y}, w.dir < 0, false, pixelsPerUnit);
}

void DrawFighter(const Fighter& f, bool debug, float alpha, float pixelsPerUnit, const Light& light, bool shadowPass) {
    if (shadowPass) {
        DrawWeapon(f, pixelsPerUnit);
        if (light.shadowAlpha <= 0) return;  // sem sombra (transmissão da TV)
    }
    int frame;
    float shove;
    const Anim* anim = CurrentAnim(f, frame, shove);
    if (anim && anim->frames.empty()) anim = nullptr;  // caixas sem textura (só em teste)

    // Posição desenhada = interpolada entre o tick anterior e o atual (`alpha` = fração do tick já passada).
    // A lógica roda a 60 Hz e o monitor pode ser 144/165 Hz: sem isso cada posição fica 2 ou 3 quadros na tela,
    // num ritmo irregular que o olho lê como tremedeira.
    float rx = f.prevX + (f.x - f.prevX) * alpha, ry = f.prevY + (f.y - f.prevY) * alpha;
    Rectangle drawn;
    if (anim) {
        float feetX = FrameX(f, *anim, frame, rx + f.facing * shove);
        bool flip = Face(f, *anim) < 0;
        // Inercialização: trocar de animação (idle preso nos pés, andar preso no tronco...) fazia o corpo saltar 2-3 px
        // de lado num quadro. O salto do tronco vira um deslocamento d0 que decai como d0·e^(-t/τ), τ = 45 ms: o corpo
        // desliza até a pose nova. Só no desenho (caixas de golpe exatas); salto grande (>12 px: virou, atravessou,
        // round novo) não suaviza. Atualiza só na tela de verdade (a TV desenha a cena de novo, menor).
        // tronco de uma pose (anim, quadro) desenhada na posição atual: comparar as duas poses no MESMO x
        auto torsoAt = [&](const Anim& a, int fr) {
            const FrameBoxes& b = a.boxes[fr % a.boxes.size()];
            return FrameX(f, a, fr, rx + f.facing * shove) + Face(f, a) * (b.body.x0 + b.body.x1) / 2;
        };
        if (!shadowPass && pixelsPerUnit >= 1) {
            if (f.drawAnim && f.drawAnim != anim && !f.drawAnim->boxes.empty()) {
                float jump = torsoAt(*f.drawAnim, f.drawFrame) - torsoAt(*anim, frame);
                f.blendX = fabsf(jump + f.blendX) < 12 ? f.blendX + jump : 0;
            }
            f.blendX *= expf(-std::min(GetFrameTime(), 0.1f) / 0.045f);
            if (fabsf(f.blendX) < 0.05f) f.blendX = 0;
            f.drawAnim = anim;
            f.drawFrame = frame;
        }
        feetX += f.blendX;
        if (shadowPass) {
            float sink = anim->boxes[frame % anim->boxes.size()].sink;
            // sombra de contato: elipse suave colada nos pés (prende o lutador no chão), some subindo no pulo
            float fade = std::max(0.0f, 1 - ry / 40), half = (PushFront(f) + PushBack(f)) * 0.55f;
            if (fade > 0) DrawEllipse((int)roundf(feetX), (int)roundf(GROUND_Y - FloorAt(f) + sink), half * (0.6f + 0.4f * fade), 3,
                                      Fade(BLACK, light.shadowAlpha * 0.6f * fade));
            DrawAnimShadow(*anim, frame, feetX, ry, GROUND_Y + sink, flip, light.side, light.shadowLength, light.shadowAlpha);
            return;
        }
        // Respiração contínua (só animações com "breath"): o topo da cabeça segue uma onda suave no mesmo ritmo
        // dos quadros, com pico na inspiração. O esticar compensa a altura de cada quadro (um quadro mais alto
        // estica menos), então trocar de quadro muda só os detalhes, sem degrau na altura. Relógio interpolado
        // (animT + alpha): em 165 Hz a cabeça sobe e desce em passos de 1 pixel de tela; pés sempre parados.
        float stretch = 0;
        if (anim->breath > 0 && anim->fps > 0) {
            int steps = anim->order.empty() ? (int)anim->boxes.size() : (int)anim->order.size();
            float cycle = (f.animT + alpha) / 60.0f * anim->fps / steps;  // ciclos completos desde o início
            float phase = cycle - floorf(cycle);
            float wave = 0.5f * (1 + cosf(2 * PI * (phase - anim->breathPeak)));  // 0 = expirado, 1 = inspirado
            stretch = anim->breathBase + anim->breath * wave - anim->frames[frame % anim->frames.size()].height;
        }
        // aura atrás do corpo (aditiva, em loop): "special_aura" no especial, "block_aura" na guarda
        bool guard = f.act == Act::Block || (f.act == Act::Free && f.holdBack && (f.walking || f.crouching));
        const char* aura = f.act == Act::Special ? "special_aura" : guard ? "block_aura" : nullptr;
        if (const Anim* au = aura ? FindAnim(*f.def, aura) : nullptr; au && !au->frames.empty()) {
            BeginBlendMode(BLEND_ADDITIVE);
            int af = AnimStep(*au, f.animT) % (int)au->boxes.size();
            float cx = AnimLeft(*anim, frame, feetX, flip) + anim->frames[frame % anim->frames.size()].width / 2.0f;  // meio do desenho
            cx -= AnimLeft(*au, af, 0, flip) + au->frames[af].width / 2.0f;  // aura (moldura vazada) pela caixa, não pelo tronco
            DrawAnimFrame(*au, af, {cx, GROUND_Y - ry + au->boxes[af].sink}, flip, false, pixelsPerUnit);
            EndBlendMode();
        }
        drawn = DrawAnimFrame(*anim, frame, {feetX, FeetY(*anim, frame, ry)}, flip, f.flash > 0, pixelsPerUnit, stretch,
                              f.flash > 0 ? WHITE : light.tint);
        if (f.flash == 0) DrawAnimLight(*anim, frame, drawn, flip, light.side, light.shade, light.warm, light.rim);
    } else if (shadowPass) {
        Vector2 c = ShadowPoint(rx, ry, (float)GROUND_Y, light.side, light.shadowLength);
        DrawEllipse((int)c.x, (int)c.y + 2, PushFront(f), 3, Fade(BLACK, light.shadowAlpha));
        return;
    } else {
        drawn = Hurtbox(f);
        drawn.x += rx - f.x + f.facing * shove;
        drawn.y += f.y - ry;
        DrawRectangleRec(drawn, f.flash > 0 ? WHITE : f.color);
        const char* label = f.def->name.c_str();
        DrawText(label, (int)(drawn.x + drawn.width / 2 - MeasureText(label, 10) / 2.0f), (int)(drawn.y + drawn.height / 2), 10, BLACK);
    }

    // F1: amarelo = desenho, azul = colisão do corpo, verde = onde apanha, vermelho = onde acerta
    Rectangle hit;
    if (ActiveHitbox(f, hit) && (debug || shove > 0)) DrawRectangleRec(hit, Fade(RED, 0.6f));
    if (debug) {
        DrawRectangleLinesEx(drawn, 1, YELLOW);
        DrawRectangleLinesEx({f.x - PushLeft(f), GROUND_Y - f.y - 6, PushLeft(f) + PushRight(f), 6}, 1, SKYBLUE);
        DrawRectangleLinesEx(Hurtbox(f), 1, GREEN);
        Rectangle pb;
        if (ParryBox(f, pb)) DrawRectangleRec(pb, Fade(SKYBLUE, 0.6f));  // ciano = parry
        if (SprayBox(f, pb)) DrawRectangleRec(pb, Fade(RED, 0.4f));      // cone do borrifo
    }
}

// Balão de provocação acima e à frente da cabeça (espelhado conforme o lado) com o texto escrito pelo código; entra
// com um "pop" de escala e some nos últimos ticks.
static void DrawTaunt(const Fighter& f, float alpha) {
    if (f.tauntT < 0) return;
    const CharDef& d = *f.def;
    float t = f.tauntT + alpha, s = t < 4 ? 0.5f + 0.7f * t / 4 : t < 8 ? 1.2f - 0.2f * (t - 4) / 4 : 1;
    float fade = std::clamp((d.tauntTicks - t) / 6, 0.0f, 1.0f);
    float x = f.prevX + (f.x - f.prevX) * alpha, y = f.prevY + (f.y - f.prevY) * alpha;
    const Anim* a = FindAnim(d, "balloon");
    if (a && a->frames.empty()) a = nullptr;
    float w = a ? (float)a->frames[0].width : 80, h = a ? (float)a->frames[0].height : 44;
    float cx = std::clamp(x + f.facing * (w / 2 + 4), arena.cam + w / 2, WallR() - w / 2);
    float cy = std::max(h / 2, GROUND_Y - y - d.height - h / 2 + 10);
    if (a) {
        const Texture2D& tx = a->frames[0];
        DrawTexturePro(tx, {0, 0, (float)(f.facing < 0 ? -tx.width : tx.width), (float)tx.height}, {cx, cy, w * s, h * s},
                       {w * s / 2, h * s / 2}, 0, Fade(WHITE, fade));
    } else {
        DrawEllipse((int)cx, (int)cy, w / 2 * s, h / 2 * s, Fade(WHITE, fade));
    }
    if (t >= 4) {  // texto só quando o balão já abriu
        const char* text = d.tauntText.c_str();
        DrawUIText(text, {roundf(cx - MeasureUIText(text, 1) / 2.0f), roundf(cy - UITextHeight(1) / 2.0f) - 2}, 1, Fade(BLACK, fade));
    }
}

void DrawEffects(const Fighter& f, bool debug, float alpha, float pixelsPerUnit) {
    if (f.clashT >= 0) {  // choque de projéteis: núcleo branco que encolhe, anel abrindo e 8 raios girando (aditivo)
        float k = f.clashT / 20.0f, x = f.clashX, y = f.clashY;
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleV({x, y}, 9 * (1 - k), Fade(WHITE, 1 - k));
        DrawCircleLinesV({x, y}, 5 + 22 * k, Fade({255, 220, 120, 255}, 1 - k));
        for (int i = 0; i < 8; i++) {
            float an = i * PI / 4 + k * 0.6f, r0 = 6 + 10 * k, r1 = r0 + 8 * (1 - k) + 3;
            DrawLineEx({x + cosf(an) * r0, y + sinf(an) * r0}, {x + cosf(an) * r1, y + sinf(an) * r1}, 2,
                       Fade(i % 2 ? Color{255, 200, 90, 255} : WHITE, 1 - k));
        }
        EndBlendMode();
    }
    if (f.djT >= 0) {  // pulo duplo: dois anéis de ar achatados abrindo embaixo dos pés + 4 baforadas, sem sprite
        float k = f.djT / 18.0f, y = GROUND_Y - f.djY;
        for (int r = 0; r < 2; r++) {
            float rx = 6 + 16 * k + r * 5, ry = 2 + 3 * k + r;
            DrawEllipseLines((int)roundf(f.djX), (int)roundf(y + r * 2), rx, ry, Fade(WHITE, (1 - k) * (r ? 0.5f : 0.9f)));
        }
        for (int i = 0; i < 4; i++) {
            float dx = (i < 2 ? -1 : 1) * (5 + 14 * k) * (i % 2 ? 0.6f : 1), dy = 2 + 9 * k * (i % 2 ? 1 : 0.5f);
            DrawRectangle((int)roundf(f.djX + dx), (int)roundf(y + dy), 2, 2, Fade(WHITE, 0.8f * (1 - k)));
        }
    }
    if (f.fxT >= 0) {  // brilho do parry, aditivo (só clareia)
        BeginBlendMode(BLEND_ADDITIVE);
        if (const Anim* a = FindAnim(*f.def, FxName(*f.def)); a && !a->frames.empty()) {
            int frame = std::min((int)a->boxes.size() - 1, AnimStep(*a, f.fxT));
            const Box& b = a->boxes[frame].body;
            DrawAnimFrame(*a, frame, {f.fxX, f.fxY + (b.y0 + b.y1) / 2}, f.facing < 0, false, pixelsPerUnit);
        } else {  // sem arte: clarão que abre e apaga
            float k = f.fxT / (float)FxTicks(*f.def);
            DrawCircleV({f.fxX, f.fxY}, 5 + 12 * k, Fade({255, 220, 140, 255}, 1 - k));
            DrawCircleLinesV({f.fxX, f.fxY}, 8 + 22 * k, Fade(WHITE, 1 - k));
        }
        EndBlendMode();
    }
    for (const Prop& p : f.props) {  // roupas da mala girando; baforadas do borrifo (aditivas, somem no fim)
        if (!p.kind) continue;
        float x = p.prevX + (p.x - p.prevX) * alpha, y = p.prevY + (p.y - p.prevY) * alpha;
        int dir = p.vx < 0 ? -1 : 1;
        const Anim* a = FindAnim(*f.def, p.kind == 1 ? "clothes" : "puff");
        if (!a || a->frames.empty()) {
            DrawRectangle((int)x - 3, (int)y - 3, 6, 6, p.kind == 1 ? RED : PINK);
        } else {
            int n = (int)a->frames.size(), fr = p.kind == 1 ? p.item % n : std::min(n - 1, p.t * n / PUFF_TICKS);
            const Texture2D& tx = a->frames[fr];
            float ang = p.kind == 1 ? p.ang + dir * f.def->suitcase.spin * (alpha - 1) : 0;
            if (p.kind == 2) BeginBlendMode(BLEND_ADDITIVE);
            DrawTexturePro(tx, {0, 0, (float)(dir * tx.width), (float)tx.height}, {x, y, (float)tx.width, (float)tx.height},
                           {tx.width / 2.0f, tx.height / 2.0f}, ang, p.kind == 1 ? WHITE : Fade(WHITE, 1 - (float)p.t / PUFF_TICKS));
            if (p.kind == 2) EndBlendMode();
        }
        if (debug && p.kind == 1) DrawRectangleRec(PropBox(f, p), Fade(RED, 0.6f));
    }
    DrawTaunt(f, alpha);
    const Shot& s = f.shot;
    if (!s.on) return;
    float x = s.prevX + (s.x - s.prevX) * alpha, sy = s.prevY + (s.y - s.prevY) * alpha;
    bool flip = s.dir < 0;
    const char* name = ShotAnimName(s, s.impactT >= 0);
    if (const Anim* a = FindAnim(*s.art, name); a && !a->frames.empty()) {
        bool cloud = s.kind == 2 && s.impactT >= 0;  // nuvem da bomba: em loop, aditiva, some nos últimos 20 ticks
        int n = (int)a->boxes.size();
        int frame = cloud ? AnimStep(*a, s.impactT) % n : s.impactT >= 0 ? std::min(n - 1, AnimStep(*a, s.impactT)) : ShotFrame(*a, s.t);
        const Box& b = a->boxes[frame].body;  // centro do "tronco" do efeito no centro do projétil
        if (a->spin != 0 && s.impactT < 0) {  // gira por código em volta do centro (tênis: 1 desenho só)
            const Texture2D& t = a->frames[frame];
            float ang = s.dir * a->spin * (s.t + alpha) / 60;
            DrawTexturePro(t, {0, 0, (float)(flip ? -t.width : t.width), (float)t.height},
                           {x, sy, (float)t.width, (float)t.height}, {t.width / 2.0f, t.height / 2.0f}, ang, WHITE);
        } else {
            if (cloud) BeginBlendMode(BLEND_ADDITIVE);
            DrawAnimFrame(*a, frame, {x, sy + (b.y0 + b.y1) / 2}, flip, false, pixelsPerUnit, 0,
                          cloud ? Fade(WHITE, std::min(1.0f, (s.art->cloudTicks - s.impactT) / 20.0f)) : WHITE);
            if (cloud) EndBlendMode();
        }
    } else if (s.kind == 1) {  // furacão sem arte: redemoinho de vento (elipses girando, afinando para baixo) + poeira
        float t = (s.t + alpha) / 60, k = s.impactT >= 0 ? 1 - s.impactT / 12.0f : 1;
        for (int i = 0; i < 7; i++) {
            float yy = s.y + 20 - i * 7, rx = (5 + i * 2.2f) * k, ph = t * 18 + i * 0.9f;
            DrawEllipseLines((int)roundf(x + sinf(ph) * 2), (int)roundf(yy), rx, 2.0f + i * 0.3f, Fade(WHITE, 0.75f * k));
            DrawEllipseLines((int)roundf(x - sinf(ph) * 2), (int)roundf(yy + 1), rx * 0.7f, 1.5f, Fade({170, 200, 230, 255}, 0.6f * k));
        }
        for (int i = 0; i < 5; i++) {
            float ph = t * 12 + i * 1.3f;
            DrawRectangle((int)roundf(x + cosf(ph) * 14), (int)roundf(s.y + 18 - (i % 3) * 3), 2, 2, Fade({200, 180, 150, 255}, 0.8f * k));
        }
    } else if (s.kind == 2 && s.impactT >= 0) {  // nuvem sem arte
        DrawRectangleRec(CloudBox(s), Fade(PINK, 0.4f));
    } else {  // sem arte: bola amarela; impacto = anel que cresce
        if (s.impactT >= 0) DrawCircleLines((int)x, (int)s.y, 6.0f + s.impactT, YELLOW);
        else DrawCircle((int)x, (int)s.y, 6, YELLOW);
    }
    if (debug && s.impactT < 0) DrawRectangleRec(ShotBox(s), Fade(RED, 0.6f));
    if (debug && s.kind == 2 && s.impactT >= 0) DrawRectangleLinesEx(CloudBox(s), 1, RED);
}

void DrawPet(const Fighter& f, bool debug, float alpha, float pixelsPerUnit, const Light& light, bool shadowPass) {
    const SupportDef* s = PetDef(f);
    if (!s) return;
    const Pet& p = f.pet;
    float x = p.prevX + (p.x - p.prevX) * alpha;
    bool flip = p.dir < 0;
    const Anim* a = p.phase == 1 ? PetAnim(*s, "action") : nullptr;
    int frame = 0;
    if (a) frame = PetActionFrame(*s, *a, p.t);
    else if ((a = PetAnim(*s, "run"))) frame = AnimStep(*a, p.t) % (int)a->boxes.size();
    if (a && a->frames.empty()) a = nullptr;  // caixas sem textura (só em teste)
    if (shadowPass) {
        if (a && light.shadowAlpha > 0) DrawAnimShadow(*a, frame, x, 0, GROUND_Y + a->boxes[frame].sink, flip, light.side, light.shadowLength, light.shadowAlpha);
        return;
    }
    if (a) {
        Rectangle dst = DrawAnimFrame(*a, frame, {x, GROUND_Y + a->boxes[frame].sink}, flip, false, pixelsPerUnit);
        DrawAnimLight(*a, frame, dst, flip, light.side, light.shade, light.warm, light.rim);
    } else {  // sem arte: bolinha com a inicial
        DrawCircleV({x, GROUND_Y - 9.0f}, 9, s->kind == PetKind::Heal ? LIME : s->kind == PetKind::Attack ? PINK : BEIGE);
        DrawText(TextFormat("%c", s->name[0]), (int)x - 3, GROUND_Y - 14, 10, BLACK);
    }
    // efeito aditivo: onda do latido / arranhão (centrados) / brilho da cura (de pé sobre quem é curado, em loop)
    if (p.fxT >= 0) {
        const Anim* fx = PetAnim(*s, "fx");
        BeginBlendMode(BLEND_ADDITIVE);
        if (fx && !fx->frames.empty()) {
            int n = (int)fx->boxes.size(), fr = AnimStep(*fx, p.fxT);
            fr = s->kind == PetKind::Heal ? fr % n : std::min(fr, n - 1);
            const Box& b = fx->boxes[fr].body;
            float fy = s->kind == PetKind::Heal ? p.fxY : p.fxY + (b.y0 + b.y1) / 2;
            DrawAnimFrame(*fx, fr, {p.fxX, fy}, s->kind == PetKind::Heal ? false : flip, false, pixelsPerUnit);
        } else {
            float k = std::min(1.0f, p.fxT / 18.0f);
            Color c = s->kind == PetKind::Heal ? GREEN : s->kind == PetKind::Attack ? SKYBLUE : YELLOW;
            DrawCircleLinesV({p.fxX, s->kind == PetKind::Heal ? p.fxY - 30 : p.fxY}, 6 + 20 * k, Fade(c, 1 - k));
        }
        EndBlendMode();
    }
    if (debug) {  // F1: arranhão (vermelho) e alcance do latido (amarelo)
        Rectangle hb;
        if (PetHitbox(f, hb)) DrawRectangleRec(hb, Fade(RED, 0.6f));
        if (s->kind == PetKind::Bark && p.phase == 1) {
            Vector2 m = PetMouth(*s, p);
            DrawCircleLinesV(m, s->pushRadius, Fade(YELLOW, 0.8f));
            DrawCircleV(m, 2, YELLOW);
        }
    }
}
