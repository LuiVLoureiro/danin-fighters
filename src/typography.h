#pragma once
#include "raylib.h"

// Tipografia do jogo: todo texto em fonte pixel (OFL, assets/fonts), escalas inteiras (1x = 8 px).
// Display (títulos, VS, K.O., nomes grandes): Silkscreen Bold com degradê vertical, contorno escuro (2 px; 1 px na
// escala 1), sombra sólida deslocada e brilho de 1 px no topo das letras. Montado uma vez por texto/escala/degradê
// e guardado (nomes e títulos se repetem). UI (rótulos, opções, dicas): Silkscreen Regular sólida, contorno de 1 px
// opcional (sobre arte). Sem os arquivos: fonte padrão da raylib, sem quebrar.
struct Gradient { Color top, mid, bottom, shadow; };
namespace Grad {
extern const Gradient Fire;   // amarelo -> laranja -> vermelho (VS, K.O., LUTE!)
extern const Gradient White;  // branco -> azulado (nomes, títulos)
extern const Gradient Gold;   // dourado (VENCEU!, PRONTO!)
extern const Gradient P1;     // vermelho do 1P
extern const Gradient P2;     // azul do 2P/CPU
}  // namespace Grad

void LoadTypography();
void UnloadTypography();

// pos = canto de cima à esquerda das letras (sem contorno), em px inteiros. zoom != 1 só durante animação: escala em
// volta do centro, fora da grade de propósito. alpha some o texto inteiro.
void DrawDisplayText(const char* text, Vector2 pos, int scale, const Gradient& g, bool shadow, float zoom = 1,
                     float alpha = 1);
Vector2 MeasureDisplayText(const char* text, int scale);  // tamanho das letras (sem contorno e sombra)

void DrawUIText(const char* text, Vector2 pos, int scale, Color color, bool outline = false);
int MeasureUIText(const char* text, int scale);
int UITextHeight(int scale);
