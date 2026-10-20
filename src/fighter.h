#pragma once
#include "sprites.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

// GROUND_Y = onde os pés pisam (muda com a arena: "floor_y" do stages.json); HORIZON_Y = fundo do chão do cenário
// desenhado (o piso continua atrás dos lutadores até aqui)
const int SCREEN_W = 384, SCREEN_H = 216, HORIZON_Y = 176;
extern int GROUND_Y;
// Plataforma da arena (mesa) encostada na borda esquerda: até x1 (px lógicos), tampo `top` px acima do chão
// (0 = nenhuma). Andando não entra; no pulo sobe e fica em cima; saindo dela, cai. A arena atualiza a cada tick.
// shortcut: só uma, na esquerda; se uma arena tiver outra, vira lista com x0/x1.
struct Platform { float x1 = 0, top = 0; };
extern Platform platform;
// Arena mais larga que a tela (cenário com câmera, ex.: Mercadão): o mundo vai de x0 a x1 e a tela mostra cam..cam+SCREEN_W.
// As paredes são as bordas da tela: quem recua encostado nela não passa, e a câmera só anda quando o ponto médio dos
// dois anda (como no Street Fighter). Padrão = a própria tela (sala: câmera parada em 0).
struct Arena { float x0 = 0, x1 = SCREEN_W, cam = 0, prevCam = 0; };
extern Arena arena;
// Sons da luta: a lógica põe o nome do arquivo (assets/sfx, sem extensão) e o jogo toca e esvazia a fila.
extern std::vector<std::string> sfxQueue;
// Tela de escolha: tamanho do ícone e do retrato em px do jogo. As texturas guardam 3x (o zoom da janela
// inicial): na tela, cada pixel delas vira 1 pixel do monitor, então rosto e óculos não somem no ícone pequeno.
// Retratos com "portrait_head" no JSON ficam todos na mesma escala: a cabeça (topo ao queixo) com PORTRAIT_HEAD px.
const int ICON_D = 20, PORTRAIT_W = 150, PORTRAIT_H = 170, PORTRAIT_HEAD = 56, PICTURE_RES = 3;
const int PET_ART_W = 150, PET_ART_H = 110;  // mascote grande na tela de escolha do suporte

// Como o golpe pode ser defendido: Mid = em pé ou agachado; Low = só agachado; High = só em pé (golpe no ar).
enum class Guard { Mid, Low, High };

// hitHeight = altura do centro da área de acerto, em fração da altura do corpo (0 = pés, 1 = cabeça)
// sfx = som do acerto (arquivo em assets/sfx sem extensão; vazio = soco_fraco, ou soco_forte com dano 9+);
// voice = voz do personagem ao começar o golpe
// push = empurrão em quem apanhou (px/tick, decai 30% por tick: 3 ≈ 10 px no total)
struct Move { int damage = 5, range = 30, startup = 5, active = 3, recovery = 10; float hitHeight = 0.75f; Guard guard = Guard::Mid; std::string sfx, voice; float push = 3; };

