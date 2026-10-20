#pragma once
#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Caixa em coordenadas do personagem olhando para a direita: x a partir do centro de massa (+ = para a frente),
// y para cima a partir dos pés. Assim serve para os dois lados e para qualquer altura (pulo).
struct Box { float x0, y0, x1, y1; };

// Caixas medidas na arte de um quadro.
struct FrameBoxes {
    float anchor;       // x (em px da textura) do centro do tronco: é ele que fica sobre a posição do lutador
    float left, right;  // extensão do desenho inteiro (para encaixar na tela igual ao desenho)
    Box body;           // tronco: colunas "cheias" do desenho (sem braço, punho, bolsa solta)
    float front;        // até onde o desenho vai para a frente na altura do tronco/braços (30%–90% da altura)
    bool hasHit;        // há um membro à frente do tronco?
    Box hit;            // o membro que mais avança à frente do tronco (punho, bolsa, pé)
    float feetX;        // centro dos pés (faixa de baixo) em relação ao ponto fixo
    float sink;         // px do recorte abaixo do corpo (óculos caindo...): desenha mais baixo, corpo no chão
};

// Animação já limpa: sem fundo, recortada na caixa do personagem e escalada para a altura do lutador.
struct Anim {
    std::vector<FrameBoxes> boxes;   // uma por quadro; vale também sem textura (testes)
    std::vector<Texture2D> frames;   // uma textura por quadro: silhueta (flash, sombra) e borda de luz saem dela num shader
    std::vector<float> anchors;      // x do centro do tronco de cada frame (fica sobre a posição do lutador)
    float fps = 10;                  // sempre 60 / hold (use SetAnimFps): ritmo exato na lógica de 60 ticks/s
    int hold = 6;                    // ticks que cada desenho fica na tela (inteiro: todos os desenhos duram igual)
    float spin = 0;                  // projétil: gira o desenho por código (graus/s; tênis do Danilo, 1 quadro só)
    int activeFrame = -1;            // golpes: quadro mostrado enquanto acerta (-1 = metade)
    std::vector<int> order;          // ordem de tocar (índices de quadro, pode repetir); vazio = 0, 1, 2...
    bool mirror = false;             // desenho feito olhando para o outro lado: espelha em relação ao lutador
    float breath = 0;                // respiração contínua: quanto (px) o topo da cabeça sobe no ciclo, 0 = desligada
    float breathPeak = 0.5f;         // em que ponto do ciclo (0..1) fica o pico da inspiração
    float breathBase = 0;            // altura (px) do quadro mais baixo da sequência: o fundo da onda
    float stepLen = 0;               // pés afastados − pés juntos (px): tamanho de um passo desenhado
    float bodyMedian = 0;            // mediana das alturas de corpo no jogo (px): outra animação pode copiar a escala
    std::vector<float> bodyHeights;  // altura do corpo de cada quadro no jogo (px), para conferir quadros fora do padrão
};

// Ritmo da animação em ticks inteiros: a lógica roda a 60 ticks/s, então só 60/N desenhos por segundo dão o
// mesmo tempo para todos os desenhos (8 fps exatos alternaria 7 e 8 ticks, e o olho lê isso como tropeço).
inline void SetAnimFps(Anim& a, float fps) {
    a.hold = std::max(1, (int)lroundf(60.0f / std::max(0.1f, fps)));
    a.fps = 60.0f / a.hold;
}
inline int AnimStep(const Anim& a, int ticks) { return ticks / a.hold; }    // quantos desenhos já passaram
inline int AnimTicks(const Anim& a, int steps) { return steps * a.hold; }   // ticks de `steps` desenhos

// Mede as caixas de um quadro já escalado (RGBA, olhando para a direita). `seedX` = x por onde procurar o tronco
// (o centro de massa serve). Caixas saem relativas ao ponto fixo: centro do tronco, ou dos pés se `feetAnchor`.
FrameBoxes MeasureFrame(const Image& img, float seedX, bool feetAnchor = false);

const float SHADOW_SQUASH = 0.15f;  // sombra deitada no chão: 1 px de altura vira 0,15 px de profundidade

