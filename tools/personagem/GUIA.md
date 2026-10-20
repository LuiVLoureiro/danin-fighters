# Guia de personagem novo — tudo o que um lutador precisa

Modelo para qualquer lutador novo, tirado do que já funciona no jogo (Terezinha, Bartinho, Natália, Thayna).
Gerador: `tools/personagem/gerar.py`, que lê a ficha `tools/personagem/<id>.json`. A ficha completa do Danilo
(`danilo.json`) é o exemplo pronto para copiar.

## Passo a passo
1. **Arte base** (`assets/raw/<id>/base.png`): o personagem inteiro, de pé, de lado em 3/4 olhando para a DIREITA,
   pixel art, fundo magenta #FF00FF. É a ÚNICA referência de todas as imagens. Se a base estiver errada, sai tudo
   errado, então ela vem primeiro e é aprovada antes do resto.
2. **Ficha** `tools/personagem/<id>.json`: copie `danilo.json` e troque:
   - `personagem` (descrição em inglês da roupa e do rosto);
   - `altura` (= `height` do characters.json);
   - os `quadros` de cada golpe;
   - o `json` de cada animação.
3. **Prévia grátis**: `python tools/personagem/gerar.py <id> --previa` mostra cada prompt.
4. **Gerar**: `python tools/personagem/gerar.py <id>`.
   - Gera, nesta ordem, o objeto (projétil), o ícone, o retrato e depois as folhas.
   - O que já existe não é refeito. Para refazer: `--refazer chute_sheet`. As tentativas anteriores ficam em
     `imagens/assets_raw_original/<id>/tentativas/`.
   - A saída diz se o fundo saiu magenta (abaixo de 90% = refazer).
5. **Colocar no jogo**: o arquivo `tools/personagem/<id>_anims.json` traz as entradas prontas. Elas entram no
   characters.json, junto dos dados dos golpes (tabela abaixo).
6. **Conferir no jogo**:
   - O console mostra `SPRITE: ... escala` de cada folha. Folhas do mesmo personagem devem ter escalas parecidas.
   - F1 mostra as caixas.
   - A cabeça tem que ter o mesmo tamanho em todas as poses. Se uma folha "cresce", use `pixel_scale` (escala
     medida) ou `frame_scale` (um quadro só), como na Thayna.

