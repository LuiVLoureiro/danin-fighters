# Loop: gerar o Danilo no Qwen-Image-2.1 local (uma imagem por iteração)

Contexto: CLAUDE.md e tools/personagem/GUIA.md. Única referência do personagem: assets/raw/danilo/base.png.
Ficha com todas as imagens: tools/personagem/danilo.json. Registro: tools/personagem/danilo_log.md (crie se não existir).

Em CADA iteração faça só isto:

1. Leia o danilo_log.md e rode `python tools/personagem/gerar.py danilo --uma` em segundo plano. A geração
   leva minutos; espere a notificação de fim, sem ficar consultando.
   - Se imprimir "NADA FALTANDO", vá para o passo 4.
   - Antes de gerar, rode `nvidia-smi --query-gpu=memory.free --format=csv,noheader`. Abaixo de 2600 MiB livres:
     - espere até liberar, com um Monitor de até 30 min;
     - não feche programas do usuário;
     - se continuar ocupado, anote no log e termine a iteração.
   - Se imprimir "SEM VRAM": mesma coisa.
   - Se for outro erro do sd-cli: anote no log, tente uma vez com `QWEN_RES=640`. Se repetir, pare e explique.
2. Olhe a imagem gerada (imagens/assets_raw_original/danilo/<arquivo>) ao lado do base.png. Também olhe os
   candidatos `_c2`/`_c3` em tentativas/: se um estiver melhor, copie-o por cima. REPROVE se o gerador avisou
   "QUADROS IGUAIS" ou "NÚMERO DE QUADROS ERRADO", ou se:
   - o personagem for outro: rosto, boné azul-marinho, regata azul, short preto, meia roxa, tênis branco e azul;
   - a proporção mudou: cabeça maior ou menor em relação ao corpo, perna curta;
   - o número de quadros ou a grade estiver errado, ou houver quadros cortados ou encostando;
   - houver objeto inventado, texto, chão ou sombra;
   - o fundo não for magenta chapado;
   - ele estiver virado para a esquerda;
   - faltar ou sobrar membro;
   - a pose não for a pedida em "quadros" na ficha.
3. Reprovou:
   - rode `python tools/personagem/gerar.py danilo --refazer <nome_sem_png>` (outra semente) na PRÓXIMA iteração;
   - no máximo 5 tentativas por imagem; depois disso, copie a melhor de
     imagens/assets_raw_original/danilo/tentativas/ para assets/raw/danilo/<arquivo>;
   - se o problema for o prompt, pode melhorar o texto dos "quadros" na ficha.

   Aprovou: só anote.

   Nos dois casos, escreva no log uma linha: arquivo, tentativa, aprovado ou reprovado, motivo. Termine a iteração
   (uma imagem por vez).
4. Tudo gerado:
   1. Coloque o conteúdo de tools/personagem/danilo_anims.json no Danilo do assets/data/characters.json: troque
      "anims", "icon" e "portrait"; meça "portrait_head" no retrato.
   2. Compile: `cmake --build build --config Release`.
   3. Rode `build/Release/fight_test.exe`: tem que terminar em "ok".
   4. Abra o jogo e confira no console as linhas `SPRITE: assets/raw/danilo/... escala`.
      - Folha com escala mais de 15% diferente da do walk: ajuste com `pixel_scale` (cabeça do mesmo tamanho do idle).
   5. Atualize o trecho do Danilo no CLAUDE.md.
   6. Só então responda exatamente: <promise>DANILO PRONTO</promise>