struct CharDef {
    std::string id, name;
    int height = 100, health = 100;
    float pushFront = 0, pushBack = 0;  // colisão do corpo a partir do centro, medida na arte (0 = 20% da altura)
    float walkSpeed = 60;
    // Defesa: o dano do golpe vira "% da barra" e quem defende perde só blockChip disso. Golpe de 12 defendido
    // = 12% x 0,15 = 1,8% da barra, para qualquer vida (90 ou 130). "block_chip" no JSON (geral ou do personagem).
    float blockChip = 0.15f;
    // Repetir o mesmo golpe seguido tira dano ("repeat_penalty" no topo do JSON): cada repetição -step, até min;
    // outro golpe ou `reset` ticks sem repetir zera. step 0 = desligado (padrão sem o JSON).
    float repeatStep = 0, repeatMin = 0.4f;
    int repeatReset = 150;
    // Barra de guarda ("guard" no topo do JSON): cada golpe defendido enche dano x perDamage (de 100); cheia, a guarda
    // quebra (aberto breakTicks); esvazia regen/s depois de delay ticks sem defender. perDamage 0 = desligada.
    float guardPerDamage = 0, guardRegen = 30;
    int guardBreak = 45, guardDelay = 60;
    float jumpHeight = 45;  // px
    int jumpRise = 18;      // ticks até o alto do pulo
    int jumpFall = 12;      // ticks do alto até o chão (menor = gravidade mais forte na descida)
    float jumpSpeed = 90;   // px/s na horizontal
    float doubleJump = 0.8f; // pulo duplo (↑ de novo no ar, um por pulo): altura em fração do pulo; 0 = sem
    Move punch;                              // botão 1 (J / Numpad 1): acerta alto por padrão
    Move kick = Move{5, 30, 5, 3, 10, 0.4f};  // botão 2 (K / Numpad 2): acerta baixo por padrão
    Move airAttack = Move{7, 25, 4, 6, 8, 0.4f, Guard::High};  // qualquer botão no ar (um por pulo); só defende em pé
    // botões 1 e 2 segurando baixo: golpe sem levantar. Sem "crouch_punch"/"crouch_kick" no JSON, levanta e golpeia.
    bool hasCrouchPunch = false, hasCrouchKick = false;
    Move crouchPunch = Move{5, 25, 4, 3, 8, 0.45f}, crouchKick = Move{7, 30, 6, 4, 14, 0.1f, Guard::Low};
    // ↓ depois → (frente) + soco: lança um projétil no fim do startup; sem "special" no JSON é só um soco
    bool hasSpecial = false;
    Move special = Move{10, 0, 14, 6, 22, 0.6f};
    float shotSpeed = 150;  // px/s do projétil
    // Arrancada (frente, frente em 12 ticks): "dash" {speed px/s, damage, push px/tick, max_ticks}. Corre até a
    // borda ou até trombar (dano leve + empurrão); segurar trás freia. Sem "dash" no JSON, não tem.
    // "half_circle": true = sai com ← ↓ → + chute (o carro do Lui) em vez de frente, frente. Anims "dash_start" (preparo)
    // e "dash_end" (recovery), se existirem; "smoke" no lugar do desenho quando o recovery acaba.
    struct Dash { bool on = false, halfCircle = false; float speed = 420; int damage = 3; float push = 4; int maxTicks = 60;
                  int startup = 8, recovery = 20; } dash;  // recovery > HIT_STUN: a trombada não emenda nada
    // Corrente (J, J, J): "punch2"/"punch3" no JSON; cada um sai só se o anterior ACERTOU (defendido não encadeia)
    int chainLen = 1;
    Move punch2 = Move{4, 26, 3, 2, 8}, punch3 = Move{6, 28, 5, 3, 14};
    // Furacão (← ↓ → + chute): "super" {damage por acerto, startup, active, recovery, speed, hits, hit_every,
    // dizzy}: redemoinho que avança e acerta `hits` vezes (um a cada `hit_every` ticks); depois fica tonto `dizzy`.
    bool hasSuper = false;
    Move super = Move{3, 0, 18, 6, 20, 0.4f};
    float superSpeed = 110;
    int superHits = 4, superEvery = 10, dizzyTicks = 48;
    int specialWhiff = 40;  // recovery do especial se o projétil ainda não acertou/foi defendido ("whiff_recovery")
    // "drops_weapon" no special: o projétil é a arma (lápis da Thayna). Acertando ou batendo na parede cai no chão;
    // sem ela, os golpes e as anims usam a versão "_unarmed" (se houver) até passar por cima e pegar de volta
    bool dropsWeapon = false;
    Move punchBare, kickBare;  // "punch_unarmed"/"kick_unarmed" (padrão = punch/kick)
    bool shotFromGround = false;  // "from": "ground" no special: o projétil nasce no chão à frente (senão, da mão)
    // ↓ depois ← (trás) + soco: parry. Aparar um golpe devolve o dano ao atacante; aparar um projétil o rebate.
    struct Parry {
        bool on = false;  // sem "parry" no JSON o comando é só um soco
        int startup = 2, active = 8, recovery = 6, whiffRecovery = 24, cooldown = 30, hitstop = 8, stunBonus = 10;
        float reflectSpeed = 1.2f;
        std::string voice;  // voz ao aparar
    } parry;
    // Thays. Parada `idleCall` ticks sem comando e sem luta acontecendo: anim "idle_call" em loop ("idle_call" no JSON)
    int idleCall = 0;
    // Borrifo (↓ + soco, segurando o soco): "spray" {startup = entrada, recovery, damage, range = cone, hit_every,
    // puff_every, drain/recharge por tick, restart}. Tanque 0..100: vazio encerra e só volta com `restart` de carga.
    struct Spray { bool on = false; Move move = Move{1, 40, 6, 1, 8, 0.7f}; int hitEvery = 10, puffEvery = 6;
                   float drain = 2, recharge = 0.5f, restart = 30; } spray;
    // Mala (↓ → chute): "suitcase" {startup, active, recovery, damage, hit_height, speed px/tick, spin graus/tick,
    // angles, cooldown}: no fim do startup solta uma roupa por ângulo (leque), cada uma acerta uma vez (defensável)
    struct Suitcase { bool on = false; Move move = Move{3, 0, 14, 4, 18, 0.62f}; float speed = 6, spin = 15;
                      std::vector<float> angles{-10, 0, 10}; int cooldown = 120; } suitcase;
    // Especial em arco ("gravity" > 0, "lift" px/tick para cima): ao tocar o chão ou o oponente vira nuvem que fica
    // `cloudTicks` e dá `cloudDamage` a cada `cloudEvery` ticks em quem estiver dentro (não em quem jogou)
    float shotGravity = 0, shotLift = 0;
    int specialCooldown = 0, cloudTicks = 180, cloudDamage = 2, cloudEvery = 20;
    // Balão de provocação ("taunt" {text, chance, ticks, gap}): sempre no especial, `chance` nos outros golpes,
    // no mínimo `gap` ticks entre um e outro
    std::string tauntText;
    float tauntChance = 0.25f;
    int tauntTicks = 50, tauntGap = 180;
    Texture2D icon{}, portrait{};  // tela de escolha ("icon", "portrait" no JSON); id 0 = sem arte
    Texture2D card{};              // ícone quadrado da grade de escolha (assets/ui/portraits/<id>_icon.png ou "icon")
    // tela de escolha: linha de estilo ("CORPO A CORPO"), nome do especial/parry e FORÇA, VELOCIDADE, ALCANCE (0..5)
    std::string style, specialName;
    std::string steps = "passo_leve", launchSfx;  // sons: passos (<steps>_1..3) e o projétil saindo
    std::string hurtVoice, koVoice, winVoice;     // voz: apanhou, K.O., venceu a luta ("voice" no JSON)
    float voiceVolume = 0.25f;                    // volume das vozes (fração dos EFEITOS): baixo, só um toque
    int stats[3] = {3, 3, 3};
    std::map<std::string, Anim> anims;  // walk, idle, crouch, jump, punch, kick, hit, block, ko, special, projectile, impact
};