Motor padrão: **Qwen-Image-2.1 Turbo local** (sem internet, sem custo).
- Instalação em C:\IA (SSD; no HD USB F: fica ~10x mais lento):
  - `sdcpp\` = stable-diffusion.cpp com CUDA (`sd-cli.exe`);
  - `modelos\`:
    - `qwen-image-2.1-turbo-Q4_K_S.gguf`
    - `Qwen3VL-8B-Instruct-Q4_K_M.gguf`
    - `mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf` (visão: lê a base)
    - `qwen_image_2.1_vae_bf16.safetensors`
- 8 passos, `--offload-to-cpu` (cabe na RTX 3050 Ti de 4 GB).
- Tamanho padrão 512 px, quadrado (`QWEN_RES`), com a base enviada em 320 px (`QWEN_REF`).
  - Cabe nos 4 GB mesmo com o VS Code aberto. Chrome e Discord fechados ajudam.
  - Boneco com ~150 px na imagem, que sobra para os 108 px do jogo.
- Cada folha gera 3 candidatos numa chamada (`QWEN_CANDIDATOS`), ~2,5 min no total. O gerador escolhe sozinho:
  - **conta os bonecos**: candidato com número de figuras diferente do pedido é descartado (o modelo às vezes
    desenha 2 por célula);
  - **mede a variação**: quanto a silhueta muda de um quadro para o seguinte (média). Abaixo de 0,20 avisa "QUADROS
    IGUAIS". Referências: andares do jogo 0,28-0,35, especial da Terezinha 0,37, chute da Natália 0,59. Não percebe
    a linha de baixo repetindo a de cima (perna trocada tem a mesma silhueta): isso só olhando;
  - os candidatos não escolhidos ficam em `tentativas/` (`_c2`, `_c3`).
- `"varia": false` na ficha = animação que tem quadros parecidos de propósito (idle, defesa, agachar, tonto, K.O.):
  gera 1 candidato só e não leva a frase de variação.
- Licença do modelo: Qwen Research (uso não comercial).

Alternativa: `--motor gemini` (Nano Banana 2, `gemini-3.1-flash-image`). Precisa da chave em `GEMINI_API_KEY`,
de `pip install google-genai` e é pago.

Loop autônomo (uma imagem por iteração, aprova ou refaz sozinho): `tools/personagem/RALPH_DANILO.md`.

## Regras de toda imagem (já embutidas no gerador)
- Anexo 1 = a base. Anexos extras só quando a imagem usa um objeto já gerado (ex.: o tênis no arremesso, `"refs"`).
- Mesmo personagem, mesmas proporções (cabeça/corpo), mesmo tamanho de bloco de pixel e o mesmo contorno escuro
  fino da base.
- Nada inventado: sem objeto extra, texto, número, legenda, chão, sombra, linhas de movimento, efeitos ou cenário.
  Efeitos (redemoinho, brilho) ou o jogo desenha, ou viram uma imagem própria.
- Grade de células iguais, invisíveis, lida da esquerda para a direita e de cima para baixo:
  - um corpo inteiro por célula, centralizado, todos do mesmo tamanho (em pé = ~80% da altura da célula);
  - pés na mesma linha de base;
  - nada cortado e nada invadindo a célula vizinha.
- Sempre virado para a DIREITA (o jogo espelha). Fundo magenta chapado, sem degradê nem linhas de grade.
- **Prompt** (modelo aprovado na arrancada, montado pelo `gerar.py`):
  1. a base só dá o VISUAL, a pose muda em todo quadro;
  2. a descrição do personagem;
  3. a grade com "EXACTLY N figures, C per row";
  4. a vista lateral virada para a direita, com perna e braço "de perto" (esquerdos, com o relógio) e "de longe";
  5. a ação (`"acao"`), com "frame-by-frame motion study: no two frames share the same pose";
  6. `Frame k (posição): ...` para cada quadro, descrevendo exatamente qual perna e qual braço estão à frente;
  7. o fundo magenta e o estilo 16-bit.
- Ciclos de corrida e andar: a IA tende a repetir a linha de cima na de baixo. Ajuda dizer "TOP ROW = near leg
  leads; BOTTOM ROW = far leg leads" e desenhar a perna de longe mais escura.
- **6 quadros = 3 colunas x 2 linhas** (`"rows": 2, "cols": 3`). Com 2 colunas x 3 linhas, as células ficam largas
  e o modelo desenha 2 bonecos em cada uma (12 no total).
- **Grades que funcionam** (todas as folhas atuais): 2x2 = 4 quadros; 3 linhas x 2 colunas = 6 quadros;
  2 linhas x 3 colunas = 6 quadros. Imagem quadrada ou 3:2. Uma linha só (`frames`) também vale, mas a IA erra
  mais o espaçamento.
- Se o golpe solta algo (projétil), o quadro do lançamento mostra a mão VAZIA: o jogo desenha o objeto voando.

## Lista completa de imagens
Na coluna "Quadros", as ações entre ";" são os quadros em ordem.

**Obrigatórias** (sem elas o jogo usa fallback, mas fica feio):

| Anim no JSON | Arquivo | Grade | Quadros | Entrada típica do JSON |
|---|---|---|---|---|
| `icon` (campo) | icone.png | 1 | cabeça e ombros, 3/4 para a direita | `"icon": ".../icone.png"` |
| `portrait` (campo) | retrato.png | 1 | da cintura para cima, guarda alta | `"portrait": ...`, `"portrait_head": N` (px do topo do crânio ao queixo, medido) |
| `idle` | idle_1.png | 2x2 | guarda; desce 1-2 px; mais baixo; volta | `fps 6, anchor feet, ref_frame 0, sequence [1,2,3,4,3,2]` |
| `walk` | andar_sheet.png | 2 linhas x 3 | contato D; passagem; pés juntos; contato E; passagem; pés juntos | sem `fps` (o jogo calcula pelo passo) |
| `crouch` | agachar_sheet.png | 2x2 | em pé; meio; agachado; agachado | `fps 20, ref_frame 0` (fica no último) |
| `jump` | pular_sheet.png | 2 linhas x 3 | impulso; sobe; topo encolhido; desce; aterrissa; de pé | `ref_frame 5` (1º = impulso, 2 últimos = aterrissagem) |
| `block` | defesa_sheet.png | 2x2 | ergue a guarda; braços cruzados; impacto; segura | `fps 16, ref_frame 0` |
| `block_low` | defesa_agachado_sheet.png | 2x2 | o mesmo, agachado | `fps 16, scale_like crouch, scale_like_frame 4` |
| `hit` | dano_sheet.png | 2x2 | cabeça para trás; tropeça; recupera; quase guarda | `fps 12, ref_frame 3` |
| `ko` | ko_sheet.png | 3 linhas x 2 | golpe; cai para trás; bate no chão; quica; deitado; deitado | `fps 8, scale_by width, ref_frame 4` (+ `mirror` se cair para o lado errado) |
| `punch` | soco_sheet.png | 2x2 | guarda; começa; ESTICADO; volta | `active_frame 2, ref_frame 0` |
| `kick` | chute_sheet.png | 3 linhas x 2 | guarda; ergue joelho; ESTICADO; segura; desce; guarda | `active_frame 2, ref_frame 0` |
| `crouch_punch` | agachar_soco_sheet.png | 2x2 | agachado; começa; ESTICADO baixo; volta | `active_frame 2, scale_like crouch, scale_like_frame 4` |
| `crouch_kick` | rasteira_sheet.png | 3 linhas x 2 | agachado; mão no chão; RASTEIRA; segura; recolhe; agachado | idem; o golpe tem `"guard": "low"` |
| `air_attack` | golpe_ar_sheet.png | 2x2 | encolhido no ar; ESTICADO; segura; recolhe | `active_frame 1, scale_like jump` |

**Por personagem** (o que o kit pedir):

| Anim | Quando | Grade | Notas |
|---|---|---|---|
| `special` + `projectile` (+ `impact`) | especial ↓→soco | 2x2 ou 3x2 | quadro do lançamento = mão vazia; projétil = imagem própria (`height`, `spin` para girar por código, `from: ground` para sair do chão) |
| `special_aura`, `block_aura` | brilho atrás do corpo | 2x2 | fundo PRETO (brilho vira transparência), `height` próprio |
| `parry` + `parry_flash` | ↓←soco | 2x2 | preparo; APARANDO (`active_frame`); sucesso; erro |
| `dash` | frente, frente | 2x2 | corrida inclinada; `fps 15, scale_like walk` |
| `punch2`, `punch3` | corrente J J J | 2x2 | cada um com `active_frame` |
| `super` + `dizzy` | ←↓→ chute | 3x2 + 2x2 | efeito desenhado pelo jogo; `dizzy` = tonto depois |
| `<anim>_unarmed`, `dropped`, `pickup` | arma que cai | — | ver Thayna |

## Exemplos reais do banco (copie o padrão)
Escolhidos porque cada um mostra um recurso diferente:
- **Terezinha, soco** (`soco_sheet`, 1 linha, 4 quadros):
  - quadros: guarda; puxa; braço esticado; volta;
  - JSON: `"frames": 4, "fps": 12, "active_frame": 2`.
- **Terezinha, especial CRUZ DE LUZ** (`especial_sheet`, 3x2):
  - quadros: guarda; reza (cruz brilhando na mão); concentra; empurra as mãos (lançamento); mãos à frente; volta;
  - JSON: `active_frame 3, ref_frame 0`;
  - o projétil (`projetil_sheet` 2x2, `height 38`) e o impacto (`impacto_sheet`, `height 48`) são imagens separadas.
- **Bartinho, defesa** (`defesa_sheet` 2x2: guarda; braços cruzados; impacto; segura) e a mesma folha agachada
  (`defesa_agachado_sheet`, `scale_like crouch`).
- **Bartinho, bolsada**: arma pendurada. A caixa de acerto pega só a parte grossa da bolsa; a alça fina não acerta.
  - grade irregular: `"layout": [2, 3]`;
  - JSON: `active_frame 1, ref_frame 2`.
- **Bartinho, K.O.** (3x2): caiu para o lado errado na arte, então tem `"mirror": true`; deitado medido pelo
  comprimento: `"scale_by": "width", "ref_frame": 5`.
- **Natália, chute** (`chute_sheet` 3x2):
  - quadros: guarda; joelho; chute lateral alto (3º = `active_frame 2`); recolhe; desce; guarda;
  - JSON: `ref_frame 5`.
- **Natália, especial INVOCAR LOBO**:
  - o lobo sai do chão: `"from": "ground"` no golpe;
  - o lobo tem fundo preto e brilho translúcido;
  - auras de poder e de defesa atrás do corpo (`special_aura`, `block_aura`).
- **Thayna, folhas em escalas diferentes**:
  - `pixel_scale` medido por folha (0,25 na maioria, 0,318 no golpe agachado);
  - `frame_scale` por quadro quando a IA desenhou poses da mesma folha em tamanhos diferentes.
  - Arma que cai: `drops_weapon`, `_unarmed`, `dropped`, `pickup`.
- **Thayna, idle**: 3 folhas 2x2 numa animação (`"files": [...]`). Quadros 1-4 em ida e volta,
  `sequence [1,2,3,4,3,2]`; o resto são provocações fora da sequência.
- **Danilo** (`danilo.json`): o kit inteiro com dash, corrente J J J, tênis girando por código e furacão com tontura.

## Dados dos golpes (characters.json) — valores de referência
Ticks a 60/s. Em cada golpe, `startup` + `active` + `recovery` é o tempo total do golpe.

| Arquétipo | Vida | Andar | Soco (dano/startup/active/recovery) | Chute | Exemplo |
|---|---|---|---|---|---|
| Rápido, fraco | 90 | 88 | 4 / 3 / 2 / 8 | 6 / 5 / 3 / 12 | Danilo |
| Distância | 90 | 60 | 6 / 5 / 3 / 10 | 9 / 8 / 4 / 16 | Terezinha |
| Equilibrado | 100 | 62-66 | 7 / 5-6 / 3 / 11-13 | 10 / 8-9 / 4 / 17-18 | Natália, Thayna |
| Pesado | 130 | 66 | 12 / 12 / 4 / 22 | 13 / 10 / 4 / 20 | Bartinho |

- Pulo (todos): `height 86-90, rise 20-23, fall 13-16, speed 140-175, double 0.6`. Assim o pulo normal passa
  por cima de quem está agachado e o pulo duplo passa por cima de quem está de pé.
- `stats` (0..5: força, velocidade, alcance), `style`, `special_name`: a placa da tela de escolha.
- `steps` (passo_leve / passo_pesado) e `voice` {hurt, ko, win}: sons.
