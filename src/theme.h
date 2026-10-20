#pragma once
#include "raylib.h"

// Paleta do jogo (usada a partir da tela de loading; telas novas devem usar estas cores).
namespace Theme {
const Color Bg = {0x0E, 0x10, 0x20, 255};      // fundo azul-noite
const Color Panel = {0x1A, 0x1D, 0x33, 255};   // painel
const Color Border = {0x2E, 0x33, 0x58, 255};  // borda
const Color Gold = {0xF5, 0xB8, 0x41, 255};    // destaque principal
const Color Red = {0xE8, 0x42, 0x4F, 255};     // destaque secundário (vermelho arcade)
const Color Ink = {0xF2, 0xF2, 0xF2, 255};     // texto
const Color InkDim = {0x9A, 0xA0, 0xC3, 255};  // texto secundário
}  // namespace Theme