// Luz do mapa: lado do sol (+1 direita, -1 esquerda), comprimento da sombra por px de altura e opacidade.
// shade/warm/rim: força (multiplicador) da penumbra do lado oposto, da luz quente do lado da luz e da borda de 1 px.
struct Light { int side = 1; float shadowLength = 0.45f, shadowAlpha = 0.35f; Color tint = WHITE; float shade = 1, warm = 1, rim = 1; };  // tint: multiplica o lutador

// ---------- mascotes (suportes) ----------
// Cada jogador escolhe UM na tela de escolha (os dois podem escolher o mesmo) e chama com 1 ou L (P1) / Numpad 7
// ou 3 (P2), gastando o medidor. Entra correndo do lado de quem chamou, faz a ação e sai correndo. Não apanha.
// Dados em assets/data/supports.json.
enum class PetKind { Bark, Attack, Heal };  // latido que empurra / arranhão (defensável) / cura gradual

struct SupportDef {
    std::string id, name;
    PetKind kind = PetKind::Bark;
    int cost = 30, cooldown = 300, maxUses = 2;  // cooldown em ticks; usos por round
    float runSpeed = 200;    // px/s
    float stopOffset = 24;   // onde para: px à frente (+) ou antes (-) do alvo, no sentido da corrida
    float pushMax = 7, pushRadius = 150, friction = 0.9f;  // latido: px/tick colado, alcance, atrito por tick
    int pushDamage = 0;                                    // latido: dano em quem está no alcance (defensável)
    Move attack = Move{6, 30, 0, 0, 0, 0.3f};              // arranhão: dano e defesa (o tempo vem da anim)
    int activeFrames = 2;                                  // arranhão: quadros ativos a partir do active_frame
    Box hitbox = {6, 4, 30, 24};                           // arranhão sem arte: px das patas, olhando para a direita
    float healTotal = 15;
    int healTicks = 120;
    int actionTicks = 36;    // duração da ação sem arte
    std::string sfx;         // som do bicho (tela de escolha e quando entra em ação)
    std::string description; // o que faz, para a tela de escolha (pode ter \n)
    std::string shortText;   // efeito em poucas palavras ("short"), na ficha da tela de escolha
    Texture2D icon{}, portrait{};  // ícone redondo (cabeça) e o bicho inteiro grande
    Texture2D card{}, stage{};     // tela de escolha: rosto quadrado (cartão) e pose parada do tamanho da corrida em 2x
    std::map<std::string, Anim> anims;  // run, action (latido/ataque/cura), fx (onda/arranhão/brilho)
};

