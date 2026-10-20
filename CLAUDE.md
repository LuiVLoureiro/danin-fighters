# Danin Fighters — Protótipo

Jogo de luta 2D **1 contra 1** (estilo Street Fighter II) com integrantes da família como lutadores.
Este é o **protótipo mínimo**: o objetivo é validar que os sprites gerados por IA funcionam no jogo.
Depois, novos personagens e golpes serão adicionados aos poucos, sem reescrever código.

## Objetivo desta versão (protótipo)
- Abrir o jogo no **menu inicial**, depois a **tela de escolha de personagem** e de lá a luta (P1 × P2).
- Os dois andam com a animação de andar dos sprites, se viram um para o outro, socam, chutam, perdem vida.
- Barra de vida para cada um, "K.O." quando a vida zera, tecla R reinicia a luta.
- Tem que ser **jogável e simples**. Nada além do listado aqui.

## Stack
- C++17 + raylib 5.x, baixada via CMake FetchContent (nada de instalação manual de bibliotecas).
- JSON com nlohmann/json via FetchContent.
- Windows é a plataforma principal. O código deve continuar compilável com Emscripten no futuro:
  sem threads, sem APIs do Windows, loop principal isolado em `UpdateDrawFrame()`.
- Código simples: structs + funções. Sem herança profunda, sem ECS, sem engine própria.

## Estrutura de pastas
```
CLAUDE.md
CMakeLists.txt
src/          main.cpp, fighter.cpp/.h, sprites.cpp/.h, (mais arquivos só se necessário)
assets/
  data/characters.json
  raw/<id>/andar_sheet.png    <- sprite sheets geradas pela IA (entrada, não editar)
```

