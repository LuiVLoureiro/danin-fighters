// Teste da lógica de luta sem janela: build/Release/fight_test.exe (sai com código != 0 se falhar).
#undef NDEBUG  // assert ativo também no build Release
#include "fighter.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

static CharDef Char(int height, int health, Move kick) {
    CharDef d;
    d.jumpHeight = 45;
    d.jumpRise = 18;
    d.jumpFall = 12;
    d.name = "teste";
    d.height = height;
    d.health = health;
    d.walkSpeed = 70;
    d.kick = kick;
    d.kick.hitHeight = 0.4f;  // chute baixo, como o padrão do jogo
    return d;
}

// Roda ticks como o jogo: respeita hitstop e para no K.O.
static void Run(Fighter& a, Fighter& b, Input ia, Input ib, int ticks, int& hitstop) {
    for (int i = 0; i < ticks; i++) {
        if (hitstop > 0) { hitstop--; continue; }
        if (a.act == Act::KO || b.act == Act::KO) return;
        UpdateFight(a, b, ia, ib, hitstop);
        ia.punch = ia.kick = ib.punch = ib.kick = false;
        ia.support = ib.support = 0;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // assert aborta: sem buffer, o que já foi impresso aparece
    CharDef tere = Char(110, 90, {9, 40, 8, 4, 16});
    CharDef bart = Char(125, 130, {13, 42, 10, 4, 20});
    tere.kick.voice = "voz_golpe";  // vozes: golpe começando, apanhou, K.O.
    bart.hurtVoice = "voz_dano";
    bart.koVoice = "voz_ko";
    int hitstop = 0;

    // 1) anda até a parede chutando: todo chute colado precisa acertar até o K.O.
    Fighter a = MakeFighter(tere, 120, WHITE), b = MakeFighter(bart, SCREEN_W - 120, WHITE);
    int kicks = 0;
    while (b.act != Act::KO && kicks < 40) {
        Run(a, b, {false, true, false, false}, {}, 21, hitstop);   // anda ~350 ms
        Run(a, b, {false, false, false, true}, {}, 1, hitstop);    // chuta
        Run(a, b, {}, {}, 33, hitstop);                             // espera
        kicks++;
    }
    printf("K.O. depois de %d chutes, vida %d, x=%.1f/%.1f\n", kicks, b.hp, a.x, b.x);
    assert(b.act == Act::KO && b.hp == 0);
    assert(kicks <= 20);  // 15 acertos + poucos chutes de aproximação

    // 1b) depois do K.O.: o vencedor termina o golpe e fica livre; o perdedor segue caído com o relógio andando
    {
        int t0 = b.animT;
        for (int i = 0; i < 60; i++) UpdateAfterKO(a, b);
        printf("depois do K.O.: vencedor livre %d, perdedor caido %d, relogio da queda %d -> %d\n",
               a.act == Act::Free, b.act == Act::KO, t0, b.animT);
        assert(a.act == Act::Free && b.act == Act::KO && b.animT == t0 + 60 && b.hp == 0);
    }

    // 2) defesa: segurar para trás tira 20% do dano
    a = MakeFighter(tere, 150, WHITE);
    b = MakeFighter(bart, 150 + 50, WHITE);
    hitstop = 0;
    Run(a, b, {false, false, false, true}, {false, true, false, false}, 30, hitstop);
    // dano defendido = (9% da barra) x 0,15 x 130 = 1,755 -> 1 ponto agora, 0,755 guardado para o próximo
    printf("defesa andando para trás: vida %d de %d, guardado %.3f, recuou %.1f px\n", b.hp, bart.health, b.chipRest, b.x - 200);
    assert(b.hp == bart.health - 1 && fabsf(b.chipRest - 0.755f) < 0.001f && b.x > 200);

    // 3) ninguém atravessa nem sai da tela
    a = MakeFighter(tere, 30, WHITE);
    b = MakeFighter(bart, 60, WHITE);
    Run(a, b, {true, false, false, false}, {true, false, false, false}, 300, hitstop);
    printf("parede: x=%.1f/%.1f\n", a.x, b.x);
    assert(a.x >= 22 && b.x - a.x >= 46.9f);

    // 4) agachar: não anda e o soco (alto) passa por cima da caixa de dano mais baixa
    a = MakeFighter(tere, 150, WHITE);
    b = MakeFighter(tere, 150 + 46, WHITE);
    hitstop = 0;
    Input crouchFwd{false, false, false, false, true};
    crouchFwd.left = true;  // segura para frente agachado: não pode andar
    Run(a, b, {false, false, true, false}, crouchFwd, 30, hitstop);
    printf("agachado: x=%.1f vida %d agachado %d\n", b.x, b.hp, b.crouching);
    assert(b.crouching && b.x == 150 + 46 && b.hp == tere.health);

    // 5) pulo para frente: sobe até a altura do JSON, avança, aterrissa e volta a ficar livre;
    //    o chute (baixo) passa por baixo de quem está no alto do pulo
    a = MakeFighter(tere, 100, WHITE);
    b = MakeFighter(bart, 200, WHITE);
    hitstop = 0;
    Input jumpFwd{false, true, false, false, false, true};
    Run(a, b, jumpFwd, {}, 1, hitstop);
    float topY = 0;
    for (int i = 0; i < 60 && (a.act == Act::Jump || a.y > 0); i++) {
        Run(a, b, {}, {}, 1, hitstop);
        topY = std::max(topY, a.y);
    }
    printf("pulo: altura max %.2f (json %.0f), x=%.1f, estado livre %d\n", topY, tere.jumpHeight, a.x, a.act == Act::Free);
    assert(a.act == Act::Free && a.y == 0 && a.x > 100 && fabsf(topY - tere.jumpHeight) < 0.01f);

    // curva do pulo: começa e termina no chão, topo exato no tick `rise`, e a descida é mais forte que a subida
    // (gravidade = 2H/t²; subida em 18 ticks, descida em 12 ⇒ razão (18/12)² = 2,25)
    float gUp = 2 * tere.jumpHeight / (tere.jumpRise * tere.jumpRise);
    float gDown = 2 * tere.jumpHeight / (tere.jumpFall * tere.jumpFall);
    printf("gravidade: subida %.3f px/tick², descida %.3f px/tick² (x%.2f)\n", gUp, gDown, gDown / gUp);
    assert(JumpY(tere, 0) == 0 && JumpY(tere, tere.jumpRise) == tere.jumpHeight);
    assert(JumpY(tere, tere.jumpRise + tere.jumpFall) == 0 && gDown > gUp);
    for (int t = 1; t <= tere.jumpRise + tere.jumpFall; t++)  // sobe sempre até o topo, depois só desce
        assert(t <= tere.jumpRise ? JumpY(tere, t) > JumpY(tere, t - 1) : JumpY(tere, t) < JumpY(tere, t - 1));

    // mesa (plataforma na esquerda): andando, os pés param na lateral; pulando para lá, cai em cima e fica;
    // andando para fora, cai até o chão
    platform = {100, 30};
    a = MakeFighter(tere, 140, WHITE);
    b = MakeFighter(bart, 300, WHITE);
    Run(a, b, {true, false, false, false}, {}, 60, hitstop);
    float wallX = a.x;
    Run(a, b, {true, false, false, false, false, true}, {}, 1, hitstop);  // pula para a esquerda
    Run(a, b, {}, {}, 50, hitstop);
    float topX = a.x, topY2 = a.y;
    Run(a, b, {false, true, false, false}, {}, 40, hitstop);  // anda para fora
    Run(a, b, {}, {}, 40, hitstop);
    printf("mesa: parede x=%.1f, em cima x=%.1f y=%.1f, desceu x=%.1f y=%.1f\n", wallX, topX, topY2, a.x, a.y);
    assert(wallX == 100 && topX < 100 && topY2 == 30 && a.y == 0 && a.x > 100 && a.act == Act::Free);
    // encostada na lateral, o outro andando para cima dela: continua no chão, na lateral (não entra embaixo do tampo
    // e um K.O. ali não a deixa deitada em cima da mesa)
    a = MakeFighter(tere, 140, WHITE);
    b = MakeFighter(bart, 200, WHITE);
    Run(a, b, {true, false, false, false}, {true, false, false, false}, 90, hitstop);
    printf("mesa: empurrada contra a lateral x=%.1f y=%.1f\n", a.x, a.y);
    assert(a.x >= 100 && a.y == 0);
    platform = {};

    CharDef small = Char(80, 90, {9, 29, 8, 4, 16});  // tamanho do jogo: chute vai até ~38 px do chão
    a = MakeFighter(small, 150, WHITE);
    b = MakeFighter(tere, 150 + 46, WHITE);
    Run(a, b, {}, {false, false, false, false, false, true}, 3 + 6, hitstop);   // b subindo; o chute pega perto do alto
    Run(a, b, {false, false, false, true}, {}, 12, hitstop);                     // a chuta embaixo
    printf("chute embaixo do pulo: vida %d de %d\n", b.hp, tere.health);
    assert(b.hp == tere.health);

    // 5b) golpe no ar: no pulo, apertar golpe acerta quem está embaixo/à frente; só um por pulo; aterrissa normal
    {
        a = MakeFighter(tere, 100, WHITE);
        b = MakeFighter(tere, 150, WHITE);
        hitstop = 0;
        Input up{false, false, false, false, false, true}, punch{false, false, true, false};
        Run(a, b, up, {}, 1, hitstop);         // impulso
        Run(a, b, {}, {}, 7, hitstop);         // já no ar
        bool air = a.airT >= 0;
        Run(a, b, punch, {}, 1, hitstop);      // golpe no ar
        bool attacking = a.act == Act::AirAttack;
        for (int i = 0; i < 40 && a.act == Act::AirAttack; i++) Run(a, b, {}, {}, 1, hitstop);
        int hpAfter = b.hp;
        bool stillAir = a.airT >= 0;
        Run(a, b, punch, {}, 1, hitstop);      // segundo golpe no mesmo pulo (ainda no ar): não sai
        bool second = a.act == Act::AirAttack;
        assert(stillAir);
        for (int i = 0; i < 80 && a.act != Act::Free; i++) Run(a, b, {}, {}, 1, hitstop);
        printf("golpe no ar: no ar %d, atacando %d, vida %d de %d, segundo golpe %d, aterrissou livre %d\n",
               air, attacking, hpAfter, tere.health, second, a.act == Act::Free && a.y == 0);
        assert(air && attacking && hpAfter == tere.health - tere.airAttack.damage && !second);
        assert(a.act == Act::Free && a.y == 0);
    }

    // 6) fluidez: andando a 60 px/s, quanto o personagem anda a cada quadro de um monitor de 165 Hz?
    //    Sem interpolação a tela mostra a posição do último tick (passos de 0 ou 1 px, ritmo irregular).
    //    Com interpolação o passo por quadro é constante = 60/165 px. Medimos o desvio dos dois jeitos.
    {
        CharDef walker = Char(80, 90, {});
        walker.walkSpeed = 60;
        Fighter w = MakeFighter(walker, 50, WHITE), far = MakeFighter(walker, 350, WHITE);
        int hs = 0;
        double acc = 0, lastRaw = w.x, lastSmooth = w.x;
        float rawMin = 1e9f, rawMax = 0, smoothMin = 1e9f, smoothMax = 0;
        for (int frame = 0; frame < 165; frame++) {  // 1 segundo de monitor
            acc += 1.0 / 165;
            while (acc >= 1.0 / 60) {
                acc -= 1.0 / 60;
                w.prevX = w.x;
                UpdateFight(w, far, {false, true, false, false}, {}, hs);
            }
            float smooth = w.prevX + (w.x - w.prevX) * (float)(acc * 60);
            if (frame > 10) {  // ignora a saída do repouso, mede só em velocidade de cruzeiro
                rawMin = std::min(rawMin, (float)(w.x - lastRaw)); rawMax = std::max(rawMax, (float)(w.x - lastRaw));
                smoothMin = std::min(smoothMin, (float)(smooth - lastSmooth)); smoothMax = std::max(smoothMax, (float)(smooth - lastSmooth));
            }
            lastRaw = w.x;
            lastSmooth = smooth;
        }
        printf("fluidez a 165 Hz: sem interpolacao %.2f..%.2f px/quadro, com interpolacao %.3f..%.3f (ideal %.3f)\n",
               rawMin, rawMax, smoothMin, smoothMax, 60.0f / 165);
        fflush(stdout);
        assert(smoothMax - smoothMin < 0.01f);  // passo constante na tela
    }

    // 7) luz: borda de luz só nos contornos virados para o sol (e topo); sombra cai do lado oposto ao sol
    {
        Image sq = GenImageColor(10, 10, BLANK);
        ImageDrawRectangle(&sq, 2, 2, 5, 5, WHITE);  // quadrado de x 2..6, y 2..6
        Image rimR = EdgeMask(sq, 1), rimL = EdgeMask(sq, -1);
        auto lit = [](const Image& m, int x, int y) { return ((Color*)m.data)[y * m.width + x].a > 0; };
        printf("borda de luz: direita %d esquerda %d topo %d miolo %d | espelho: esquerda %d direita %d\n",
               lit(rimR, 6, 4), lit(rimR, 2, 4), lit(rimR, 4, 2), lit(rimR, 4, 4), lit(rimL, 2, 4), lit(rimL, 6, 4));
        assert(lit(rimR, 6, 4) && !lit(rimR, 2, 4) && lit(rimR, 4, 2) && !lit(rimR, 4, 4));
        assert(lit(rimL, 2, 4) && !lit(rimL, 6, 4));
        UnloadImage(sq); UnloadImage(rimR); UnloadImage(rimL);

        Vector2 feet = ShadowPoint(100, 0, 200, 1, 0.45f), head = ShadowPoint(100, 80, 200, 1, 0.45f);
        Vector2 headLeftSun = ShadowPoint(100, 80, 200, -1, 0.45f);
        printf("sombra (sol a direita): pe (%.0f,%.0f) cabeca (%.0f,%.0f); sol a esquerda: cabeca x=%.0f\n",
               feet.x, feet.y, head.x, head.y, headLeftSun.x);
        assert(feet.x == 100 && feet.y == 200);         // o pé encosta na própria sombra
        assert(head.x < 100 && head.y > 200);           // sol à direita: sombra vai para a esquerda, deitada no chão
        assert(headLeftSun.x > 100);
    }

    // 8) caixas medidas na arte: tronco (x 20..35), braço esticado (x 36..55, y 30..36) e pé de apoio à frente
    //    (x 36..45, y 92..99). O acerto tem que ser só o braço; o corpo, só o tronco. Todas as caixas perdem
    //    1 px de contorno em cada borda (exceto o chão).
    {
        Image fr = GenImageColor(60, 100, BLANK);
        ImageDrawRectangle(&fr, 20, 10, 16, 90, WHITE);
        ImageDrawRectangle(&fr, 36, 30, 20, 7, WHITE);
        ImageDrawRectangle(&fr, 36, 92, 10, 8, WHITE);
        FrameBoxes b = MeasureFrame(fr, 30);
        printf("medida: centro %.0f, tronco %.0f..%.0f alto %.0f, frente %.0f, acerto %.0f..%.0f altura %.0f..%.0f\n",
               b.anchor, b.body.x0, b.body.x1, b.body.y1, b.front, b.hit.x0, b.hit.x1, b.hit.y0, b.hit.y1);
        assert(b.anchor == 28 && b.body.x0 == -7 && b.body.x1 == 7 && b.body.y1 == 89);
        assert(b.front == 27 && b.hasHit);
        assert(b.hit.x0 == 9 && b.hit.x1 == 27 && b.hit.y0 == 64 && b.hit.y1 == 69);  // pé de apoio não entra
        UnloadImage(fr);

        // bolsa pendurada por alças finas: só a bolsa (parte grossa da ponta) acerta, alça e mão não
        Image bg = GenImageColor(90, 100, BLANK);
        ImageDrawRectangle(&bg, 20, 10, 16, 90, WHITE);  // tronco
        ImageDrawRectangle(&bg, 36, 30, 14, 5, WHITE);   // braço
        ImageDrawRectangle(&bg, 50, 31, 20, 2, WHITE);   // alça fina
        ImageDrawRectangle(&bg, 70, 25, 16, 16, WHITE);  // bolsa
        FrameBoxes bb = MeasureFrame(bg, 30);
        printf("bolsa: acerto %.0f..%.0f altura %.0f..%.0f\n", bb.hit.x0, bb.hit.x1, bb.hit.y0, bb.hit.y1);
        assert(bb.hasHit && bb.hit.x0 == 70 - 28 + 1 && bb.hit.x1 == 86 - 28 - 1);
        UnloadImage(bg);
    }

    // 8b) bolsada x agachar: agachada de verdade a bolsa passa por cima; em pé ou ainda descendo, acerta.
    //     Números das caixas medidas na arte do jogo (bolsa 51..70 px à frente, 58..78 de altura;
    //     Terezinha em pé 79, meio agachada 73, agachada 58).
    {
        CharDef bart2 = Char(91, 130, {});
        bart2.punch = {12, 99, 12, 4, 22};
        FrameBoxes bIdle{0, -15, 20, {-13, 0, 13, 90}, 21, false, {}};
        FrameBoxes bSwing{0, -15, 72, {-13, 0, 13, 90}, 70, true, {51, 58, 70, 78}};
        bart2.anims["walk"].boxes = {bIdle};
        bart2.anims["punch"].boxes = {bIdle, bSwing, bIdle};
        bart2.anims["punch"].activeFrame = 1;
        bart2.pushFront = 21; bart2.pushBack = 13;
        CharDef tz = Char(80, 90, {});
        FrameBoxes tStand{0, -15, 20, {-13, 0, 13, 79}, 25, false, {}};
        FrameBoxes tHalf{0, -15, 20, {-13, 0, 13, 73}, 25, false, {}};
        FrameBoxes tLow{0, -15, 20, {-13, 0, 13, 58}, 25, false, {}};
        tz.anims["walk"].boxes = {tStand};
        tz.anims["crouch"].boxes = {tStand, tHalf, tLow};
        tz.anims["crouch"].fps = 20;
        tz.pushFront = 25; tz.pushBack = 13;
        Input down{false, false, false, false, true};
        int hs = 0;
        Fighter at = MakeFighter(bart2, 100, WHITE), df = MakeFighter(tz, 155, WHITE);
        Run(at, df, {false, false, true, false}, {}, 30, hs);
        int standHp = df.hp;
        at = MakeFighter(bart2, 100, WHITE); df = MakeFighter(tz, 155, WHITE); hs = 0;
        Run(at, df, {false, false, true, false}, down, 30, hs);
        int crouchHp = df.hp;
        at = MakeFighter(bart2, 100, WHITE); df = MakeFighter(tz, 155, WHITE); hs = 0;
        Run(at, df, {false, false, true, false}, {}, 11, hs);   // agacha só quando a bolsa já está chegando
        Run(at, df, {}, down, 20, hs);
        int lateHp = df.hp;
        printf("bolsada: em pe vida %d, agachada vida %d, agachou tarde vida %d (de %d)\n", standHp, crouchHp, lateHp, tz.health);
        assert(standHp < tz.health && crouchHp == tz.health && lateHp < tz.health);
    }

    // 9) luta com caixas de arte: soco acerta só onde o punho desenhado alcança; quem recua escapa; corpos encostam
    {
        CharDef art = Char(80, 90, {});
        art.punch = {6, 99, 5, 3, 10};  // range 99 do JSON é ignorado: vale o punho desenhado
        FrameBoxes idle{0, -15, 20, {-8, 0, 8, 80}, 14, false, {}};
        FrameBoxes jab{0, -15, 32, {-8, 0, 8, 80}, 30, true, {8, 55, 30, 65}};
        art.anims["walk"].boxes = {idle};
        art.anims["punch"].boxes = {idle, jab, idle};
        art.anims["punch"].activeFrame = 1;
        art.pushFront = 14;
        art.pushBack = 8;
        int hs = 0;
        // punho vai até x+30; o outro (virado para a esquerda) apanha de x-14 a x+8 => acerta se estiver a < 44
        Fighter at = MakeFighter(art, 100, WHITE), df = MakeFighter(art, 146, WHITE);
        Run(at, df, {false, false, true, false}, {}, 20, hs);
        int farHp = df.hp;
        at = MakeFighter(art, 100, WHITE); df = MakeFighter(art, 140, WHITE); hs = 0;
        Run(at, df, {false, false, true, false}, {}, 20, hs);
        int nearHp = df.hp;
        at = MakeFighter(art, 100, WHITE); df = MakeFighter(art, 140, WHITE); hs = 0;
        Run(at, df, {false, false, true, false}, {false, true, false, false}, 20, hs);  // recua durante o startup
        printf("alcance da arte: a 46 px vida %d, a 40 px vida %d, recuando vida %d (estado %d, x=%.1f)\n",
               farHp, nearHp, df.hp, (int)df.act, df.x);
        assert(farHp == art.health && nearHp == art.health - 6);
        assert(df.hp == art.health && df.act == Act::Free);  // nem apanhou nem precisou defender

        at = MakeFighter(art, 100, WHITE); df = MakeFighter(art, 160, WHITE); hs = 0;
        Run(at, df, {false, true, false, false}, {}, 120, hs);
        printf("corpos encostados: distancia %.1f (frente %.0f + frente %.0f)\n", df.x - at.x, art.pushFront, art.pushFront);
        assert(fabsf(df.x - at.x - 2 * art.pushFront) < 0.01f);
    }

    // 10) fundo: sai o magenta (inclusive buraco fechado) e a franja rosada; rosa dentro do contorno fica
    Image img = GenImageColor(100, 60, {235, 10, 235, 255});
    ImageDrawRectangle(&img, 10, 10, 30, 30, BLACK);                  // contorno
    ImageDrawRectangle(&img, 12, 12, 26, 26, {255, 170, 230, 255});   // roupa rosa (lente, blusa...)
    ImageDrawRectangle(&img, 9, 20, 1, 4, {150, 20, 150, 255});       // franja de antialiasing colada no fundo
    ImageDrawRectangle(&img, 50, 10, 30, 30, BLACK);                  // anel (braço fechando um buraco)
    ImageDrawRectangle(&img, 55, 15, 20, 20, {225, 20, 225, 255});    // buraco com fundo um pouco diferente
    RemoveBackground(img, 215);
    Color* px = (Color*)img.data;
    auto alpha = [&](int x, int y) { return px[y * img.width + x].a; };
    printf("fundo: rosa %d, contorno %d, franja %d, buraco %d, borda %d\n",
           alpha(25, 25), alpha(10, 25), alpha(9, 21), alpha(65, 25), alpha(0, 0));
    assert(alpha(25, 25) && alpha(10, 25));                   // conteúdo fica
    assert(!alpha(9, 21) && !alpha(65, 25) && !alpha(0, 0));  // franja, buraco e fundo saem
    UnloadImage(img);

    // 11) especial (↓ → + soco): o projétil sai no fim do startup, atravessa a tela, acerta uma vez e toca o
    //     impacto; só um por vez na tela (com ele voando, o comando dá soco); segurando para trás, defende (20%)
    {
        Input down{}, downFwd{}, fwdPunch{};
        down.down = downFwd.down = downFwd.right = fwdPunch.right = fwdPunch.punch = true;
        auto motion = [&](Fighter& at, Fighter& df, Input back, int& hs) {
            Run(at, df, down, back, 1, hs);
            Run(at, df, downFwd, back, 1, hs);
            Run(at, df, fwdPunch, back, 1, hs);
        };
        CharDef sp = tere;
        sp.hasSpecial = true;
        sp.special = {10, 0, 14, 6, 22, 0.6f};
        sp.shotSpeed = 150;
        for (int block = 0; block < 2; block++) {
            Fighter at = MakeFighter(sp, 80, WHITE), df = MakeFighter(bart, 300, WHITE);
            int hs = 0, firedAt = -1, hitAt = -1, again = 0;
            Input back{};
            back.right = block;  // df olha para a esquerda: direita = para trás
            motion(at, df, back, hs);
            assert(at.act == Act::Special);
            for (int i = 0; i < 200 && hitAt < 0; i++) {
                Run(at, df, {}, back, 1, hs);
                if (firedAt < 0 && at.shot.on) firedAt = i + 1;
                if (at.shot.on && at.shot.impactT >= 0) hitAt = i + 1;
                if (i == 60) { motion(at, df, back, hs); again = at.act == Act::Special; }  // ainda voando: vira soco
            }
            printf("especial%s: sai no tick %d, acerta no tick %d, vida %d de %d, segundo lancamento %d\n",
                   block ? " (defesa)" : "", firedAt, hitAt, df.hp, bart.health, again);
            assert(firedAt == sp.special.startup && hitAt > firedAt && !again);
            assert(df.hp == bart.health - (block ? 1 : 10));  // 10% x 0,15 x 130 = 1,95 -> 1 (+0,95 guardado)
            Run(at, df, {}, {}, 60, hs);
            assert(!at.shot.on && df.hp == bart.health - (block ? 1 : 10));  // acerta uma vez só e some
        }
        // sem o comando é soco: só → + soco, ou ↓ antigo demais (passou da janela)
        Fighter at = MakeFighter(sp, 80, WHITE), df = MakeFighter(bart, 300, WHITE);
        int hs = 0;
        Run(at, df, fwdPunch, {}, 1, hs);
        bool plain = at.act == Act::Punch;
        at = MakeFighter(sp, 80, WHITE);
        Run(at, df, down, {}, 1, hs);
        Run(at, df, {}, {}, 12, hs);
        Run(at, df, fwdPunch, {}, 1, hs);
        printf("sem comando: so frente+soco = soco %d, baixo antigo = soco %d%c", plain, at.act == Act::Punch, 10);
        assert(plain && at.act == Act::Punch);
    }

    // 12) fundo preto (efeitos): preto some, o brilho vira opacidade e a cor fica cheia
    img = GenImageColor(20, 20, BLACK);
    ImageDrawPixel(&img, 5, 5, {255, 255, 255, 255});
    ImageDrawPixel(&img, 6, 5, {100, 60, 0, 255});
    RemoveBackground(img, 215);
    px = (Color*)img.data;
    printf("fundo preto: borda %d, nucleo %d, halo a=%d r=%d\n", alpha(0, 0), alpha(5, 5), alpha(6, 5), px[5 * 20 + 6].r);
    assert(!alpha(0, 0) && alpha(5, 5) == 255 && alpha(6, 5) == 100 && px[5 * 20 + 6].r == 255);
    UnloadImage(img);

    // 13) parry (↓ ← + soco). Bartinho olha para a esquerda: direita = trás.
    {
        CharDef par = bart;
        par.parry.on = true;  // padrão: startup 2, active 8, recovery 6/24, cooldown 30, hitstop 8, +10 de atordoamento
        Input parryIn{false, true, true, false, true, false}, punch{false, false, true, false};
        // a) golpe na caixa: Bartinho não perde vida, o atacante leva o próprio dano e fica atordoado mais tempo
        Fighter at = MakeFighter(tere, 150, WHITE), df = MakeFighter(par, 200, WHITE);
        int hs = 0, ticks = 1;
        UpdateFight(at, df, punch, parryIn, hs);  // apertam juntos: o parry fica ativo antes do soco
        for (; ticks < 20 && hs == 0; ticks++) UpdateFight(at, df, {}, {}, hs);
        printf("parry: no tick %d, hitstop %d, vida bartinho %d/%d, atacante %d/%d, atordoado %d\n", ticks, hs, df.hp,
               bart.health, at.hp, tere.health, at.t);
        assert(hs == 8 && df.hp == bart.health && at.hp == tere.health - tere.punch.damage);
        assert(at.act == Act::Hit && at.t == 25 && df.act == Act::Parry && df.hitDone);
        Run(at, df, {}, {}, 8 + 6, hs);  // hitstop + recovery curto
        printf("parry: depois do recovery bartinho livre %d, atacante ainda atordoado %d (%d ticks)\n",
               df.act == Act::Free, at.act == Act::Hit, at.t);
        assert(df.act == Act::Free && at.act == Act::Hit && at.t >= 15);  // sobra tempo para punir

        // b) erro: 2 + 8 + 24 ticks preso, depois 30 de espera (o comando vira soco)
        at = MakeFighter(tere, 60, WHITE);
        df = MakeFighter(par, 250, WHITE);
        hs = 0;
        UpdateFight(at, df, {}, parryIn, hs);
        for (ticks = 0; df.act == Act::Parry && ticks < 100; ticks++) UpdateFight(at, df, {}, {}, hs);  // ticks depois do aperto
        UpdateFight(at, df, {}, parryIn, hs);
        bool blocked = df.act == Act::Punch;
        Run(at, df, {}, {}, 40, hs);
        UpdateFight(at, df, {}, parryIn, hs);
        printf("parry errado: preso %d ticks, logo depois vira soco %d, depois da espera aparou de novo %d\n", ticks,
               blocked, df.act == Act::Parry);
        assert(ticks == 34 && blocked && df.act == Act::Parry);

        // c) projétil na caixa: volta 1,2x mais rápido, passa a ser do Bartinho e acerta quem lançou
        CharDef sp = tere;
        sp.hasSpecial = true;
        sp.special = {10, 0, 14, 6, 22, 0.6f};
        sp.shotSpeed = 150;
        par.parry.active = 60;  // janela larga só no teste: o foco é o que acontece depois
        at = MakeFighter(sp, 80, WHITE);
        df = MakeFighter(par, 300, WHITE);
        hs = 0;
        Input down{}, downFwd{}, fwdPunch{};
        down.down = downFwd.down = downFwd.right = fwdPunch.right = fwdPunch.punch = true;
        Run(at, df, down, {}, 1, hs);
        Run(at, df, downFwd, {}, 1, hs);
        Run(at, df, fwdPunch, {}, 1, hs);
        while (!(at.shot.on && at.shot.t == 30)) Run(at, df, {}, {}, 1, hs);
        Run(at, df, {}, parryIn, 1, hs);
        for (ticks = 0; at.shot.on && ticks < 200; ticks++) Run(at, df, {}, {}, 1, hs);
        printf("rebatido: dono bartinho %d, direcao %d, velocidade %.0f\n", df.shot.on, df.shot.dir, df.shot.speed);
        assert(!at.shot.on && df.shot.on && df.shot.dir == -1 && fabsf(df.shot.speed - 180) < 0.01f && df.hitDone);
        for (ticks = 0; at.hp == sp.health && ticks < 300; ticks++) Run(at, df, {}, {}, 1, hs);
        printf("rebatido: Terezinha vida %d de %d, Bartinho %d de %d\n", at.hp, sp.health, df.hp, par.health);
        assert(at.hp == sp.health - 10 && df.hp == par.health);
    }

    // 14) defesa agachada: golpe mid defende dos dois jeitos, low só agachado, high (no ar) só em pé.
    //     Defendendo agachado, fica abaixado durante a defesa (caixa de dano baixa).
    {
        auto taken = [&](Guard g, bool crouch, bool back, bool* lowDuringBlock) {
            CharDef at = tere;
            at.kick.guard = g;
            Fighter a = MakeFighter(at, 150, WHITE), b = MakeFighter(tere, 196, WHITE);
            Input df{};
            df.down = crouch;
            df.right = back;  // b olha para a esquerda: direita = trás
            int hs = 0;
            Run(a, b, {}, df, 10, hs);  // já agachado e segurando antes do golpe
            Run(a, b, {false, false, false, true}, df, 1, hs);
            for (int i = 0; i < 30; i++) {
                Run(a, b, {}, df, 1, hs);
                if (b.act == Act::Block && lowDuringBlock) *lowDuringBlock = b.crouching && b.blockLow;
            }
            return tere.health - b.hp;
        };
        bool low = false;
        int midStand = taken(Guard::Mid, false, true, nullptr), midCrouch = taken(Guard::Mid, true, true, &low);
        int lowStand = taken(Guard::Low, false, true, nullptr), lowCrouch = taken(Guard::Low, true, true, nullptr);
        int highStand = taken(Guard::High, false, true, nullptr), highCrouch = taken(Guard::High, true, true, nullptr);
        int open = taken(Guard::Mid, true, false, nullptr);
        printf("defesa: mid em pe %d agachado %d | low em pe %d agachado %d | high em pe %d agachado %d | sem defesa %d"
               " | abaixado na defesa %d\n", midStand, midCrouch, lowStand, lowCrouch, highStand, highCrouch, open, low);
        // defendido: 9% x 0,15 x 90 = 1,215 -> 1
        assert(midStand == 1 && midCrouch == 1 && lowStand == 9 && lowCrouch == 1 && highStand == 1 && highCrouch == 9);
        assert(open == 9 && low);
    }

    // 15) defesa em pé relativa à vida: a mesma sequência de golpes defendidos tira a mesma fatia da barra de
    //     quem tem pouca e de quem tem muita vida, bem menos que apanhar; e defender nunca derruba
    {
        auto blockRun = [&](int health, int hits, bool block) {
            CharDef df = tere;
            df.health = health;
            Fighter a = MakeFighter(tere, 150, WHITE), b = MakeFighter(df, 196, WHITE);
            Input back{};
            back.right = block;
            int hs = 0;
            for (int i = 0; i < hits; i++) {
                a.x = 150, b.x = 196;  // sem andar para longe com o empurrão
                Run(a, b, {false, false, false, true}, back, 1, hs);
                Run(a, b, {}, back, 40, hs);
            }
            return (float)(health - b.hp) / health;  // fração da barra perdida
        };
        float small = blockRun(90, 10, true), big = blockRun(130, 10, true), open = blockRun(90, 5, false) / 5 * 10;
        printf("10 chutes defendidos: vida 90 perde %.1f%% da barra, vida 130 perde %.1f%% (esperado 13.5%%); "
               "sem defender %.0f%%\n", small * 100, big * 100, open * 100);
        assert(fabsf(small - 0.135f) < 0.012f && fabsf(big - 0.135f) < 0.012f && open > 6 * small);
        CharDef df = tere;
        df.blockChip = 1;  // defesa cheia: mesmo assim não derruba
        Fighter a = MakeFighter(tere, 150, WHITE), b = MakeFighter(df, 196, WHITE);
        b.hp = 3;
        Input back{};
        back.right = true;
        int hs = 0;
        for (int i = 0; i < 3; i++) {
            Run(a, b, {false, false, false, true}, back, 1, hs);
            Run(a, b, {}, back, 40, hs);
        }
        printf("defesa com pouca vida: ficou com %d, caido %d\n", b.hp, b.act == Act::KO);
        assert(b.hp == 1 && b.act != Act::KO);
    }

    // 16) golpes agachados: baixo + soco/chute golpeia sem levantar (caixa de dano baixa) e volta ao agachado;
    //     sem o golpe agachado no JSON, levanta e golpeia como antes
    {
        CharDef cr = tere;
        cr.hasCrouchPunch = cr.hasCrouchKick = true;
        Fighter a = MakeFighter(cr, 150, WHITE), b = MakeFighter(tere, 300, WHITE);
        Input down{}, downPunch{}, downKick{};
        down.down = downPunch.down = downKick.down = true;
        downPunch.punch = true;
        downKick.kick = true;
        int hs = 0;
        Run(a, b, down, {}, 10, hs);
        Run(a, b, downPunch, {}, 1, hs);
        bool punch = a.act == Act::CrouchPunch && a.crouching;
        Run(a, b, down, {}, 30, hs);
        bool back = a.act == Act::Free && a.crouching;
        Run(a, b, downKick, {}, 1, hs);
        bool kick = a.act == Act::CrouchKick && cr.crouchKick.guard == Guard::Low;
        Fighter c = MakeFighter(tere, 150, WHITE);  // sem golpe agachado
        Run(c, b, down, {}, 10, hs);
        Run(c, b, downPunch, {}, 1, hs);
        printf("agachado: soco %d, volta agachado %d, chute baixo %d, sem golpe agachado levanta %d\n", punch, back, kick,
               c.act == Act::Punch);
        assert(punch && back && kick && c.act == Act::Punch);
    }

    // 17) mascotes: medidor, custo, um por vez, usos por round, não chama atordoado; latido empurra e tira dano,
    //     arranhão tira dano e pode ser defendido, cura gradual sem passar da vida máxima; medidor ganha com dano
    {
        SupportSet set;
        SupportDef bark, scratch, heal;
        bark.id = "perola"; bark.kind = PetKind::Bark; bark.cost = 25; bark.maxUses = 2; bark.cooldown = 0;
        bark.pushDamage = 5;
        scratch.id = "milka"; scratch.kind = PetKind::Attack; scratch.cost = 35; scratch.stopOffset = -34;
        scratch.runSpeed = 420; scratch.attack.damage = 6;
        heal.id = "belle"; heal.kind = PetKind::Heal; heal.cost = 50; heal.stopOffset = -22; heal.healTotal = 15;
        set.list = {bark, scratch, heal};
        auto fresh = [&](Fighter& a, Fighter& b) {
            a = MakeFighter(tere, 120, WHITE);
            b = MakeFighter(bart, 260, WHITE);
            a.pets = b.pets = &set;
        };
        Fighter a, b;
        // escolhe o mascote k (1..3) e aperta a tecla de chamar
        auto call = [&](int k) { a.petChoice = k - 1; Input i{}; i.support = 1; return i; };
        int hs = 0;
        fresh(a, b);
        a.meter = 10;
        Run(a, b, call(1), {}, 1, hs);
        bool poor = a.pet.id < 0;  // sem medidor: nada
        a.meter = 100;
        Run(a, b, call(1), {}, 1, hs);
        bool called = a.pet.id == 0 && fabsf(a.meter - 75) < 0.01f;
        Run(a, b, call(1), {}, 1, hs);
        bool onlyOne = a.pet.id == 0 && fabsf(a.meter - 75) < 0.01f;  // já em cena: não chama de novo
        float x0 = b.x;
        for (int i = 0; i < 400 && a.pet.id >= 0; i++) Run(a, b, {}, {}, 1, hs);
        printf("mascote latido: sem medidor %d, chamou %d, um por vez %d, empurrou %.1f px, vida %d de %d\n", poor, called,
               onlyOne, b.x - x0, b.hp, bart.health);
        assert(poor && called && onlyOne && b.x - x0 > 10 && b.hp == bart.health - 5);
        Run(a, b, call(1), {}, 1, hs);
        for (int i = 0; i < 400 && a.pet.id >= 0; i++) Run(a, b, {}, {}, 1, hs);
        Run(a, b, call(1), {}, 1, hs);
        printf("mascote: usos no round %d de %d, terceiro chamado %d\n", a.petUses, bark.maxUses, a.pet.id >= 0);
        assert(a.petUses == 2 && a.pet.id < 0);
        a.act = Act::Hit;
        a.t = 10;
        a.meter = 100;
        a.petChoice = 1;
        a.petUses = a.petCD = 0;  // só o atordoamento impede
        bool stunned = !CanCallPet(a);
        a.act = Act::Free;
        stunned = stunned && CanCallPet(a);

        for (int block = 0; block < 2; block++) {  // arranhão: dano cheio ou defendido
            fresh(a, b);
            a.meter = 100;
            Input back{};
            back.right = block;
            Run(a, b, call(2), back, 1, hs);
            for (int i = 0; i < 400 && a.pet.id >= 0; i++) Run(a, b, {}, back, 1, hs);
            printf("mascote arranhao%s: vida %d de %d\n", block ? " (defesa)" : "", b.hp, bart.health);
            assert(b.hp == bart.health - (block ? 1 : 6));  // defendido: 6% x 0,15 x 130 = 1,17 -> 1
        }

        fresh(a, b);
        a.meter = 100;
        a.hp = 50;
        Run(a, b, call(3), {}, 1, hs);
        for (int i = 0; i < 600 && a.pet.id >= 0; i++) Run(a, b, {}, {}, 1, hs);
        int healed = a.hp;
        fresh(a, b);
        a.meter = 100;
        a.hp = tere.health - 3;
        Run(a, b, call(3), {}, 1, hs);
        for (int i = 0; i < 600 && a.pet.id >= 0; i++) Run(a, b, {}, {}, 1, hs);
        printf("mascote cura: 50 -> %d, quase cheia -> %d (max %d), atordoado chama %d\n", healed, a.hp, tere.health, !stunned);
        assert(healed == 65 && a.hp == tere.health && stunned);

        fresh(a, b);  // medidor: chute de 9 no Bartinho
        Run(a, b, {}, {}, 1, hs);
        a.x = 150; b.x = 196;
        Run(a, b, {false, false, false, true}, {}, 30, hs);
        printf("medidor: quem bateu %.1f, quem apanhou %.1f (dano %d)\n", a.meter, b.meter, bart.health - b.hp);
        assert(fabsf(a.meter - 9 * set.gainHit) < 0.01f && fabsf(b.meter - 9 * set.gainTaken) < 0.01f);
    }

    // 12) arma arremessada (lápis da Thayna): acertando, cai no chão; sem ela não tem especial (sai o soco) e,
    //     passando por cima, pega de volta
    {
        Input down{}, downFwd{}, fwdPunch{}, fwd{};
        down.down = downFwd.down = downFwd.right = fwdPunch.right = fwdPunch.punch = fwd.right = true;
        CharDef th = tere;
        th.hasSpecial = th.dropsWeapon = true;
        th.special = {10, 0, 14, 6, 22, 0.6f};
        th.punchBare = th.punch;
        th.kickBare = th.kick;
        Fighter at = MakeFighter(th, 80, WHITE), df = MakeFighter(bart, 300, WHITE);
        int hs = 0;
        for (Input in : {down, downFwd, fwdPunch}) Run(at, df, in, {}, 1, hs);
        for (int i = 0; i < 300 && !at.weapon.down; i++) Run(at, df, {}, {}, 1, hs);
        printf("lapis: vida %d de %d, sem arma %d, caido em x %.0f (rival em %.0f)\n", df.hp, bart.health, at.unarmed,
               at.weapon.x, df.x);
        assert(df.hp == bart.health - 10 && at.unarmed && at.weapon.down && !at.shot.on && at.weapon.x < df.x);
        for (Input in : {down, downFwd, fwdPunch}) Run(at, df, in, {}, 1, hs);
        assert(at.act == Act::Punch && !at.shot.on);  // sem arma: soco
        Run(at, df, {}, {}, 40, hs);
        df.x = 370;  // rival longe: ela anda até o lápis
        int t = 0;
        while (t < 300 && at.act != Act::Pickup) { Run(at, df, fwd, {}, 1, hs); t++; }
        Run(at, df, {}, {}, 60, hs);
        printf("lapis: pegou de volta em %d ticks andando, com arma %d\n", t, !at.unarmed);
        assert(t < 300 && !at.unarmed && !at.weapon.on && at.act == Act::Free);
        // golpe forte (9+) sem defender: o lápis cai da mão; golpe fraco não
        Fighter hitter = MakeFighter(bart, at.x + 60, WHITE);
        Run(at, hitter, {}, {false, false, false, true}, 40, hs);  // chute de 13
        printf("lapis: chute forte -> sem arma %d, lapis solto %d\n", at.unarmed, at.weapon.on);
        assert(at.unarmed && at.weapon.on);
    }

    // 14) pulo duplo: soltar e apertar ↑ no ar sobe de novo (uma vez só); segurar ↑ não conta; aterrissa no chão
    {
        Fighter a = MakeFighter(tere, 100, WHITE), b = MakeFighter(bart, 300, WHITE);
        int hs = 0;
        Input up{};
        up.up = true;
        float single = 0, dbl = 0;
        Run(a, b, up, {}, 40, hs);  // segurando ↑ o pulo todo: só um
        for (int i = 0; i < 60 && a.act == Act::Jump; i++) { Run(a, b, {}, {}, 1, hs); single = std::max(single, a.y); }
        Run(a, b, {}, {}, 10, hs);
        assert(a.act == Act::Free && a.y == 0 && !a.doubleUsed);
        Run(a, b, up, {}, 1, hs);
        Run(a, b, {}, {}, 3 + 12, hs);  // impulso + subindo
        Run(a, b, up, {}, 1, hs);       // ↑ de novo no ar
        bool used = a.doubleUsed;
        Run(a, b, {}, {}, 1, hs);
        Run(a, b, up, {}, 1, hs);       // terceira vez: nada
        for (int i = 0; i < 120 && a.act == Act::Jump; i++) { Run(a, b, {}, {}, 1, hs); dbl = std::max(dbl, a.y); }
        printf("pulo duplo: simples %.0f px, duplo %.0f px, usado %d, no chao %d\n", single, dbl, used, a.y == 0 && a.act == Act::Free);
        assert(used && dbl > single * 1.5f && a.y == 0 && a.act == Act::Free);
    }

    // 15) pular por cima: só com os pés acima da caixa de dano inteira do outro. Pulo normal: passa por cima de quem
    //     está agachado, não de quem está em pé; pulo duplo passa por cima de quem está em pé. Andando não atravessa.
    {
        CharDef jt = tere;
        jt.jumpHeight = 90;
        jt.jumpRise = 22;
        jt.jumpFall = 15;
        jt.jumpSpeed = 150;
        Input fwdUp{}, fwd{}, crouch{};
        fwdUp.right = fwdUp.up = fwd.right = crouch.down = true;
        auto jumpOver = [&](bool foeCrouch, bool doubleJump) {
            Fighter a = MakeFighter(jt, 150, WHITE), b = MakeFighter(bart, 160, WHITE);
            int hs = 0;
            Input ib = foeCrouch ? crouch : Input{};
            Run(a, b, fwd, ib, 30, hs);  // andando: encosta e empurra, não passa
            assert(a.x < b.x);
            Run(a, b, fwdUp, ib, 1, hs);
            for (int i = 0; i < 120 && a.act != Act::Free; i++) {
                Run(a, b, i == 12 && doubleJump ? fwdUp : fwd, ib, 1, hs);  // ↑ de novo no ar (soltou no tick anterior)
            }
            Run(a, b, {}, ib, 2, hs);
            printf("pulo por cima (%s, %s): x %.0f, rival em %.0f\n", foeCrouch ? "agachado" : "em pe",
                   doubleJump ? "duplo" : "simples", a.x, b.x);
            return a.x > b.x && a.facing == -1 && b.facing == 1;
        };
        assert(!jumpOver(false, false));  // em pé, pulo normal: bate e volta
        assert(jumpOver(true, false));    // agachado, pulo normal: passa
        assert(jumpOver(false, true));    // em pé, pulo duplo: passa
    }

    // 16) golpe repetido seguido tira dano: 10, 8, 6, 4 (mínimo 40%); outro golpe zera; 3 s sem repetir também
    {
        CharDef sp = tere;
        sp.punch.damage = 10;
        sp.kick.damage = 10;
        sp.repeatStep = 0.2f;
        sp.repeatMin = 0.4f;
        sp.repeatReset = 150;
        Fighter a = MakeFighter(sp, 150, WHITE), b = MakeFighter(bart, 196, WHITE);
        b.hp = 10000;
        int hs = 0;
        auto strike = [&](bool punch, int wait) {
            int before = b.hp;
            a.x = 150; b.x = 186;
            Run(a, b, {false, false, punch, !punch}, {}, 1, hs);
            Run(a, b, {}, {}, wait, hs);
            return before - b.hp;
        };
        int d1 = strike(true, 40), d2 = strike(true, 40), d3 = strike(true, 40), d4 = strike(true, 40), d5 = strike(true, 40);
        int k1 = strike(false, 40), p1 = strike(true, 200), p2 = strike(true, 40);
        printf("golpe repetido: socos %d %d %d %d %d, chute %d, soco depois %d, soco 3 s depois %d\n", d1, d2, d3, d4, d5, k1, p1, p2);
        assert(d1 == 10 && d2 == 8 && d3 == 6 && d4 == 4 && d5 == 4 && k1 == 10 && p1 == 10 && p2 == 10);
    }

    // 17) canto: golpe em quem está na parede empurra quem bateu (perto); longe da parede, empurra quem apanhou
    //     reação: abaixo de 25% da vida, o medidor enche 1,5x (dando e levando)
    {
        auto kickAt = [&](float defX, float& atMoved, float& dfMoved) {
            Fighter a = MakeFighter(tere, defX - 46, WHITE), b = MakeFighter(bart, defX, WHITE);
            b.hp = 10000;
            int hs = 0;
            Run(a, b, {}, {}, 1, hs);  // Separate encaixa os dois (o da parede fica no limite)
            float ax = a.x, bx = b.x;
            Run(a, b, {false, false, false, true}, {}, 40, hs);
            atMoved = a.x - ax;
            dfMoved = b.x - bx;
        };
        float atMid, dfMid, atWall, dfWall;
        kickAt(220, atMid, dfMid);
        kickAt(SCREEN_W, atWall, dfWall);
        printf("canto: no meio atacante %+.1f, defensor %+.1f; na parede atacante %+.1f, defensor %+.1f\n", atMid, dfMid, atWall, dfWall);
        assert(fabsf(atMid) < 0.5f && dfMid > 5 && fabsf(dfWall) < 0.5f && atWall < -5);

        SupportSet ss;
        Fighter a = MakeFighter(tere, 150, WHITE), b = MakeFighter(bart, 196, WHITE);
        a.pets = b.pets = &ss;
        int hs = 0;
        b.hp = bart.health / 5;  // 20%: reação
        a.x = 150; b.x = 196;
        Run(a, b, {false, false, false, true}, {}, 30, hs);
        int dmg = bart.health / 5 - b.hp;
        printf("reacao: dano %d, quem bateu %.1f, quem apanhou (20%% de vida) %.1f\n", dmg, a.meter, b.meter);
        assert(dmg > 0 && fabsf(a.meter - dmg * ss.gainHit) < 0.01f && fabsf(b.meter - dmg * ss.gainTaken * ss.comebackGain) < 0.01f);
    }

    // 18) choque de projéteis: os dois somem no meio, ninguém leva dano. Recovery do especial: errando (projétil
    //     ainda voando) fica parado startup+active+whiff; acertando de perto, o normal
    {
        CharDef sp = tere;
        sp.hasSpecial = true;
        sp.special = {10, 0, 14, 6, 22, 0.6f};
        sp.shotSpeed = 150;
        sp.specialWhiff = 40;
        Input down{}, downFwd{}, fwdPunch{}, downBack{}, backPunch{};
        down.down = downFwd.down = downFwd.right = fwdPunch.right = fwdPunch.punch = true;
        downBack.down = downBack.left = backPunch.left = backPunch.punch = true;  // o da direita: "frente" = esquerda
        Fighter a = MakeFighter(sp, 80, WHITE), b = MakeFighter(sp, 300, WHITE);
        int hs = 0, clashAt = -1;
        Run(a, b, down, down, 1, hs);
        Run(a, b, downFwd, downBack, 1, hs);
        Run(a, b, fwdPunch, backPunch, 1, hs);
        for (int i = 0; i < 120 && clashAt < 0; i++) {
            Run(a, b, {}, {}, 1, hs);
            if (a.clashT >= 0) clashAt = i;
        }
        printf("choque: no tick %d em x %.0f, vidas %d/%d, projeteis %d %d\n", clashAt, a.clashX, a.hp, b.hp, a.shot.on, b.shot.on);
        assert(clashAt > 0 && !a.shot.on && !b.shot.on && a.hp == sp.health && b.hp == sp.health && fabsf(a.clashX - 190) < 12);

        auto specialTicks = [&](float foeX) {
            Fighter at = MakeFighter(sp, 80, WHITE), df = MakeFighter(bart, foeX, WHITE);
            df.hp = 10000;
            int h = 0, n = 3;
            Run(at, df, down, {}, 1, h);
            Run(at, df, downFwd, {}, 1, h);
            Run(at, df, fwdPunch, {}, 1, h);
            assert(at.act == Act::Special);
            while (at.act == Act::Special && n < 200) { Run(at, df, {}, {}, 1, h); n++; }
            return n;
        };
        int nearT = specialTicks(150), farT = specialTicks(SCREEN_W - 20);
        printf("especial: de perto (acertou) %d ticks, de longe (errou) %d ticks\n", nearT, farT);
        assert(farT - nearT >= 12);  // 18 a mais, menos a pausa do acerto que conta no de perto
    }

    // 19) barra de guarda: chute de 9 defendido enche 22,5; o 5º quebra a guarda (aberto, aviso); sem defender,
    //     esvazia sozinha. Aviso de golpe repetido com a porcentagem.
    {
        CharDef df = bart;
        df.guardPerDamage = 2.5f;
        df.guardBreak = 45;
        df.guardDelay = 60;
        df.guardRegen = 30;
        Fighter a = MakeFighter(tere, 150, WHITE), b = MakeFighter(df, 196, WHITE);
        b.hp = 10000;
        int hs = 0, blocks = 0;
        Input back{};
        back.right = true;  // b olha para a esquerda: segurar → é defender
        while (blocks < 10 && b.act != Act::Hit) {
            a.x = 150; b.x = 186;
            Run(a, b, {false, false, false, true}, back, 1, hs);
            for (int i = 0; i < 40 && b.act != Act::Hit; i++) Run(a, b, {}, back, 1, hs);
            blocks++;
        }
        printf("guarda: quebrou no %do golpe defendido, aberto %d ticks, aviso %d\n", blocks, b.t, b.warnKind);
        assert(blocks == 5 && b.act == Act::Hit && b.t > 20 && b.warnKind == 2 && b.guard == 0);
        Fighter c = MakeFighter(df, 196, WHITE);
        c.guard = 45;
        c.guardT = 0;
        Fighter d = MakeFighter(tere, 100, WHITE);
        int hs2 = 0;
        Run(d, c, {}, {}, 60 + 100, hs2);
        printf("guarda: 45 -> %.1f depois de 2,7 s sem defender\n", c.guard);
        assert(c.guard == 0);

        CharDef sp = tere;
        sp.repeatStep = 0.2f;
        Fighter e = MakeFighter(sp, 150, WHITE), f2 = MakeFighter(bart, 186, WHITE);
        f2.hp = 10000;
        for (int k = 0; k < 2; k++) {
            e.x = 150; f2.x = 186;
            Run(e, f2, {false, false, true}, {}, 1, hs2);
            Run(e, f2, {}, {}, 40, hs2);
        }
        printf("aviso: tipo %d, -%d%%\n", e.warnKind, e.warnPct);
        assert(e.warnKind == 1 && e.warnPct == 20);
    }

    // 20) ritmo das animações: todo desenho fica o mesmo número inteiro de ticks (8 fps viraria 7-8-7-8)
    {
        for (float fps : {6.89f, 8.0f, 9.47f, 14.0f, 16.0f}) {
            Anim an;
            SetAnimFps(an, fps);
            int last = 0, prev = 0, minH = 1000, maxH = 0;
            for (int t = 1; t <= 240; t++) {
                int st = AnimStep(an, t);
                if (st != prev) { minH = std::min(minH, t - last); maxH = std::max(maxH, t - last); last = t; prev = st; }
            }
            printf("ritmo: %.2f fps -> %d ticks por desenho (%.2f fps), min %d max %d\n", fps, an.hold, an.fps, minH, maxH);
            assert(minH == maxH && minH == an.hold && AnimTicks(an, 3) == 3 * an.hold);
        }
    }

    // 21) Danilo: arrancada (frente, frente) cruza a arena em < 1 s e tromba; trás freia. Corrente J, J, J só
    //     encadeia acertando. Furacão (← ↓ → + K) acerta várias vezes e depois ele fica tonto.
    {
        CharDef dn = tere;
        dn.dash.on = true;
        dn.dash.speed = 480;
        dn.dash.damage = 3;
        dn.dash.push = 5;
        dn.dash.maxTicks = 50;
        dn.punch = {4, 26, 3, 2, 8};
        dn.punch2 = {4, 26, 3, 2, 8};
        dn.punch3 = {7, 28, 5, 3, 14};
        dn.chainLen = 3;
        dn.hasSuper = true;
        dn.super = {3, 0, 16, 6, 18, 0.4f};
        dn.superSpeed = 120;
        dn.superHits = 4;
        dn.superEvery = 10;
        dn.dizzyTicks = 48;
        Input fwd{}, none{};
        fwd.right = true;
        int hs = 0;
        // arrancada sozinho (oponente longe, do outro lado): cruza a tela
        Fighter a = MakeFighter(dn, 30, WHITE), b = MakeFighter(bart, 370, WHITE);
        a.facing = 1;
        Run(a, b, fwd, {}, 1, hs); Run(a, b, none, {}, 1, hs); Run(a, b, fwd, {}, 1, hs);
        bool dashed = a.act == Act::Dash;
        int t = 0;
        float x0 = a.x;
        while (a.act == Act::Dash && a.t >= 0 && t < 80) { Run(a, b, fwd, {}, 1, hs); t++; }
        printf("arrancada: comecou %d, %d ticks, andou %.0f px, vida do rival %d de %d\n", dashed, t, a.x - x0, b.hp, bart.health);
        assert(dashed && t < 60 + dn.dash.startup && b.hp < bart.health);  // trombou no rival (dano leve)
        // sem loop: quem apanhou se recupera antes dele (a trombada não emenda outra arrancada nem soco)
        int u = 0;
        while (b.act == Act::Hit && u < 60) { Run(a, b, fwd, {}, 1, hs); u++; }
        printf("arrancada: rival livre em %d ticks, Danilo ainda no recovery %d\n", u, a.act == Act::Dash && a.t < 0);
        assert(a.act == Act::Dash && a.t < 0);
        // freio: trás no meio da corrida
        Fighter c = MakeFighter(dn, 30, WHITE), d = MakeFighter(bart, 370, WHITE);
        Run(c, d, fwd, {}, 1, hs); Run(c, d, none, {}, 1, hs); Run(c, d, fwd, {}, 1, hs);
        Run(c, d, fwd, {}, 5, hs);
        Input back{};
        back.left = true;
        Run(c, d, back, {}, 1, hs);
        printf("arrancada: tras freia -> recovery %d\n", c.act == Act::Dash && c.t < 0);
        assert(c.act == Act::Dash && c.t < 0);
        Run(c, d, none, {}, dn.dash.recovery, hs);
        assert(c.act == Act::Free);

        // corrente: 3 socos acertando = 4 + 4 + 7; com o rival defendendo, para no 1º
        auto chain = [&](bool block) {
            Fighter e = MakeFighter(dn, 150, WHITE), g = MakeFighter(bart, 186, WHITE);
            g.hp = 1000;
            int h = 0, steps = 0;
            Input blk{};
            blk.right = block;
            Input j{};
            j.punch = true;
            for (int i = 0; i < 90; i++) {
                Run(e, g, i % 4 == 0 ? j : none, blk, 1, h);
                if (e.act == Act::Punch) steps = std::max(steps, e.chain + 1);
                e.x = 150; g.x = 186;
                if (e.act == Act::Free && i > 5) break;
            }
            return std::make_pair(steps, 1000 - g.hp);
        };
        auto hit = chain(false), blk = chain(true);
        printf("corrente: acertando %d golpes (%d de dano); defendido %d golpe(s)\n", hit.first, hit.second, blk.first);
        assert(hit.first == 3 && hit.second >= 4 + 4 + 7 - 3 && blk.first == 1);

        // furacão: ←, ↓, → + K; vários acertos; depois tonto
        Fighter f = MakeFighter(dn, 80, WHITE), g = MakeFighter(bart, 200, WHITE);
        g.hp = 1000;
        Input l{}, dw{}, r{};
        l.left = true; dw.down = true; r.right = true; r.kick = true;
        int h3 = 0;  // pausa de acerto nova: a que sobrou dos testes acima engoliria as teclas do comando
        Run(f, g, l, {}, 1, h3); Run(f, g, dw, {}, 1, h3); Input rr{}; rr.right = true; Run(f, g, rr, {}, 1, h3);
        Run(f, g, r, {}, 1, h3);
        bool sup = f.act == Act::Super;
        bool dizzy = false;
        for (int i = 0; i < 200; i++) { Run(f, g, none, {}, 1, h3); dizzy |= f.act == Act::Dizzy; }
        printf("furacao: saiu %d, dano %d (%d acertos de 3), ficou tonto %d\n", sup, 1000 - g.hp, (1000 - g.hp) / 3, dizzy);
        assert(sup && 1000 - g.hp >= 9 && dizzy);
    }

    // 13) níveis da CPU: em 10 s contra um boneco parado, o AVANÇADO tira mais vida que o FÁCIL
    {
        SetRandomSeed(7);
        int dealt[CPU_LEVEL_N];
        for (int lv = 0; lv < CPU_LEVEL_N; lv++) {
            Fighter dummy = MakeFighter(bart, 100, WHITE), cpu = MakeFighter(tere, 260, WHITE);
            dummy.hp = 100000;
            int hs = 0;
            for (int i = 0; i < 600; i++) Run(dummy, cpu, {}, CpuInput(cpu, dummy, lv), 1, hs);
            dealt[lv] = 100000 - dummy.hp;
        }
        printf("cpu: dano em 10 s por nivel: facil %d, medio %d, dificil %d, avancado %d\n", dealt[0], dealt[1], dealt[2], dealt[3]);
        assert(dealt[0] < dealt[3]);
    }

    // 22) Thays: borrifo gasta o tanque e trava até recarregar 30; mala solta 3 roupas que acertam (e espera para
    // repetir); bomba em arco vira nuvem que machuca o oponente e não quem jogou; idle_call depois de 240 ticks parada
    {
        CharDef th = Char(108, 100, {6, 44, 7, 4, 12});
        th.spray.on = th.suitcase.on = th.hasSpecial = true;
        th.special = Move{2, 0, 16, 4, 20, 0.6f};
        th.shotGravity = 0.22f;
        th.shotLift = 3.2f;
        th.specialCooldown = 300;
        th.idleCall = 240;
        th.tauntText = "TEU C#$@!";
        int hs = 0;
        Fighter f = MakeFighter(th, 150, WHITE), g = MakeFighter(bart, 180, WHITE);
        g.hp = 1000;
        Input spray{}, hold{};
        spray.down = spray.punch = spray.punchHeld = true;
        hold.punchHeld = true;
        Run(f, g, spray, {}, 1, hs);
        bool started = f.act == Act::Spray;
        int ticks = 0;
        while (f.act == Act::Spray && f.t >= 0 && ticks < 200) { Run(f, g, hold, {}, 1, hs); ticks++; }
        int sprayDmg = 1000 - g.hp;
        Run(f, g, {}, {}, 20, hs);
        Run(f, g, spray, {}, 1, hs);
        bool locked = f.act != Act::Spray;  // vazio: sai o soco normal
        Run(f, g, {}, {}, 80, hs);          // recarrega 0,5 por tick: passa de 30
        Run(f, g, spray, {}, 1, hs);
        printf("borrifo: %d ticks, dano %d, travou vazio %d, volta com %.0f de carga %d\n", ticks, sprayDmg, locked, f.spray,
               f.act == Act::Spray);
        assert(started && ticks >= 50 && ticks <= 60 && sprayDmg >= 3 && locked && f.act == Act::Spray);

        f = MakeFighter(th, 100, WHITE);
        g = MakeFighter(bart, 260, WHITE);
        g.hp = 1000;
        Input dn{}, df{}, fk{}, fp{};
        dn.down = df.down = df.right = fk.right = fk.kick = fp.right = fp.punch = true;
        Run(f, g, dn, {}, 1, hs); Run(f, g, df, {}, 1, hs); Run(f, g, fk, {}, 1, hs);
        bool mala = f.act == Act::Suitcase;
        Run(f, g, {}, {}, 60, hs);
        int malaDmg = 1000 - g.hp;
        Run(f, g, dn, {}, 1, hs); Run(f, g, df, {}, 1, hs); Run(f, g, fk, {}, 1, hs);
        bool waits = f.act == Act::Kick;  // espera de 120 ticks: sai o chute
        printf("mala: saiu %d, 3 roupas = dano %d, espera %d\n", mala, malaDmg, waits);
        assert(mala && malaDmg == 9 && waits);

        f = MakeFighter(th, 100, WHITE);
        g = MakeFighter(bart, 200, WHITE);
        g.hp = 1000;
        Run(f, g, dn, {}, 1, hs); Run(f, g, df, {}, 1, hs); Run(f, g, fp, {}, 1, hs);
        bool bomb = f.act == Act::Special && f.tauntT >= 0;  // balão sempre na bomba
        int k = 0;
        while ((!f.shot.on || f.shot.impactT < 0) && k < 200) { Run(f, g, {}, {}, 1, hs); k++; }
        bool cloud = f.shot.on && f.shot.kind == 2 && f.shot.impactT >= 0;
        f.x = f.shot.x;  // a Thays dentro da própria nuvem
        Run(f, g, {}, {}, 200, hs);
        printf("bomba: saiu com balao %d, estourou em %d ticks, nuvem %d, dano no rival %d, na Thays %d, nuvem sumiu %d\n", bomb,
               k, cloud, 1000 - g.hp, th.health - f.hp, !f.shot.on);
        assert(bomb && cloud && 1000 - g.hp >= 2 && f.hp == th.health && !f.shot.on);

        f = MakeFighter(th, 100, WHITE);
        g = MakeFighter(bart, 250, WHITE);
        Run(f, g, {}, {}, 250, hs);
        int quiet = f.idleT;
        Run(f, g, hold, {}, 1, hs);
        printf("idle_call: parada %d ticks, qualquer comando zera: %d\n", quiet, f.idleT);
        assert(quiet >= 240 && f.idleT == 0);
    }

    // 23) Lui: o carro é a arrancada com "half_circle": frente, frente não sai; ← ↓ → + chute sai, tromba e acaba livre
    {
        CharDef lu = Char(110, 100, {9, 40, 8, 4, 16});
        lu.dash = {true, true, 330, 14, 9, 70, 36, 44};
        int hs = 0;
        Fighter f = MakeFighter(lu, 80, WHITE), g = MakeFighter(bart, 260, WHITE);
        g.hp = 1000;
        Input r{}, l{}, dw{}, rk{};
        r.right = l.left = dw.down = rk.right = rk.kick = true;
        Run(f, g, r, {}, 1, hs); Run(f, g, {}, {}, 1, hs); Run(f, g, r, {}, 1, hs);
        bool noTap = f.act != Act::Dash;
        Run(f, g, {}, {}, 30, hs);
        Run(f, g, l, {}, 1, hs); Run(f, g, dw, {}, 1, hs); Run(f, g, r, {}, 1, hs); Run(f, g, rk, {}, 1, hs);
        bool car = f.act == Act::Dash;
        Run(f, g, {}, {}, 200, hs);
        printf("carro: frente-frente nao sai %d, meia-lua + K sai %d, dano %d, livre no fim %d\n", noTap, car, 1000 - g.hp,
               f.act == Act::Free);
        assert(noTap && car && 1000 - g.hp == 14 && f.act == Act::Free);
    }

    // 24) arena mais larga que a tela (Mercadão): a câmera segue o ponto médio; quem recua para na borda da tela se o
    // outro está na outra borda; andando os dois para o mesmo lado, a câmera acompanha até a borda do mundo
    {
        arena = {0, 691, 153, 153};
        int hs = 0;
        Fighter a2 = MakeFighter(tere, 273, WHITE), b2 = MakeFighter(bart, 417, WHITE);
        Input l{}, r{};
        l.left = true;
        r.right = true;
        Run(a2, b2, l, r, 600, hs);  // os dois recuam: param nas bordas da tela, a câmera não anda
        float span = b2.x - a2.x, cam1 = arena.cam;
        Run(a2, b2, r, r, 900, hs);  // os dois para a direita: a câmera vai até o fim do mundo
        printf("arena larga: recuando ficam a %.0f px (tela %d), camera %.0f; andando juntos camera %.0f (max %.0f), x %.0f/%.0f\n",
               span, SCREEN_W, cam1, arena.cam, arena.x1 - SCREEN_W, a2.x, b2.x);
        assert(span <= SCREEN_W && fabsf(cam1 - 153) < 3 && arena.cam == arena.x1 - SCREEN_W && b2.x <= arena.x1);
        arena = {};
    }

    // 25) controle de toque: o botão do golpe manda o comando pronto (Input.cmd) junto do chute/soco
    {
        CharDef lu = Char(110, 100, {9, 40, 8, 4, 16});
        lu.dash = {true, true, 330, 14, 9, 70, 36, 44};
        int hs = 0;
        Fighter f = MakeFighter(lu, 80, WHITE), g = MakeFighter(bart, 260, WHITE);
        Input car{}, kick{};
        car.kick = kick.kick = true;
        car.cmd = 3;
        Run(f, g, car, {}, 1, hs);
        bool ok = f.act == Act::Dash;
        Fighter h = MakeFighter(lu, 80, WHITE);
        Run(h, g, kick, {}, 1, hs);
        printf("toque: botao SUPER sai o carro %d, chute sem comando nao %d\n", ok, h.act != Act::Dash);
        assert(ok && h.act != Act::Dash);
    }

    // sons: os golpes e o K.O. dos testes acima entraram na fila (o jogo toca e esvazia)
    auto has = [](const char* n) { return std::find(sfxQueue.begin(), sfxQueue.end(), n) != sfxQueue.end(); };
    printf("sons na fila: %zu (whoosh %d, ko %d, defesa %d, soco_forte %d)\n", sfxQueue.size(), has("whoosh"), has("ko"),
           has("defesa"), has("soco_forte"));
    assert(has("whoosh") && has("ko") && has("defesa") && has("soco_forte"));
    assert(has("voz_golpe") && has("voz_dano") && has("voz_ko"));

    puts("ok");
    return 0;
}