struct SupportSet {
    float maxMeter = 100, gainHit = 1.5f, gainTaken = 2.0f;  // medidor ganha por ponto de dano dado / levado
    float comebackBelow = 0.25f, comebackGain = 1.5f;        // abaixo dessa fração da vida, o medidor enche x gain
    std::vector<SupportDef> list;
};

struct Roster {
    int chromaTolerance = 190;
    Light light;
    std::vector<CharDef> chars;
    SupportSet supports;
    int p1 = 0, p2 = 1;  // índices em chars
};

// Lê o JSON e carrega as imagens. Erro → false, `out` intacto, mensagem em `err`.
bool LoadRoster(const char* path, Roster& out, std::string& err);
// Carregamento em tarefas, sem threads (tela de loading). Lê os JSONs agora (erro = exceção) e põe na fila uma
// tarefa por animação/imagem, mascotes primeiro: quando as deles acabam, `pets` aponta para eles. A última tarefa
// entrega o Roster em `out` (e `pets` passa a apontar para out.supports). out.chromaTolerance já vale na hora.
// Tarefa que lança exceção: descartar a fila inteira (o que já carregou é liberado).
void QueueRoster(const char* path, std::vector<std::function<void()>>& tasks, Roster& out, const SupportSet*& pets);
void UnloadRoster(Roster& r);

enum class Act { Free, Punch, Kick, Hit, Block, KO, Jump, AirAttack, Special, Parry, CrouchPunch, CrouchKick, Pickup, Dash, Super, Dizzy, Spray, Suitcase };

struct Input { bool left, right, punch, kick, down, up; int support; bool punchHeld; int cmd; };  // support != 0: chamar o mascote; punchHeld = soco segurado (borrifo)
// cmd = comando já feito (botão do controle de toque): 1 = ↓→, 2 = ↓←, 3 = ←↓→, 4 = frente, frente