## Sprites (muito importante)
**Tamanho dos PNGs em assets/raw:** guardados com ~3x a resolução que o jogo usa de verdade (efeitos de brilho 4x),
reduzidos com vizinho mais próximo em fator inteiro (sem cores novas: o magenta continua limpo); cenas do menu com
708 px (o mapa da arena fica inteiro, ver Arena). Os originais da IA ficam em `imagens/assets_raw_original`. PNG gigante deixa a abertura lenta e a tela de
loading travando: arte nova deve entrar já reduzida do mesmo jeito.
As sprite sheets vêm de uma IA de imagem e têm estas características:
- PNG grande (ex.: 1776x592 ou 2172x724), **uma linha horizontal** de N frames de mesma largura.
- Fundo **magenta** (#FF00FF), não transparente. As bordas podem ter tons rosados por antialiasing.
- Pixel art "falsa": os blocos de pixel são grandes e não alinhados a uma grade exata.
- O personagem não fica exatamente na mesma posição em cada célula.

O carregamento deve, em tempo de execução, no C++ (sem Python):
1. Fatiar a imagem em N células iguais (N vem do JSON).
2. Trocar o magenta e os tons próximos por transparente (tolerância configurável no JSON, padrão razoável para limpar a borda rosa).
3. Achar a caixa delimitadora dos pixels visíveis em cada frame.
4. Alinhar todos os frames **pelos pés** (base do CORPO, a maior mancha: pedaço solto abaixo dele, como óculos
   caindo, não levanta o personagem) e pelo centro de massa horizontal (braço esticado não desloca o corpo).
5. Escalar para a altura-alvo do personagem com filtro **POINT** (nunca bilinear), mantendo a proporção.
6. Se o arquivo não existir: desenhar um retângulo colorido com o nome do personagem. O jogo nunca pode quebrar por falta de arte.

O sprite original olha para a **direita**; espelhar horizontalmente quando o lutador estiver virado para a esquerda.

## Dados (data-driven)
Tudo que é conteúdo fica em `assets/data/characters.json`, nunca fixo no código. Exemplo de formato:
```json
{
  "chroma_tolerance": 80,
  "characters": [
    {
      "id": "terezinha",
      "name": "Dona Terezinha",
      "height": 110,
      "health": 90,
      "walk_speed": 70,
      "punch": { "damage": 6,  "range": 30, "startup": 5, "active": 3, "recovery": 10 },
      "kick":  { "damage": 9,  "range": 40, "startup": 8, "active": 4, "recovery": 16 },
      "anims": {
        "walk": { "file": "assets/raw/terezinha/andar_sheet.png", "frames": 6, "fps": 9 }
      }
    },
    {
      "id": "bartinho",
      "name": "Bartinho",
      "height": 125,
      "health": 130,
      "walk_speed": 55,
      "punch": { "damage": 9,  "range": 32, "startup": 7, "active": 3, "recovery": 14 },
      "kick":  { "damage": 13, "range": 42, "startup": 10, "active": 4, "recovery": 20 },
      "anims": {
        "walk": { "file": "assets/raw/bartinho/andar_sheet.png", "frames": 8, "fps": 10 }
      }
    }
  ],
  "p1": "terezinha",
  "p2": "bartinho"
}
```
Adicionar um personagem no futuro = colocar a sheet em `assets/raw/<id>/` + adicionar uma entrada no JSON (ele
aparece sozinho na tela de escolha; `p1`/`p2` = onde os cursores começam).
`"icon"` e `"portrait"` no personagem: imagens da tela de escolha (fundo magenta). Ícone = recorte quadrado do topo
do desenho (cabeça) dentro do círculo; retrato = desenho inteiro recortado. Sem arquivo: letra inicial / sem retrato.
`"style"`, `"special_name"` e `"stats"` (`strength`, `speed`, `reach`, 0..5): placa da tela de escolha.
`"portrait_head"` = altura da cabeça em px da imagem do retrato (a de assets/raw), do TOPO DO CRÂNIO (sem o volume do cabelo:
cabelo armado faz a pessoa parecer maior e encolhe o retrato) ao queixo. Cada IA enquadra de um
jeito (close ou de longe), então todos os retratos são escalados para a cabeça ter o mesmo tamanho na tela
(PORTRAIT_HEAD). Na tela, o ponto mais alto dos dois retratos fica na mesma linha (a do mais baixo); o mais alto
desce e a sobra sai embaixo da tela. Medir no editor de imagem ao adicionar um personagem.
Novas animações (idle, soco, chute, pulo, dano, nocaute...) entram em `anims` com o mesmo formato.
Sheet em grade (mais de uma linha): `"layout": [2, 3]` = quadros em cada linha, de cima para baixo; a ordem é a de
leitura (linha de cima da esquerda para a direita, depois a de baixo). Sem `layout` = uma linha só.
Grade regular: `"rows": 2, "cols": 2`. Várias sheets numa animação: `"files": [a, b, c]` (em sequência; a grade
vale para cada uma). `"scale_like": "walk"` = a mediana das alturas de corpo fica igual à da outra animação; com
`"scale_like_frame": N` (a partir de 1), igual à altura do corpo no quadro N dela.
`"anchor": "feet"` = ponto fixo no centro dos pés (padrão: centro do tronco).
`"pixel_scale": 0.27` = escala fixa (px do jogo por px da imagem), ignorando alturas: para folhas geradas em outra
escala (o personagem "cresce" numa pose). Medir na arte: a cabeça (ex.: distância brinco–olho) tem que dar o mesmo
tamanho que no idle; o console mostra a escala de cada folha (`SPRITE: ... escala`). Natália: block 0,273 e
crouch_kick 0,257 (as folhas vieram ~1,25x e ~1,2x maiores que a do idle).
`"frame_scale": [1, 0.9, ...]` = multiplicador da escala por quadro (ordem dos quadros das folhas, antes da
`sequence`): a IA às vezes desenha poses da mesma folha em tamanhos diferentes (agachada maior que de pé). Thayna:
calculado para a área do corpo sem o lápis ficar igual à do idle (desvio < 4% fica 1; limite 0,8–1,25).
`"sequence": [12, 12, 8, 4, ...]` = ordem de tocar, com números de quadro a partir de 1; repetir um número segura
o quadro (pausa). Com sequence, `fps` conta passos da sequência.
`"breath": 2.5` (+ `"breath_peak"`, 0..1, ponto do ciclo com o pico) = respiração contínua: o topo da cabeça
segue uma onda suave de 2,5 px no ritmo da animação, com os pés parados; o esticar compensa a altura de cada
quadro, então trocar de quadro não dá degrau. Usar em idle com poucos quadros DE ALTURA PARECIDA (com um quadro bem
mais baixo na sequência, todos são achatados até ele). `idle` toca só parado no chão;
ao carregar, o console lista cada quadro do idle (`IDLE:`) e marca os que destoam da mediana.
`punch` e `kick` são os botões 1 e 2 de cada personagem (o golpe desenhado pode ser outro: bolsada, soco forte...).
**Caixas medidas na arte, quadro a quadro, ao carregar** (lógica e desenho usam o mesmo quadro e a mesma posição):
- tronco = faixa contínua de colunas "cheias" (>= 40% da mais cheia); o centro dele é o ponto fixo do personagem.
  Topo do tronco = 1ª linha com um trecho contínuo de >= 35% da largura dele (a cabeça; lápis fino em diagonal por
  cima não sobe a caixa).
- onde apanha = tronco + o que estiver à frente na altura de 30%–90% (guarda, braço/perna esticados).
- onde acerta (só nos ticks `active`, no `active_frame`) = o membro que mais avança à frente do tronco, numa faixa
  de ±15% da altura em volta da ponta (punho, bolsa, pé; o pé de apoio não entra), e só a parte grossa da ponta:
  braço/perna entram inteiros, mas a bolsa pendurada por alças finas entra sozinha (alça e mão não acertam).
- o contorno escuro de 1 px da arte fica fora de todas as caixas: golpe que só raspa o contorno não conta
  (ex.: Terezinha totalmente agachada desvia da bolsada; em pé ou ainda descendo, apanha).
- colisão do corpo = no quadro parado do andar: para trás até o fim do tronco, para a frente até a guarda.
`range` e `hit_height` (padrão punch 0.75, kick 0.4) só valem para golpe sem arte ou sem membro à frente do tronco.
Efeitos (projétil, impacto) podem vir com fundo **preto** em vez de magenta: o brilho de cada pixel vira a
opacidade dele (o halo some suave sobre o cenário). `"height"` dentro de uma anim = tamanho próprio dela em px
(efeitos), em vez da altura do lutador.
Em golpes (`punch`, `kick`, `special`), `active_frame` diz qual quadro aparece enquanto o golpe acerta; os anteriores cobrem
o startup e os seguintes o recovery.
**Animação ausente usa fallback**: idle = frame 0 do walk; soco/chute = frame 0 do walk deslocado alguns pixels para frente + hitbox visível.
Personagem ainda sem `walk` (ex.: Natália, só com idle): o idle faz o papel do andar (toca andando, mede a colisão do
corpo no 1º quadro da sequência e serve de quadro parado dos golpes sem arte). Natália: idle em 3 sheets com
`"layout": [2, 4]` (linha de cima = poses extras, fora da sequência; linha de baixo = balanço da guarda).
Natália: chute (chute_sheet 3x2, chute lateral alto no 3º quadro) e especial INVOCAR LOBO (↓ → soco; especial_sheet
2x2: concentra, toca o chão, aponta = lançamento, volta) que solta o lobo espiritual (lobo_sheet 3x2, fundo preto =
brilho translúcido) no chão à frente dela (`"from": "ground"` no special: pés do projétil no piso, em vez da mão);
ao acertar toca `impact` (lobo_ataque: bote e se desfaz). Durante o especial, `special_aura` (chamas azul/dourada).
Guarda com escudo do Paysandu: anims `block` (defesa_alta) e `block_low` (defesa_baixa) — personagem que tem essas
anims mostra a guarda ao recuar (andando para trás = block; agachado para trás = block_low) e ao defender; ergue uma
vez e segura o último quadro; `block_aura` (oval azul) atrás dela na guarda. Auras: aditivas, em loop, atrás do
corpo, centradas pela caixa do desenho. Também: crouch (agachar 2x2), crouch_kick (rasteira 3x2, guard low), jump
(pular 3x2), air_attack (soco no pulo 2x2), hit (dano 2x2; as legendas escritas na folha original foram apagadas).
Essas vieram em JPEG 1024 (guardadas como PNG sem reduzir). ko (3x2: golpe, cai, bate, quica, tonta, deitada com
o polegar para cima; `scale_by` width, deitada = 110 px). Ainda sem soco, andar e soco agachado.
Thayna: idle (3 sheets 2x2; loop nos quadros 1-4, ida e volta; 5-12 = variações e provocações, fora da sequência),
retrato (portrait_head 386) e ícone. Arma = lápis gigante. Soco = cutucada (cutucada_sheet, `layout` [3,2,1]), chute =
lapisada (lapisada_sheet 3x2), especial LÁPIS VOADOR = arremesso (arremesso_sheet 2x2; o lápis desenhado no 3º quadro
foi apagado, o jogo desenha o projétil `lapis_voando` girando). Folhas em escalas diferentes: `pixel_scale` medido pela
altura da coroa à sola contra o idle (quase todas ~0,25; golpe agachado 0,318). Com lápis: walk (andar 3x2), crouch,
jump (pular 3x2, `sequence` [1,2,3,6,4,5]: impulso, ar, aterrissa), air_attack (pancada para baixo), crouch_punch
(cutucada baixa: quadros 1,2,1 da folha do golpe agachado) e crouch_kick (rasteira com o lápis no chão, guard low),
block/block_low (lápis na horizontal) e hit. Sem lápis (`_unarmed`): walk, crouch, jump (folha do pulo + a do
agachar para impulso e aterrissagem), air_attack (mergulho com as mãos), block, block_low e hit; sem arte agachada
sem lápis, os golpes agachados sem ele viram tapinha/empurrão em pé. ko = queda_e_levantar (`layout` [2,1,2],
quadros 1-3, `mirror`: cai para longe de quem bateu; 4-5, sentada tonta e de pé, sem uso: o jogo não tem
levantar). Golpe forte (9+ de dano, sem defender) ou K.O. com o lápis na mão: ele voa e cai no chão (a arte da queda é sem
lápis). Todas as anims dela têm `frame_scale` (golpe agachado e defesa agachada ~0,9; pegar o lápis 0,83–1,13).
**Arma arremessada** (`"drops_weapon": true` no special): acertando (dano normal) ou batendo na parede, o lápis quica
para trás e cai no piso embaixo (chão ou tampo da mesa, anim `dropped` = lapis_chao, quadro 1), atrás dos lutadores.
Sem ele: sem especial (o comando dá soco) e as anims/golpes `<nome>_unarmed` substituem os normais quando existem
(`idle_unarmed` = idle_sem 1-3 com respiração e gestos de vez em quando, `punch_unarmed` = tapinha, `kick_unarmed`
= empurrão; dados `punch_unarmed`/`kick_unarmed` no JSON). Parada no chão e passando por cima do lápis (16 px), pega
de volta: anim `pickup` (pegar_lapis, quadros 1-4) uma vez, vulnerável; apanhando no meio, o lápis continua no chão.
Cada round começa com o lápis na mão. A CPU não usa o especial.

Danilo (VELOCISTA, especial FURACÃO): arte feita pelo usuário (originais em imagens/pixel, cópias em
assets/raw/danilo):
- icone.png (close do rosto) e retrato.png (busto olhando o relógio, `portrait_head` 440 medido), do usuário;
  base.png = referência (retrato de corpo inteiro não serve: o VS encosta o retrato embaixo e mostrava só as pernas);
- idle_1..3 (2x2 cada): quadro 8 = guarda (`ref_frame` 7, SEM `breath`: a respiração estica cada quadro até o mais
  baixo da sequência, o pulinho de 90 px, e achatava a guarda em 18 px); de vez em quando olha o relógio (6-7),
  alonga (9-10) e dá um pulinho de aquecimento (1, 2, 4);
- andar_sheet (3x2, guarda; `fps` 10 fixo: passo curto demais para o cálculo automático);
- pular_sheet (3x2) e agachar_sheet (2x2, termina na posição de largada do velocista);
- arrancada_sheet (dash 3x2) e soco_sheet (soco leve 3x2);
- chute_sheet (chute alto 2x2) e tonto_sheet (dizzy 2x2);
- arremesso_sheet (2x2; o tênis desenhado no 3º quadro foi apagado, o jogo desenha o `projectile` tenis.png girando);
- furacao_sheet (super, corrida em círculo 2x3) e redemoinho_sheet (`super_projectile`, tornado em fundo preto).
- defesa_sheet / defesa_agachado_sheet (block / block_low 2x2), dano_sheet (hit 2x2, o boné voa), ko_sheet (3x2:
  golpe, cai, deitado, quica, deitado sem boné, joinha), golpe_ar_sheet (chute no ar 2x2), agachar_soco_sheet (2x2) e
  rasteira_sheet (crouch_kick 3x2, guard low).

As folhas do usuário vêm todas na mesma escala por grade: 2x2 = `pixel_scale` 0,233 (as de pé medem 0,231–0,237),
3 linhas x 2 = 0,3418; furacão 0,267 estimado. Falta só arte própria para `punch2`/`punch3` (usam o soco); a
danilo_freada não tem lugar no jogo. Golpes:
- frente-frente = arrancada (`dash`);
- J J J = corrente (`punch2`, `punch3`, só encadeia acertando; sem arte própria, usam o soco);
- K = chute;
- ↓→J = tênis voador;
- ←↓→K = furacão (vários acertos, depois `dizzy`).

Gerador local para personagens novos (Qwen-Image-2.1 em C:\IA, `tools/personagem/gerar.py`): ver
tools/personagem/GUIA.md.

Thays (arte do usuário em imagens/pixel, convertida para assets/raw/thays por um script único: PNG sem reduzir; o
frasco e as roupas desenhados no quadro 3 do arremesso e da mala foram apagados, e a grade branca da nuvem pintada de preto):
- `height_cm` 163 (px = cm × `px_per_cm` do topo do JSON, padrão 0,66 → 108 px); folhas 1024 em 2x2 com
  `pixel_scale` 0,225; idle_b veio 1,17x maior (`frame_scale` 0,857); idle_call 0,243 e `mirror` (arte olha para a esquerda).
- idle = idle_a + idle_b (A A A B), `idle_call` 240 = notebook em loop depois de 4 s parada, sem comando, sem o
  oponente golpeando e sem projétil na tela; qualquer comando volta ao idle.
- J palma (`push` 6 = empurrão alto), K chute tai chi (âncora no pé de apoio = `anchor` feet). Todas as anims com
  âncora nos pés, menos a mala (a mala desenhada no chão entraria na média dos pés).
- ↓ + J borrifo (`spray`): quadro 1 = entrada, 2-4 em loop enquanto SEGURA o soco; baforadas (anim `puff`, aditiva)
  a cada 6 ticks; cone à frente do frasco (`range`) dá 1 a cada 10 ticks, sem pausa de acerto. Tanque 0..100: -2/tick
  borrifando, +0,5/tick fora; vazio encerra e trava até 30 (barra rosa fina no HUD, escura piscando = travada).
- ↓ → K mala (`suitcase`): no fim do startup solta 3 roupas (anim `clothes`, 1 item por quadro) em leque -10°/0°/+10°,
  6 px/tick, girando 15°/tick, 3 de dano cada, defensáveis; espera 120 ticks (nela, sai o chute).
- ↓ → J bomba de perfume (`special` com `gravity`/`lift`): perfume.png girando em arco; no chão (ou tampo da mesa)
  ou no oponente vira nuvem (anim `cloud`, aditiva) por 180 ticks, 2 de dano a cada 20 em quem está dentro (a dona
  não sofre); espera 300 ticks. Projétil tipo 2 do motor (choca, é aparado e conta como "um por vez" como os outros).
- Balão `taunt` "TEU C#$@!" (balao.png + texto na fonte UI preta): sempre na bomba, 25% nos outros golpes, 180
  ticks entre balões, 50 ticks na tela com "pop".
- Segunda leva: ícone, retrato (`portrait_head` 350), andar com notebook (`sequence` começando no quadro 5, o único
  sem o notebook à frente: a colisão do corpo sai do 1º quadro e com o notebook ia a 34 px, a palma quase não
  alcançava), agachar (meia agachada: corpo 93–97 px contra 103 em pé), pulo (quadro 6 de pular2_sheet, a cópia
  "golpe_ar" que veio com a aterrissagem agachada, é o 1º da aterrissagem), defesa alta/baixa, dano, K.O. (3x2;
  quadros 5-6 espelhados na conversão para a cabeça ficar do lado da queda; sombra desenhada no 4 apagada), palma
  agachada (`crouch_punch`: só sai com o tanque do borrifo travado, ↓ + J é o borrifo) e rasteira (`crouch_kick`, low).
  Escalas medidas pela área do jeans contra o idle: andar 0,235, agachar/palma agachada 0,21, defesa baixa 0,205,
  defesa/dano 0,22, 3x2: pulo 0,265, rasteira e K.O. 0,30.
- Falta arte: golpe no ar (golpe_ar_thays.jpeg é o pulo de novo, sem golpe).

Lui (arte do usuário em imagens/pixel, convertida para assets/raw/lui por um script único: grade preta do jab pintada
de magenta; fundo rosa do teclado e do mouse virou magenta; teclado e mouse voando no quadro 3 apagados; sombra
magenta-escura embaixo do carro apagada; quadro 6 do K.O. espelhado para a cabeça ficar do lado da queda):
- `height` 110; folhas 2x2 com `pixel_scale` 0,242 (guarda ~455 px), pulo/golpe no ar 0,25; 3x2: jab 0,351, queda 0,34,
  K.O. 0,35; carro 0,345 (~156 px de comprimento). Ícone, retrato (`portrait_head` 330).
- idle = idle_1..3 (guarda parada; estalar os dedos, café): guarda, dedos, guarda, café, guarda, pulinho.
- J jab, J de novo (acertando) = direto (`punch2`): a mesma folha com `sequence` [1,2,3,6] e [1,4,5,6]. K chute
  circular. ↓+J soco agachado; ↓+K entrada nas pernas (`crouch_kick`, low, âncora no tronco: pelos pés o mergulho
  alcançava 114 px). No ar: joelhada.
- ↓→J mouse (special, mouse.png girando). ↓→K teclado: a mecânica da mala da Thays com um objeto só (`suitcase`
  angles [0], anim `clothes` = teclado.png), espera 150.
- ← ↓ → K Fiesta: a arrancada com `"half_circle": true` (frente, frente não sai): `dash_start` = chama o carro
  (startup 36, punível), `dash` = dirigindo (atropela: 14 de dano, empurrão 9), `dash_end` = sai do carro (recovery
  44) e a fumaça (`smoke`) cobre o carro sumindo.

## Regras de jogo do protótipo
- Resolução lógica **384x216**, desenhada direto na janela com zoom inteiro (Camera2D, sem filtro, centralizada).
  Janela inicial 1152x648, redimensionável.
- Lógica em passo fixo de 60 ticks/s. Durações de golpes em frames. O desenho interpola a posição entre o tick
  anterior e o atual e encaixa na grade de pixels da tela: em monitores de 144/165 Hz o movimento fica constante
  (sem isso cada posição fica 2 ou 3 quadros na tela, num ritmo irregular).
- Andar sem `fps` no JSON: o ritmo da animação é calculado para o pé não deslizar —
  passo = (pés afastados − pés juntos) medido na arte; fps = velocidade × quadros / (2 × passo).
  Andando de costas, o ciclo toca ao contrário.
- Chão plano. Cenário da luta = arena do stages.json (abaixo); sem a imagem, o cenário desenhado: gradiente + linha
  do chão, piso a partir do horizonte (y 176) e lutadores pisando em y 200 (o menu inicial sempre usa este).
- Luz do mapa no JSON (`light`): `sun` ("right"/"left"), `shadow_length` (quanto a sombra estica por px de altura),
  `shadow_alpha`. Desenho: sol no horizonte e chão mais claro do lado do sol; sombra = silhueta do frame atual
  deitada no chão para o lado oposto ao sol (no pulo fica no chão, mais fraca); no personagem, penumbra azulada
  do lado oposto, luz quente e borda de luz de 1 px nos contornos virados para o sol e no topo. Embaixo dos pés,
  sombra de contato (elipse suave, some ao subir no pulo). A arena pode ter o próprio `light` (stages.json), que vale
  na luta por cima deste, com `shade`, `warm` e `rim` (força da penumbra, da luz quente e da borda; 1 = padrão).
- Ao tirar o fundo, a borda que sobra com tom de magenta (cabelo escuro arroxeado) é neutralizada (só a 2 px do fundo).
- Andar para frente e para trás (a animação de andar toca; parado = frame 0).
- Pular segurando para cima: `jump.height` (px), `jump.rise` (ticks até o topo), `jump.fall` (ticks do topo ao
  chão, menor que rise = descida com gravidade mais forte, pulo menos flutuante), `jump.speed` (px/s horizontal).
  Curva exata: meia-parábola na subida (g = 2H/rise²) e na descida (g = 2H/fall²). 3 ticks de impulso e 4 de
  aterrissagem no chão. A direção segurada no impulso fica travada. Animação `jump`: 1º quadro = impulso, 2 últimos = aterrissagem, o resto no ar.
  No ar não defende; quem apanha no ar cai normalmente.
- Pulo duplo (sem sprite próprio): no ar, soltar e apertar ↑ de novo = nova meia-parábola a partir de onde está, com
  `jump.double` (0,6; padrão 0,8) da altura, na direção segurada naquele momento; um por pulo (vale também caindo da mesa) e o
  golpe no ar fica liberado de novo. A anim `jump` recomeça a parte do ar; efeito desenhado no código: dois anéis de ar
  achatados abrindo embaixo dos pés + 4 baforadas brancas (0,3 s) e whoosh. `"double": 0` desliga.
- Câmera vertical da luta: quando o topo da cabeça mais alta passa de y 44 (perto do HUD), o mundo (cenário, TV,
  lutadores, efeitos) desce até 100 px, suave (sobe rápido, volta mais devagar), em px inteiros da tela; HUD e telas
  por cima ficam paradas e a vinheta presa à tela. O fundo carrega 100 px de imagem acima da faixa da luta para não
  aparecer borda. Zera a cada round.
- Golpe no ar: no pulo, soco ou chute dispara `air_attack` (dados no JSON do personagem, anim `air_attack`),
  um por pulo; a parábola continua igual; se aterrissar antes de terminar, o golpe é cortado e ele aterrissa. Em anims, `ref_frame` (opcional) diz qual
  quadro tem a altura normal em pé, para pose com braço para cima não encolher o personagem.
- Agachar segurando para baixo: não anda, toca a animação `crouch` e fica no último quadro; a caixa de dano
  fica com a altura desse quadro. Agachado + para trás = defesa.
- Golpes agachados: segurando baixo, soco/chute viram `crouch_punch`/`crouch_kick` (dados e anims com esses nomes):
  golpeia sem levantar (caixa de dano baixa desde o 1º tick) e, segurando baixo, volta direto ao último quadro do
  agachado. Rasteiros são `"guard": "low"` (só defende agachado). Personagem sem esses golpes no JSON levanta e
  golpeia. Na anim: `"scale_like": "crouch", "scale_like_frame": 3` = corpo com a altura do agachado (último quadro),
  não a de pé. Baixo + frente + soco é o especial de quem tem; baixo + trás + soco é o parry de quem tem.
- Os lutadores sempre se viram um para o outro, não se atravessam e não saem da tela. Exceção, pular por cima:
  só quem, ao encostar no outro no ar, está com os pés acima da caixa de dano INTEIRA dele (a do quadro atual: em pé
  ou agachado). Aí atravessa e, ao aterrissar, cai do outro lado, no sentido em que ia (mesmo com o outro na parede,
  que é empurrado). Pés abaixo da cabeça dele: bate e volta. Pulos calibrados para isso (caixas em pé/agachado:
  Terezinha 93/67, Bartinho 104/75, Natália 90/83, Thayna 106/96): `jump.height` 86–88 passa por cima de quem está
  agachado e não de quem está em pé (Thayna agachada, com o lápis, só com pulo duplo); pulo duplo (`double` 0,6)
  passa por cima de todos em pé. `rise` 22–23, `fall` 15–16, `speed` 140–150 px/s.
- Soco e chute: startup → active (hitbox ativa) → recovery. Ao acertar: dano, empurrão curto, flash branco no atingido, hitstop de 4 frames.
- Segurar para trás = defesa, sem flash (parado ou andando para trás: recuar já é estar defendendo).
  Com `block`/`block_low` na arte, quem recua mostra a guarda (em pé andando para trás; agachado para trás) e a
  mesma pose ao defender: Terezinha e Bartinho (defesa_sheet / defesa_agachado_sheet 2x2: guarda, braços
  cruzados, impacto, segura) e Natália (escudo). Sem essa arte (Thayna), defende igual mas sem pose de guarda. Dano defendido relativo à barra de quem defende: o dano do golpe é lido
  como "% da barra" e quem defende perde `block_chip` (0,15) disso — golpe de 12 defendido = 1,8% da barra, tanto
  com 90 quanto com 130 de vida (sem defender, 12 pontos = 13% / 9%). A fração que não fecha 1 ponto fica guardada
  para o próximo golpe. Defender nunca derruba (fica no mínimo com 1). `"block_chip"` no topo do JSON ou no
  personagem. Em pé (← ) ou agachado (↙), conforme a altura do
  golpe — `"guard"` no golpe do JSON: `"mid"` (padrão: defende dos dois jeitos), `"low"` (rasteiro: só agachado),
  `"high"` (por cima: só em pé; padrão do `air_attack`, então quem só defende agachado apanha de quem pula).
  Projétil e golpe devolvido pelo parry são mid. Defendendo agachado, fica abaixado durante toda a defesa (anim
  `crouch` no último quadro, caixa de dano baixa); durante a defesa dá para trocar entre em pé e agachado.
  A CPU agacha para defender golpe baixo e fica em pé contra o resto.
- Barra de guarda: cada golpe defendido enche dano x `per_damage` (2,5) de 100 (chute de 9 = 22,5: o 5º seguido
  quebra). Cheia: a guarda quebra, ele fica aberto `break` (0,75 s, anim de dano, flash, som tilintar) e a barra
  zera. Depois de `regen_delay` (1 s) sem defender, esvazia `regen` (30) por segundo. `"guard"` no topo do
  characters.json (sem ele, desligada). Golpe repetido enche menos (dano menor): variar quebra a guarda mais rápido.
- Canto: golpe em quem está encostado na parede (ou na lateral da mesa) não empurra quem apanhou (não tem para onde
  ir): o empurrão inteiro vai para quem bateu, se ele estiver a até 100 px (projétil de longe não empurra o atirador).
  Ninguém fica preso no canto apanhando sem fim.
- Golpe repetido (fair play): começar o MESMO golpe de novo (soco, chute, agachados, no ar, especial) até 2,5 s depois
  do anterior tira dano: 100%, 80%, 60%, 40% (mínimo). Outro golpe zera a contagem; conta o uso, acertando ou não.
  O projétil sai com o dano reduzido. `"repeat_penalty": {step 0,2, min 0,4, reset 2,5 s}` no topo do characters.json
  (sem ele, desligado). Dano menor também muda o som (soco_fraco abaixo de 9) e não derruba o lápis da Thayna.
- Golpes extras por JSON (Danilo, Lui): `"dash"` {speed, damage, push, max_ticks, startup, recovery, half_circle} = frente, frente
  (com `half_circle`: ← ↓ → + chute; anims opcionais `dash_start`/`dash_end` no preparo e no recovery, `smoke` ao acabar;
  a trombada conta da frente do desenho, o para-choque, não só do corpo)
  (2 toques em 12 ticks): `startup` (8) ticks de preparo parado, corre até a borda ou trombar no oponente (dano +
  empurrão); trás freia; ao parar (trombou, defendido, borda, freio ou fim) fica `recovery` (20) ticks parado e
  punível — mais que o atordoamento de quem apanhou (15), então a trombada não emenda outra arrancada nem soco (era
  um loop infinito); defendida, o rival tem 12 ticks para punir. Anim `dash` (sem ela, o andar no dobro do
  ritmo); preparo e recovery mostram o 1º quadro. `"punch2"`/`"punch3"` = corrente J, J, J: o próximo sai se o anterior ACERTOU (defendido não
  encadeia) e J foi apertado até 10 ticks antes. `"super"` {damage por acerto, startup, active, recovery, speed,
  hits, hit_every, dizzy} = ← ↓ → + chute: projétil-redemoinho (anims super_projectile/super_impact; sem elas,
  desenhado por código) que acerta `hits` vezes; no fim do golpe fica tonto `dizzy` ticks (anim `dizzy`, sem ela
  `hit`), punível. Anim `projectile` com `"spin"` (graus/s) gira o desenho por código.
- Especial = comando do Street Fighter: ↓, depois → (para a frente; ↘ no meio vale), depois soco — o → até 10 ticks
  depois de soltar o ↓ e o soco até 10 ticks depois do →. Só no chão, só quem tem `special` no JSON; senão (ou com
  o projétil ainda na tela) sai um soco normal. Dados: `{damage, startup, active, recovery, speed}`.
  Anim `special` toca como um golpe; no fim do startup (`active_frame`) o projétil sai da ponta da mão medida
  na arte e voa reto a `speed` px/s (anim `projectile` em loop; acerta com a bola, o rastro não conta).
  Um projétil por lutador na tela. Choque: dois projéteis voando que se encontram na horizontal (em qualquer
  altura: lobo no chão e cruz na mão também) se anulam no meio, sem dano, com estouro desenhado no código (núcleo
  branco, anel abrindo e 8 raios, 0,33 s, aditivo), som de defesa e hitstop curto; o lápis da Thayna cai no chão.
  Recuperação do especial: enquanto o projétil não acertou nem foi defendido, o recovery é `whiff_recovery`
  (padrão: recovery + 18 ticks); acertou/defendido/chocou → volta ao `recovery` normal (ou termina na hora). De perto
  nada muda; errar de longe deixa parado mais tempo (punível). Projétil aparado conta como erro. Ao acertar: dano/defesa/hitstop como um golpe e a anim `impact` toca uma vez
  no ponto do acerto. Some ao sair da tela. Se a lutadora apanhar antes de soltar, o projétil não sai.
- Parry (quem tem `parry` no JSON): ↓, depois ← (para trás; ↙ junto vale), depois soco. JSON: `startup`, `active`
  (janela de aparar), `recovery` (depois de aparar), `whiff_recovery` (errou: punível), `cooldown` (ticks sem poder
  repetir depois de errar), `hitstop`, `hitstun_bonus`, `reflect_speed`. Anim `parry`: antes do `active_frame` =
  preparo, `active_frame` = aparando, o seguinte = sucesso, o último = recovery do erro. Caixa (ciano no F1): do
  centro do tronco até a frente do desenho, 30%–90% da altura, só nos ticks `active`; é checada antes dos acertos
  (se o golpe toca a caixa e o corpo no mesmo tick, o parry vence). Golpe aparado: o atacante leva o próprio dano,
  atordoado + `hitstun_bonus`, empurrado para trás. Projétil aparado: volta `reflect_speed`x mais rápido e passa a
  ser de quem aparou. Nos dois: `hitstop` longo, brilho `parry_flash` (aditivo) na caixa e recovery curto.
  Valores atuais pensando em criança jogando: janela de 8 ticks (133 ms), erro = 34 ticks preso + 30 de espera.
  A CPU apara às vezes (25% dos golpes, 35% dos projéteis), nunca sempre.
- Melhor de 3 rounds (padrão; muda em OPÇÕES): quem vence 2 leva a luta (bolinhas embaixo do nome = rounds vencidos). Cada round começa
  com "ROUND N" (ou "ROUND FINAL" no 1 a 1) por 1 s com todo mundo parado, depois "LUTE!". K.O. duplo não conta
  para ninguém. Vida chega a 0 → tela de fim de luta (abaixo); 2,5 s depois começa o próximo round com vida cheia.
  Quando alguém fecha os rounds, o menu: REVANCHE (luta nova, 0 a 0) / TROCAR LUTADORES (volta para a tela de
  escolha) / MENU INICIAL / SAIR DO JOGO (W/S ou ↑/↓ escolhem; J, Numpad 1 ou Enter confirmam). R reinicia a luta inteira. Depois do K.O. ninguém comanda nem apanha, mas as
  animações seguem: o perdedor toca `ko` uma vez e fica no último quadro (deitado); o vencedor termina o que fazia
  e volta ao idle. Na anim `ko`: `"scale_by": "width"` + `ref_frame` do quadro deitado (comprimento do corpo
  deitado = altura em pé) e `"mirror": true` quando a arte cai para o lado errado (deve cair longe de quem bateu).

## Arena (assets/data/stages.json; F5 recarrega)
- Arena = `stages[i]` escolhida na tela de arena (começa na 1ª): `image` (assets/bg/mapa1.png, 2048x2048, guardada no tamanho original: as coordenadas do
  JSON são px dela), `crop_top`/`crop_bottom` (faixa que vira a tela, reduzida com POINT; a largura é a de 16:9, centrada: faixa menor =
  mais zoom. Hoje 553..1600, ~10% de zoom, com os lutadores 20% maiores que antes para ficarem na proporção da sala), `floor_y` (pés dos
  lutadores na imagem → GROUND_Y; F1 mostra a linha amarela), `parallax` (0,3), `brightness` (0..1, escurece o fundo para os lutadores se destacarem; 0,7), `music` e
  `tv` {x, y, w, h}.
- Luz da sala (luz de dentro de casa, sem poluição): `light` {sun left = vem da janela, sombra curta 0,25 e 0,45 de
  opacidade, shade 0,5, warm 0,35, rim 0,25}; `vignette` 0,35 (teto e laterais escurecem em degradê, o olho vai
  para o meio); `tv_glow` 0,6 (halo e feixe da TV mais fracos).
- Mesa da esquerda = plataforma (`table`: `right` = x da lateral direita, `top_y` = y onde os pés pisam no tampo, px
  da imagem; F1 mostra em laranja). Andando, os pés param na lateral (o corpo fica na frente da mesa). Pulando
  para a esquerda, quem passa da altura do tampo cai em cima e fica lá; anda, pula e luta em cima; andando para
  fora (ou empurrado), cai pulando até o chão; K.O. em cima fica deitado em cima. Encostado na lateral conta como
  parede: o empurrão do outro não o põe embaixo do tampo (um K.O. ali o deixava deitado em cima da mesa). Pulo de cima continua a parábola
  até o piso embaixo. Segue o parallax do fundo.
- O fundo fica 8 px maior que a tela de cada lado (e na proporção em cima/embaixo): anda `parallax` x o
  deslocamento do meio dos lutadores (a "câmera"), suavizado até 8 px, sem nunca mostrar a borda da imagem.
- TV: sem `tv` no JSON, o jogo acha o maior retângulo quase preto da imagem e grava. Na tela dela, a luta ao vivo
  (fundo + lutadores, sem HUD, numa textura de 192x108 atualizada a cada 2 quadros) com tom azulado, scanlines e
  cantos escuros. Brilho aditivo ~15% (halo em volta + feixe em trapézio no chão) na cor média da imagem da TV,
  tremulando. Golpe de 9+ de dano: flash branco de 2 quadros e o halo pulsa. K.O.: estática 0,5 s e "K.O."
  piscando (o K.O. grande e a faixa do vencedor ficam por cima). Pausa: "▌▌" no canto (o painel da pausa cobre).
  Lutador perto da TV pega um toque da cor dela (multiplicado). `"tv_live": false` no assets/data/balance.json =
  sem transmissão, só o brilho (azulado fixo).

## Mercadão (arena em camadas com câmera; stages.json "layers")
- Arte do usuário (imagens/pixel) convertida para assets/raw/stages/mercadao por um script único: no cenario_fundo.png
  o céu pintado virou magenta (o jogo desenha o céu e as nuvens; fios, poste e prédios ficam); chao.png só com a
  calçada, opaca (com metade da borda sem magenta, o RemoveBackground caía no modo "fundo preto").
- Mundo mais largo que a tela: 0..SCREEN_W + 2 x margin, margin = (largura do fundo − tela) / 2 / parallax do fundo
  (fundo 1,7x a tela, parallax 0,5 → 269 px de cada lado). `arena` (fighter.h): as paredes são as bordas da tela;
  a câmera fica no meio entre as bordas dos dois CORPOS (pelos pés escorregava), presa ao mundo; quem recua para na
  borda se o outro está na outra (fight_test 24). Sala: mundo = tela, câmera 0 (nada mudou). Round começa no meio.
- Proporção (medida na fachada): a picape (~1,85 m) tem 55 px da imagem → ~14 px por metro na tela na linha do muro.
  Figurantes do fundo nessa escala (pessoa ~23 px; o `pixel_scale`/`height` deles muda junto com `width_scale`);
  lutadores (~65 px/m) estão bem mais perto. O fundo foi de 1,4x para 1,7x e o muro subiu (topo em 136, base em 178)
  para o muro não ficar na cintura dos lutadores e sobrar chão entre os dois planos. `front` = cenario_cerca.png, só a
  grade da cerca, desenhada por cima da cliente e da torcida (que ficam atrás dela, pés no asfalto da calçada).
  Pombos no chão da luta (parallax 1, na escala dos lutadores, por cima do chão).
- Camadas, posições em px da imagem do fundo: céu (gradiente `sky.top/bottom` + 3 nuvens de nuvens.png a 0,05 px/tick,
  presas à tela), fundo (`width_scale` 1,7; muro `wall_y` 538 da imagem em `wall_screen_y` 136; parallax 0,5; letreiro
  `title` na faixa vermelha `title_at`, UI 2x branco com sombra: fica atrás do HUD, aparece quando a câmera desce no
  pulo), animações (parallax 0,6): cliente com carrinho a cada 6–12 s descendo o caminho `path` da rampa a 0,4 px/tick
  e saindo pela esquerda atrás do muro (`files` = variações), vendedor fixo, torcida (3 fps; 10 fps por 60 ticks em
  golpe de 9+, especial/furacão e K.O.), pombos (quadros 1-2 ciscando; lutador a menos de 60 px: 3-4 voando para
  cima/direita; voltam em 15 s); chão em tile preso ao mundo (`scale` 0,25, topo em `top_y` 178), pés em `floor_y` 196.
- Sem TV (sem transmissão). Prévia da escolha de arena = fundo sobre a cor do céu + chão; close = a torcida.
- F1: linha do chão, limites da arena (vermelho) e o caminho da rampa (magenta). F5 recarrega.

## Celular (Android e iPhone: build web instalável, PWA)
- Mesmo código compilado com Emscripten (F:\emsdk) para `build-web/`: index.html (página `web/shell.html`), .js, .wasm
  e index.data (a pasta assets inteira, ~130 MB, baixada uma vez) + manifest.webmanifest, sw.js (offline: rede
  primeiro, cópia guardada sem rede; só em https) e ícones (web/). WebGL 2. App nativo de iPhone exige Mac + Xcode.
- Canvas na resolução real do celular (tamanho css x devicePixelRatio, conferido a cada quadro): zoom inteiro maior e
  nítido; sem RESIZABLE na web. O 1º toque no Android põe em tela cheia e deitado; instalado ("Adicionar à tela de
  início") abre sem barras. Opções de janela/tela cheia não fazem nada na web; settings.json não fica gravado.
- Fonte: o atlas é montado na CPU (ler a textura da placa de volta não funciona no WebGL e saía lixo).
- Controle de toque (só com tela de toque): controle virtual do P1 desenhado por cima de tudo, em px da tela inteira /
  zoom (nos celulares mais largos que 16:9 fica nas faixas pretas). Entra como o controle 0 (`ReadPads`), então todos
  os menus funcionam sem código próprio. Direcional à esquerda (8 direções, arrastando); à direita SOCO/CHUTE (nos
  menus OK/VOLTA), SUP (suporte), II (pausa, canto de cima) e um botão vermelho por golpe especial do lutador, que
  manda o comando pronto (`Input.cmd`: ESP = ↓→ soco, ESP2 = ↓→ chute, SUPER = ←↓→ chute, DASH = frente-frente,
  PARRY = ↓← soco). Os toques ativos ficam na página (`Module.touchXY`): o raylib web perde quem soltou com 2 dedos.

## Desempenho (medido: luta parada ~7–8% de 1 núcleo, como o menu; ~100 MB de RAM; 60 quadros/s)
- Sprites: UMA textura por quadro. Flash de dano, sombra e penumbra/luz quente usam um shader de silhueta (pinta os
  pixels visíveis na cor do vértice); a borda de luz de 1 px é outro shader (pixel com vizinho vazio do lado do sol
  ou em cima). Antes eram 4 texturas por quadro (cor, silhueta, borda direita, borda esquerda).
- NUNCA mudar parâmetro (uniform) de shader entre um sprite e outro: no driver da NVIDIA isso sincroniza com a placa
  e a luta foi de 7% para 50–78% de CPU. A borda de luz pega o tamanho do texel com textureSize e o lado do sol
  pelo verde da cor do vértice (232 = direita da textura, 231 = esquerda). Na web (GLSL ES 1.0) fica sem a borda.
- Quadros por segundo (OPÇÕES → QUADROS POR SEGUNDO, `max_fps` no settings.json): ECONÔMICO (padrão) = 1 quadro a
  cada N atualizações do monitor, ~60/s (165 Hz → 55, 144 → 72, 120/60 → 60), ritmo sempre regular e a interpolação
  deixa o movimento liso; MÁXIMO = 1 por atualização. A taxa do monitor é conferida a cada 2 s (consultar o monitor
  a cada quadro é lento no Windows).
- Lote de desenho com 3 buffers em rodízio (`rlLoadRenderBatch(3, 4096)` em main): as trocas de shader esvaziam o
  lote, e com o buffer único padrão do raylib o driver travava a CPU esperando a placa de vídeo (luta ia a 35%).
- TV: transmissão a 20 quadros/s, desenhada sem luz nem sombra (na tela pequena não aparecem); a cor do brilho vem da
  imagem da sala, calculada uma vez ao carregar (nada de ler textura da placa de vídeo durante a luta).
- Luz com `shade`/`warm`/`rim` 0 e `shadow_alpha` 0 pula os passes inteiros. A lógica (60 ticks/s) é barata; o custo
  é desenho. Ao medir: tirar prints em janela (em tela cheia o PrintWindow sai preto).

## Fluidez do movimento (medido quadro a quadro, ver fight_test 20)
- Posição: lógica a 60 ticks/s, desenho interpolado e encaixado na grade de pixels da tela. Andando a 62 px/s a 3x:
  3 px de tela por quadro e um 4 a cada ~10 (= 3,1 exatos); tremor da interpolação 0,05 px (invisível).
- Tempo do quadro encaixado no período do monitor (`framePeriod`): o relógio oscila ±0,6 ms; perto do período (até
  2 ms) usa o exato. A 60 Hz: sempre 1 tick por quadro (antes saíam quadros com 0 e 2 ticks).
- Ritmo das animações em ticks INTEIROS: `SetAnimFps` arredonda para 60/N desenhos por segundo (`hold` = N ticks por
  desenho); `AnimStep`/`AnimTicks` convertem ticks ↔ desenhos. Antes, 8 fps alternava 7 e 8 ticks e o andar calculado
  (6,89 fps) alternava 8 e 9: o olho lê como tropeço. O `fps` do JSON vira o mais perto possível (8 → 7,5; 14 → 15).
- Inercialização na troca de animação (só desenho): cada anim prende o desenho num ponto (idle nos pés, andar no
  tronco) e trocar fazia o corpo saltar 2–3 px de lado num quadro (Bartinho 3,3, Thayna 2,6). O salto do tronco,
  medido com as duas poses no mesmo x, vira um deslocamento que decai como d0·e^(−t/τ), τ = 45 ms (~70% por quadro);
  salto maior que 12 px (virou, atravessou, round novo) não suaviza. Caixas de golpe sempre exatas.

## Tela de loading (abre o jogo)
- Carregamento em fila de tarefas, sem threads (`QueueRoster` em fighter.cpp): mascotes primeiro, depois cada
  animação e imagem dos lutadores, por fim as cenas do menu. Cada quadro roda tarefas por até ~12 ms e desenha.
  Erro numa tarefa: fila descartada (o que carregou é liberado) e a mensagem aparece. F5 continua recarregando direto.
- Visual (cores em `src/theme.h`; telas novas usam essa paleta): logo no topo flutuando 1 px; painel arredondado com
  uma frase de `assets/data/loading_tips.json` (troca a cada 2,5 s com fade) e o ícone de um mascote; barra de
  energia em segmentos (vermelho → dourado, brilho passando) + "CARREGANDO... N%"; os 3 mascotes correndo no chão
  com poeira; casas da vila em 2 camadas de parallax (procedural; `assets/ui/loading_bg.png`, se existir, vira a
  camada de trás); vinheta e scanlines (`"scanlines": false` no loading_tips.json desliga). Tudo em px inteiros.
- Entrada: sai do preto com uma faixa diagonal na cor do fundo. Saída: mascotes correm para fora pela direita, a
  cena vai para a cor do fundo e a cortina (fundo + borda dourada) revela o menu.

## Menu inicial (estilo da loja do Hi-Fi Rush)
- Fundo = cenário da luta escurecido; logo "DANIN FIGHTERS" (`assets/raw/menu/logo.png`, fundo magenta, POINT) no
  canto de cima, por cima de tudo (nada passa por cima dele): flutua ±2 px num ciclo de 2 s e a cada 3 s uma faixa
  de brilho diagonal branca atravessa as letras (silhueta do logo = máscara). Sem o arquivo: título em texto numa
  faixa vermelha.
- 4 cartões inclinados: CONTRA CPU (liga a CPU; começa escolhido), 2 JOGADORES, OPÇÕES, SAIR (o último, à direita). Caixa branca embaixo descreve o
  cartão escolhido. Cada cartão mostra a sua cena `assets/raw/menu/card_<2p|cpu|sair|opcoes>_1..4.png` (imagem
  inteira, sem chroma key; o quadrado do meio, cortado exatamente pelas bordas inclinadas), em loop. Faltou um
  quadro: fica só o 1º, parado. Sem nenhum: degradê de cor.
- Escolhido: brilho normal, cresce (`zoom`), sobe, sombra, parallax suave da cena, borda branca pulsando e
  partículas (CPU: pixels vermelhos piscando; 2 jogadores: faíscas azuis; sair: poeira dourada; opções: faíscas
  amarelas). Os outros: escurecidos (`dim`) e animação mais lenta. Anima em ~0,2 s.
- `assets/data/menu.json` (F5 recarrega): `fps`, `fps_unselected`, `zoom`, `dim`, `parallax` (px) e `particles`
  (`life` em s; por cartão `rate` por segundo e `color`).
- A/D ou ←/→ escolhem; J, Numpad 1 ou Enter confirmam. Esc no menu inicial vai para SAIR (não fecha direto).
- Sem dicas de teclas no menu inicial. Crédito escondido no canto de baixo à esquerda: "Developed by Lui Loureiro", 22% de opacidade, menor que a fonte
  UI 1x (cada pixel da fonte = zoom − 1 pixels do monitor: 2/3 do tamanho no zoom 3x, ainda nítido).

## Fim de luta (pós-K.O.)
- Tempo 1 (0–1,2 s, todo K.O.): luta congela 6 ticks, flash branco de 2 quadros, tremida forte; arena com azul-noite
  55% e listras diagonais lentas; "K.O." grande (degradê amarelo→laranja→vermelho, contorno escuro 2 px, sombra
  vermelho-escura 4 px, brilho branco de 1 px no topo) cai letra por letra (K, O, pontos; 0,08 s entre elas;
  escala 2 → 0,9 → 1) com uma tremida curta a cada letra. `assets/ui/ko.png` (fundo magenta), se existir, substitui as letras: cai inteiro
  (180x110 px) e no topo usa uma cópia pequena própria (nítida); carregado junto com o logo (F5 recarrega).
- Tempo 2 (1,2–2,5 s, só quando a luta acaba): o K.O. encolhe para o topo (abaixo do ∞); faixa a 12° entra pela
  esquerda na cor da barra do vencedor (P1 amarelo, P2 ciano), com borda escura e brilho passando: ícone do HUD
  grande, nome (24 px se couber antes do painel; senão 16, e nome comprido em duas linhas), "VENCEU!" em dourado
  com um pulsar curto e os rounds ganhos em losangos acendendo um a um. O vencedor volta à cor normal; o perdedor
  fica escurecido.
- Tempo 3 (2,5 s em diante): painel inclinado (12°, ~42% da largura) desliza da direita: borda escura + dourada
  fina, cabeçalho "FIM DE LUTA" em faixa vermelha, itens em cartões inclinados com ícone pixel 8x8 (o escolhido
  em dourado, 6 px para a direita, seta piscando e brilho), teclas W/S/J desenhadas no rodapé. Aceita comandos
  quando o painel para (2,8 s). Confirmar = cortina diagonal.
- Texto desta tela pela tipografia (abaixo): K.O. (sem ko.png), nome do vencedor e "VENCEU!" em Display, no
  mesmo estilo do "VS"; itens e teclas em UI.

## Tipografia (src/typography.h)
- Todo texto do jogo em fonte pixel (nada de fonte lisa), escalas inteiras (1x = 8 px): **Display** = Silkscreen
  Bold (títulos, VS, K.O., nomes grandes, ROUND/LUTE!) com degradê vertical, contorno escuro (2 px; 1 px na
  escala 1), sombra sólida de 3-4 px e brilho de 1 px no topo; **UI** = Silkscreen Regular (rótulos, opções, dicas),
  contorno de 1 px opcional. `DrawDisplayText(texto, pos, escala, degradê, sombra)` (degradês em `Grad::`: Fire,
  White, Gold, P1, P2; cada texto é montado uma vez e guardado) e `DrawUIText(...)`. `Text()`/`TextW()` antigos
  viram UI (tamanho 10 = 1x, 20 = 2x...). Arquivos em `assets/fonts/` (OFL em `OFL-Silkscreen.txt`); sem eles,
  fonte padrão da raylib. O raylib escala pela altura ascendente+descendente: ao carregar, a cópia em memória tem
  a ascendente ajustada para a altura virar 1 em (8 px na grade).
- Imagens de tela (LoadPicture) ganham 1 px transparente em volta: cortadas em forma inclinada além da borda, o
  CLAMP não estica a última linha.

## Música
- `assets/audio/musica_menu.mp3` em loop no menu inicial, nas telas de escolha, no VS e nas opções abertas do menu
  (não nas abertas da pausa). Entra e sai em 0,5 s (some no VS, antes da luta) e recomeça do início ao voltar.
  Volume = MÚSICA das opções. Sem o arquivo: sem música.
- Na luta (e nas opções abertas da pausa): a `music` da arena (assets/audio/musica_mapa1.mp3), do mesmo jeito.

## Efeitos sonoros (assets/sfx)
- Todo .wav/.ogg/.mp3 de `assets/sfx` carrega ao abrir o jogo, pelo nome sem extensão; volume = EFEITOS. Nome que
  não existe = sem som. A lógica da luta põe os nomes numa fila (`sfxQueue`) e o jogo toca a cada quadro.
- Acerto: `sfx` do golpe no characters.json (punch, kick, air_attack, crouch_*, special = acerto do projétil;
  pet `attack`); sem `sfx`: soco_fraco (dano < 9) ou soco_forte. Defendido: defesa. Começo de soco/chute/golpe no
  ar: whoosh. Projétil saindo: `launch_sfx` do special (lancamento_cruz). Parry aparou: tilintar. K.O.: ko; corpo
  no chão (último quadro da anim `ko`): queda. Passos: `steps` do personagem + _1.._3 (sorteado), no quadro 0 e
  na metade do andar (passo_leve na Terezinha, passo_pesado no Bartinho).
- Voz do personagem: `voice` em cada golpe (toca ao começar o golpe/especial) e `"voice": {hurt, ko, win}` no
  personagem (apanhou sem defender, K.O., venceu a luta junto da faixa VENCEU!), com `volume` (padrão 0,25 dos EFEITOS:
  vozes bem baixas, só um toque). Terezinha: golpe_fraco no soco,
  soco agachado e golpe no ar; golpe_forte no "chute" e na rasteira; especial; dano; ko; vitoria
  (terezinha_demo.wav é a amostra, sem uso). Bartinho: golpe_forte na bolsada (soco) e na rasteira; golpe_fraco na
  bolsada agachada e no golpe no ar; joelho no chute (acerto = joelho.wav); parry (`voice` no "parry", ao aparar); dano; ko; vitoria
  (bartinho_demo.wav sem uso).
- Mascotes: `sfx` no supports.json (latido na Pérola, junto da onda; miado na Milka e na Belle, ao chegar);
  a cura também toca tilintar. O mesmo som na tela de escolha do suporte.
- Menus: cursor ao mover, confirmar ao confirmar; pausa_vhs ao pausar; rewind na REVANCHE e no R.
- joelho.wav ainda sem lugar.

## Navegação, pausa, opções e transições
- Esc (ou Backspace) volta: escolha → menu inicial (K sem nada escolhido também); luta → PAUSA (Continuar /
  Opções / Trocar lutadores / Menu inicial; Esc de novo continua); opções → de onde veio (menu inicial ou pausa).
  O Esc não fecha o jogo (SetExitKey desligado): sair é pelo cartão SAIR ou pelo menu do fim da luta.
- OPÇÕES: tamanho da janela (zoom inteiro 1x..o que cabe no monitor), tela cheia (janela sem borda), quadros por
  segundo (ECONÔMICO ~60 / MÁXIMO, ver Desempenho), música e
  efeitos (0–100%; música = volume das músicas, efeitos = volume dos sons), rounds para vencer (1/2/3 = melhor de 1/3/5), dificuldade
  da CPU (FÁCIL / MÉDIO / DIFÍCIL / AVANÇADO, padrão MÉDIO, `cpu_level` 0..3), mostrar caixas (o mesmo que F1). W/S linha, A/D muda, J liga/desliga, Esc volta. Grava em `settings.json` (raiz do
  projeto) a cada mudança; lido ao abrir o jogo, antes de criar a janela.
- Transição entre telas: cortina preta inclinada com borda vermelha atravessa da esquerda para a direita (0,3 s
  cobrindo, a tela troca por baixo, 0,3 s descobrindo). Durante ela ninguém comanda e a luta não anda.

## Tela de escolha
- Fundo: raios diagonais laranja/vermelho andando devagar, vinheta e scanlines. Topo: faixa vermelha central
  "ESCOLHA SEU LUTADOR" (Display) e "1P" (vermelho) / "CPU" ou "2P" (azul) nos cantos.
- Lados (~30%): painel inclinado de borda dupla (escura + cor do jogador) com o busto do personagem sob o cursor
  (retrato na metade do tamanho do VS, cabeças iguais entre si, para o busto quase inteiro caber acima da placa;
  encostado na placa, sem passar do topo do painel)
  (`assets/ui/portraits/<id>_big.png` se existir, senão o "portrait" do JSON, senão o idle ampliado 2x; P2
  espelhado) e a placa: nome (Display), `style`, FORÇA/VELOCIDADE/ALCANCE (`stats` 0..5, 5 segmentos) e
  `special_name` com o comando desenhado (↓→ especial, ↓← parry, + tecla de soco). Confirmar: o busto pisca em
  branco e aparece "PRONTO!". Skin: não há (indicador só quando existir mais de uma).
- Centro: grade 4xN de cartões a 8° (28 px): personagens do JSON na ordem, vagas sem personagem = silhueta com
  cadeado ("EM BREVE" com o cursor em cima, não confirma), último = "?" (sorteia). Cartão: `<id>_icon.png` ou o
  quadrado do topo do "icon" (sem máscara redonda) ou o rosto do sprite. 8 cartões até 7 personagens; mais que
  isso, ganha linhas. Cursores: moldura pulsando vermelha (1P) / azul (2P/CPU) com etiqueta; no mesmo cartão a
  moldura fica dividida nas duas cores.
- Rodapé: as teclas desenhadas (WASD MOVER, J CONFIRMA, K VOLTA). A arena tem tela própria (abaixo).
- Entrada: painéis deslizam das bordas e os cartões surgem em cascata.
- P1: A/D/W/S move, J confirma, K desfaz. P2: setas, Numpad 1 confirma, Numpad 2 desfaz. Pode repetir personagem.
  CPU ligada: o P1 escolhe o próprio lutador e, em seguida, o da CPU com as mesmas teclas/controle (a faixa do topo
  vira "ESCOLHA O RIVAL"; o cursor CPU anda). K desfaz o da CPU e, sem ele, o próprio.
- Os dois com lutador confirmado: a cortina passa e entra a tela ESCOLHA SEU SUPORTE (abaixo).

## Tela de suporte
- Mesmo padrão da escolha de lutador: faixa "ESCOLHA SEU SUPORTE" + 1P/CPU nos cantos; fundo roxo com raios,
  vinheta, scanlines e chão de lajotas embaixo; teclas desenhadas no rodapé (A/D MOVER, J CONFIRMA, K VOLTA).
- Lados (~31%): palco com raios na cor do jogador e almofada redonda; o pet na pose parada (a arte grande do
  supports.json, no tamanho da corrida em 2x) respirando 1 px (1,2 s). Ao trocar o cursor, o pet atual sai correndo
  pela borda de fora e o novo entra correndo (anim `run` em 2x), desacelera e para (~0,35 s, poeira nos pés).
- Ficha embaixo: nome (Display), tipo (ATAQUE vermelho / CONTROLE azul / CURA verde, do `type`), RECARGA em
  segmentos (quanto menor o `cooldown`, mais cheia; escala até 10 s) com os segundos, o efeito em até 2 linhas
  (`short` no supports.json; padrão = `description`) e a tecla de chamar do jogador (1 no P1, 7 do Numpad no P2).
- Centro: um cartão por pet na horizontal (34 px, 8°) com o rosto (quadrado do topo do `icon`, sem máscara; sem
  ícone, o canto da frente da arte grande), nome embaixo; cursores como na escolha de lutador. Os dois podem
  escolher o mesmo.
- Confirmar: pulinho no palco, o som do pet (`sfx` no supports.json), o painel
  pisca e "PRONTO!". Desfazer tira a escolha e, sem escolha, volta aos lutadores (Esc também). CPU: o P1 escolhe o suporte
  dela depois do seu ("SUPORTE DO RIVAL" no topo), do mesmo jeito.
  Os dois prontos: 1 s e a cortina leva à tela ESCOLHA A ARENA (abaixo).

## Tela de arena (depois do suporte; sem mascotes, depois dos lutadores)
- Seleção no estilo da escolha de estágio do DOA, só a disposição (lista + prévia grande), no visual 8 bits do jogo:
  faixa "ESCOLHA A ARENA" + 1P/CPU nos cantos, fundo azul-petróleo com raios, vinheta e as teclas no rodapé
  (W/S MOVER, J CONFIRMA, K VOLTA).
- Esquerda: faixas inclinadas em cascata (entram pela esquerda): cada arena do stages.json (tira da imagem + nome
  `name`), vagas "EM BREVE" com cadeado até 5 linhas (não confirmam) e ALEATÓRIO por último ("???"). A escolhida
  anda para a direita, cresce, ganha borda dourada e a etiqueta 1P; as outras ficam escurecidas.
- Direita: prévia (entra pela direita) com borda escura + dourada e um jogo de câmeras em loop: CAM 1 geral fechando
  devagar, CAM 2 close na TV (com chuvisco), CAM 3 panorâmica no chão; 2,4 s cada, corte com flash branco (também
  ao trocar de arena). Scanlines, cantos de visor, "CAM N" e REC piscando; nome em Display numa faixa embaixo.
  Prévia = imagem inteira reduzida para 768 px com POINT, carregada no loading (F5 recarrega). EM BREVE/ALEATÓRIO:
  chuvisco com cadeado / "?".
- Só o P1 escolhe (W/S, ↑/↓ ou direcional; J/Enter/A confirma; K/Esc/B volta aos suportes). Confirmar: a faixa
  pisca, carrega a arena escolhida (`stages[i]`; ALEATÓRIO sorteia) e a cortina leva ao VS, que mostra o nome dela.

## Tela VS (2 s, antes da luta)
- Tela dividida na diagonal (1P vermelho à esquerda, 2P azul à direita) com raios andando; bustos entrando pelas
  bordas com leve zoom, presos à sua metade e encostados embaixo; nomes em Display na base (grande se couber,
  senão menor/duas linhas) com 1P/CPU em cima; "VS" (Display, degradê de fogo) bate com escala 2 → 0,9 → 1, flash
  branco e tremida; "SALA" embaixo em UI. Depois a cortina leva direto ao "ROUND 1" (tudo já foi carregado).

## Mascotes (suportes) — assets/data/supports.json
- Cada jogador escolhe UM suporte na tela de escolha, depois do lutador (os dois podem escolher o mesmo).
- Medidor por jogador, embaixo da barra de vida; zera a cada round. Ganha `gain_on_hit` por ponto de dano dado e
  `gain_on_damage_taken` por ponto levado (um pouco mais, para quem está apanhando reagir); máximo `max_meter`.
  Reação de quem está perdendo: abaixo de `comeback.below` (25%) da vida, ganha `comeback.gain` (1,5x) mais, dando e
  levando (supports.json).
  Marca branca no medidor = custo do suporte escolhido; a vaga do suporte ao lado do retrato acende (aro amarelo)
  quando dá para chamar; a tecla aparece no fim do medidor.
- Chamar: 1 ou L (P1), Numpad 7 ou Numpad 3 (P2). Cada mascote: `cost`, `cooldown` (ticks), `max_uses_per_round`.
  Não chama atordoado (apanhando), no K.O. nem com ele já em cena.
- Entram correndo pela borda atrás de quem chamou (`run_speed`), param em `stop_offset` px do alvo no sentido da
  corrida, fazem a ação (anim `action`) e voltam correndo por onde vieram. Não apanham. Sem arte: bolinha com inicial.
  - `bark` (Pérola): para à frente de quem chamou; no `active_frame` do latido solta a onda (`fx`, aditiva) e
    empurra o oponente para longe: velocidade = `push.max` × (1 − distância/`push.radius`), multiplicada por
    `push.friction` a cada tick. Quem está no alcance também leva `push.damage` (5; regras normais de defesa).
  - `attack` (Milka): corre até o oponente; arranhão nos `active_frames` quadros a partir do `active_frame`, caixa
    medida na arte (ou `hitbox` [x0,y0,x1,y1]); `attack.damage`/`attack.guard` com as regras normais de defesa;
    `fx` (aditivo) no ponto do acerto.
  - `heal` (Belle): para atrás de quem chamou e cura `heal.total` espalhado em `heal.ticks`, nunca além da vida
    máxima; `fx` (aditivo) em loop de pé sobre quem é curado.
- `height` (× `scale`) = altura do corpo mais alto da corrida; `icon` + `icon_focus`/`icon_size` = recorte da cabeça
  (bicho de lado). F1: arranhão em vermelho e alcance do latido em amarelo. O suporte da CPU é escolhido pelo P1 e ela
  chama de vez em quando, quando faz sentido.

## Controles
- P1: A/D andar · W pular · S agachar · J soco · K chute · especial: S, D, J · parry: S, A, J (olhando para a direita) · suporte 1 ou L
- P2: ←/→ andar · ↑ pular · ↓ agachar · Numpad 1 soco · Numpad 2 chute · especial: ↓ → Numpad 1 · parry: ↓ ← Numpad 1 · suporte Numpad 7 ou 3
- Controle de videogame (raylib, qualquer um que o Windows reconheça): o 1º conectado é o P1, o 2º é o P2. Direcional
  ou analógico esquerdo = setas/WASD (especial e parry com os mesmos comandos). Na luta: X/Quadrado = soco, Y/Triângulo
  = chute, RB/R1 = suporte, Start = pausa. Nos menus: A/Cruz confirma; B/Bola desfaz nas telas de escolha e volta no
  menu inicial, nas opções e na pausa; Start volta (como o Esc). Teclado continua valendo junto. As dicas na tela mostram só as teclas.
- F2: liga/desliga a CPU no P2 (IA bem simples: aproxima, ataca quando está no alcance, às vezes recua ou defende)
  Níveis (tabela `CPU_LEVELS` em fighter.cpp; MÉDIO = a IA original): tempo entre decisões (FÁCIL 24-45 ticks ...
  AVANÇADO 3-8), chance de atacar no alcance, recuar, defender (20/50/75/92%), aparar (0/25/35/50%), mascote.
  DIFÍCIL e AVANÇADO também soltam o especial de longe (↓ → soco), punem o recovery de golpe errado e, sem a arma
  (Thayna), vão buscar o lápis. Futuro: rede neural adaptativa entra como mais um nível no lugar da tabela.
- F1: mostra hitboxes, hurtboxes e caixa delimitadora dos sprites
- F5: recarrega o JSON e as imagens sem fechar o jogo
- R: reinicia a luta · Esc: volta / pausa

## HUD
Estilo Marvel vs Capcom Infinite, espelhado: no canto, ícone grande do lutador com o ícone menor do suporte
encaixado embaixo (o suporte escolhido; acende quando dá para chamar e dá um pulinho ao ficar pronto); os dois
círculos ficam por cima da ponta de fora da barra de vida e do medidor, cobrindo só ~3 px; nome em
Display pequeno em cima da barra de vida inclinada (ponta de dentro cortada em diagonal), degradê amarelo→verde no
P1 e azul→ciano no P2, brilho em cima; abaixo de 25% de vida a barra fica vermelha pulsando; flash branco de 2
quadros ao apanhar; rastro vermelho = dano recente, derrete em ~0,5 s (para no hitstop); medidor do suporte embaixo
da barra com a marca do custo e a tecla (1 / 7) na ponta; pisca branco ao chamar.
Barra de guarda (laranja → vermelha, pisca quase cheia) curta depois da tecla do suporte, só quando tem algo nela.
Avisos embaixo do medidor, do lado de cada um, por 1,25 s (sobem 2 px e somem): "REPETIDO -40%" (amarelo, em quem
bateu com o dano reduzido pelo golpe repetido) e "GUARDA QUEBRADA!" (vermelho, em quem teve a guarda quebrada).
No meio: placa hexagonal inclinada (fundo de painel, borda escura + dourada fina) com um ∞ pequeno (luta sem
tempo) e, embaixo dela, as medalhas de round 7x7 (P1 à esquerda, P2 à direita, uma por round a vencer): vazias
escuras, ganhas douradas com brilho, piscam ao acender.
"ROUND N"/"LUTE!" no começo do round; no fim da luta, a tela de fim de luta (acima). Nada mais.

## Forma de trabalhar
- Ao terminar cada tarefa: compilar, rodar, corrigir erros e listar o que testar manualmente.
- Mantenha o código curto e legível; prefira menos arquivos.
- Não adicione funcionalidades fora deste documento sem pedir.
- Informe sempre o comando exato de build e de execução.