// Pixels visíveis com vizinho vazio do lado `dx` (+1 direita, -1 esquerda) ou em cima: viram borda de luz.
Image EdgeMask(const Image& img, int dx);

// Onde cai no chão a sombra de um ponto na altura z (sol do lado sunSide: +1 direita, -1 esquerda).
Vector2 ShadowPoint(float x, float z, float groundY, int sunSide, float length);

// Sombra projetada: silhueta do frame deitada no chão, para o lado oposto ao sol. z = altura dos pés (pulo).
void DrawAnimShadow(const Anim& a, int frame, float feetX, float z, float groundY, bool flipX, int sunSide,
                    float length, float alpha);

// Luz do sol sobre o frame já desenhado em `dst`: penumbra do lado oposto, luz quente e borda de luz do lado do sol.
void DrawAnimLight(const Anim& a, int frame, Rectangle dst, bool flipX, int sunSide, float shade = 1, float warm = 1, float rim = 1);

// Apaga o fundo magenta (RGBA8). `tolerance` controla quanto da franja rosada perto do fundo também sai.
// Borda preta em vez de magenta (efeitos de luz): o brilho de cada pixel vira a opacidade dele.
void RemoveBackground(Image& img, int tolerance);

struct AnimOptions {
    int fps = 10;
    int refFrame = -1;         // quadro em que o corpo tem a altura normal (em pé); -1 = o corpo mais alto
    std::vector<int> layout;   // quadros em cada linha de CADA sheet, de cima para baixo (ex.: {2, 3}); vazio = 1 linha
    float bodyTarget = 0;      // > 0: a mediana das alturas de corpo vira este valor (copiar a escala de outra anim)
    bool feetAnchor = false;   // ponto fixo = centro dos pés (padrão: centro do tronco)
    bool refByWidth = false;   // escala pela LARGURA do corpo de referência (deitado: comprimento ≈ altura em pé)
    float pixelScale = 0;      // > 0: escala fixa (px do jogo por px da imagem), ignora as alturas
    std::vector<float> frameScale;  // multiplicador por quadro (a IA desenha algumas poses maiores); vazio = 1
};

// Várias sheets = uma animação só, em sequência; dentro de cada sheet os quadros vão na ordem de leitura
// (linha de cima da esquerda para a direita, depois a de baixo). `frames` = quadros por sheet quando não há layout.
// Arquivo ausente ou inválido devolve Anim vazia (quem desenha usa o fallback).
Anim LoadAnim(const std::vector<std::string>& files, int frames, int height, int chromaTolerance, const AnimOptions& opt = {});
void UnloadAnim(Anim& a);

// Imagem de tela (ícone, retrato): sem fundo, recortada no desenho e escalada com POINT para caber em w x h px
// (ou multiplicada por `scale`, se > 0). circle = ícone redondo: quadrado do topo do desenho (cabeça), fora do
// círculo fica transparente. Arquivo ausente: textura vazia (id 0); quem desenha usa um substituto.
// focusX/square (ícone): onde fica o quadrado no topo do desenho (0 = esquerda, 1 = direita) e o lado dele em fração
// do menor lado do desenho (bicho de lado olhando para a direita: 1 e ~0,6 pegam a cabeça).
// roundMask = false: recorta o quadrado do topo como o ícone, mas sem apagar fora do círculo (cartão quadrado).
Texture2D LoadPicture(const std::string& file, int chromaTolerance, int w, int h, bool circle, float scale = 0,
                      float focusX = 0.5f, float square = 1, bool roundMask = true);

// x da borda esquerda do frame desenhado com o centro de massa em feetX.
float AnimLeft(const Anim& a, int frame, float feetX, bool flipX);

// Desenha com os pés em `feet` (base da caixa no chão, centro de massa em feet.x). Devolve o retângulo desenhado.
// `pixelsPerUnit` = pixels de tela por pixel do jogo (posição encaixa nessa grade, permitindo movimento sub-pixel).
// `stretch` = px que o desenho estica para cima, com os pés parados (respiração contínua). `tint` multiplica as cores.
Rectangle DrawAnimFrame(const Anim& a, int frame, Vector2 feet, bool flipX, bool flash, float pixelsPerUnit = 1,
                        float stretch = 0, Color tint = WHITE);