// Mascote em cena de um lutador.
struct Pet {
    int id = -1;          // índice em SupportSet::list; -1 = nenhum em cena
    int phase = 0, t = 0; // 0 chegando, 1 ação, 2 indo embora; ticks na fase
    float x = 0, prevX = 0;
    int dir = 1;          // para onde corre e olha
    bool done = false;    // latido/arranhão já aplicado
    float healRest = 0;   // fração de cura que ainda não virou 1 ponto
    int fxT = -1;         // efeito (onda, arranhão, brilho): ticks desde que começou; -1 = nenhum
    float fxX = 0, fxY = 0;
};

// Projétil do especial: um por lutador na tela (rebatido, passa para quem rebateu). impactT = -1 voando;
// >= 0 tocando o impacto onde acertou. art = de quem são as animações projectile/impact.
// kind 1 = furacão (anims super_projectile/super_impact; acerta hitsLeft vezes, uma a cada hitCD ticks)
// kind 2 = bomba em arco (vy px/tick, gravidade do dono): o "impacto" é a nuvem (anim cloud) que fica e dá dano
struct Shot { bool on; float x, prevX, y; int dir, t, impactT; float speed; int damage; const CharDef* art; int kind, hitsLeft, hitCD; float vy, prevY; };  // y = centro

// Objetos soltos (Thays): kind 1 = roupa da mala (voa girando, acerta uma vez), 2 = baforada do borrifo (só desenho).
struct Prop { int kind; float x, y, prevX, prevY, vx, vy, ang; int t, item, damage; };

struct Fighter {
    const CharDef* def;
    Color color;     // cor do retângulo quando falta arte
    float x;         // centro dos pés
    float y, vx;     // altura acima do chão e velocidade horizontal no ar
    float prevX, prevY;  // posição no tick anterior (desenho interpola entre os dois)
    int airT;        // ticks no ar; -1 = no chão
    int jumpDir, landT;
    float jumpBase;  // altura de onde pulou (chão ou plataforma)
    float jumpScale; // altura do pulo atual em fração de jump.height (1; pulo duplo = doubleJump)
    bool upHeld, doubleUsed;  // ↑ segurado no tick anterior; pulo duplo já usado neste pulo
    int passDir;     // passando por cima do oponente: +1 indo para a direita, -1 para a esquerda, 0 = não
    int spamMove, spamCount, spamT;  // último golpe começado (Act), repetições seguidas e ticks desde ele
    float spamScale;                 // fração do dano do golpe atual (repetido = menos)
    int chain;       // soco da corrente em andamento (0 = punch, 1 = punch2, 2 = punch3)
    bool chainOk;    // o soco atual acertou (sem defesa): o próximo J encadeia
    int punchBufT;   // ticks desde o último J (encadear aceita J apertado um pouco antes)
    int lastDir, tapT;    // arrancada: direção do tick anterior e ticks desde o último toque para a frente
    int hcfStage, hcfT;   // furacão: etapa do comando ← ↓ → (3 = completo) e ticks na etapa
    float guard;     // barra de guarda 0..100 (cheia = guarda quebra)
    int guardT;      // ticks desde a última defesa
    int warnT, warnKind, warnPct;  // aviso no HUD: ticks desde ele (-1 = nenhum); 1 = golpe repetido (-pct%), 2 = guarda quebrada
    bool shotLanded;  // o projétil deste especial já acertou, foi defendido ou chocou (recovery normal)
    int clashT;      // choque de projéteis: ticks desde o choque (-1 = nenhum), em clashX, clashY
    float clashX, clashY;
    // Só desenho (inercialização): troca de animação que faria o tronco saltar de lado vira um deslocamento que some
    // em ~3 quadros. drawAnim/drawFrame = o que estava na tela, blendX = deslocamento.
    mutable const Anim* drawAnim;
    mutable int drawFrame;
    mutable float blendX;
    int djT;         // efeito do pulo duplo: ticks desde o impulso (-1 = nenhum), em djX, djY (altura)
    float djX, djY;
    int facing;      // +1 = olhando para a direita
    int hp;
    Act act;
    int t;           // golpe: ticks desde o início; atordoado: ticks restantes
    int walkT, crouchT, animT;  // animT: relógio do lutador em ticks (quadro das animações em loop)
    int downT, motionT, backT;  // ticks desde que segurou ↓ / completou ↓ → (especial) / completou ↓ ← (parry)
    int parryCD;                // parry errado: ticks até poder tentar de novo
    int fxT;                    // brilho do parry: ticks desde o acerto (-1 = nenhum), centrado em fxX, fxY
    float fxX, fxY;
    bool walking, crouching, holdBack, hitDone, airUsed;  // airUsed: já deu o golpe no ar deste pulo
    bool blockLow;   // defendendo agachado (fica abaixado durante a defesa, com a caixa de dano baixa)
    int flash;
    float push;      // empurrão em px/tick, decai sozinho
    float slide, slideFriction;  // empurrão do latido: px/tick, multiplicado pelo atrito a cada tick
    const SupportSet* pets;      // mascotes disponíveis (nullptr = sem mascotes)
    float meter;                 // medidor dos mascotes (zera a cada round)
    int petChoice;               // mascote escolhido (índice na lista; -1 = nenhum)
    int petCD, petUses;          // espera (ticks) e usos neste round
    Pet pet;
    float chipRest;  // fração de vida da defesa que ainda não virou 1 ponto (acumula para o próximo golpe)
    Shot shot;
    bool unarmed;  // arremessou a arma e ainda não pegou de volta
    struct { bool on, down; float x, h, vx, vh; int dir, t; } weapon;  // arma solta: h = altura sobre o chão; down = caída
    int idleT;                   // ticks parada sem comando nem luta (idle_call)
    float spray;                 // tanque do borrifo 0..100
    bool sprayLock;              // esvaziou: travado até `restart`
    int suitcaseCD, specialCD;   // espera da mala e do especial (ticks)
    int tauntT, tauntGapT;       // balão: ticks desde que apareceu (-1 = nenhum) e desde o último
    Prop props[12];
};

Fighter MakeFighter(const CharDef& def, float x, Color color);
void UpdateFight(Fighter& a, Fighter& b, const Input& ia, const Input& ib, int& hitstop);
void UpdateAfterKO(Fighter& a, Fighter& b);
void UpdateCamera(const Fighter& a, const Fighter& b);  // câmera no ponto médio, presa às bordas do mundo  // depois do K.O.: animações seguem, sem comandos nem golpes
// level: 0 FÁCIL, 1 MÉDIO, 2 DIFÍCIL, 3 AVANÇADO (OPÇÕES)
const int CPU_LEVEL_N = 4;
Input CpuInput(const Fighter& me, const Fighter& foe, int level);
bool CanCallPet(const Fighter& f);  // escolheu um, medidor, espera, usos, nenhum em cena, não atordoado
// Mascote do lutador: shadowPass = só a sombra (junto das sombras dos lutadores); senão o bicho e o efeito.
void DrawPet(const Fighter& f, bool debug, float alpha, float pixelsPerUnit, const Light& light, bool shadowPass);
float JumpY(const CharDef& d, int ticksInAir);  // altura sobre o ponto de onde pulou (nunca abaixo dele)
// alpha = fração do tick atual já passada (0..1); pixelsPerUnit = escala da tela.
// shadowPass: desenha só a sombra no chão (chamar para os dois lutadores antes de desenhá-los).
void DrawFighter(const Fighter& f, bool debug, float alpha, float pixelsPerUnit, const Light& light, bool shadowPass);
void DrawEffects(const Fighter& f, bool debug, float alpha, float pixelsPerUnit);  // projétil, impacto, brilho do parry (por cima)
