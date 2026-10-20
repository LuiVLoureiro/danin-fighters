"""Gera as imagens de um personagem a partir SÓ da arte base dele. Ver tools/personagem/GUIA.md.

Motores: qwen (padrão, local: Qwen-Image-2.1 Turbo no stable-diffusion.cpp, C:/IA) ou gemini (Nano Banana 2, nuvem).
Uso:
  python tools/personagem/gerar.py danilo --previa          # só mostra os prompts
  python tools/personagem/gerar.py danilo                   # gera o que ainda não existe
  python tools/personagem/gerar.py danilo --uma             # gera só a PRÓXIMA que falta (para o loop)
  python tools/personagem/gerar.py danilo --so soco_sheet   # só essas (nome do arquivo, sem .png)
  python tools/personagem/gerar.py danilo --refazer soco_sheet   # gera de novo com outra semente (a anterior fica em tentativas/)
  python tools/personagem/gerar.py danilo --motor gemini    # chave em GEMINI_API_KEY, modelo em GEMINI_IMAGE_MODEL
Saída: imagens/assets_raw_original/<id>/<arquivo> (original) e assets/raw/<id>/<arquivo> (a que o jogo lê) e, no fim,
tools/personagem/<id>_anims.json com as entradas prontas para o characters.json.
"""
import argparse, io, json, os, subprocess, sys, time
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "tools", "personagem")
MODEL = os.environ.get("GEMINI_IMAGE_MODEL", "gemini-3.1-flash-image")
SD = os.environ.get("SDCPP_DIR", "C:/IA/sdcpp")  # no SSD: no HD USB (F:) o sd.cpp relê os pesos e fica ~10x mais lento
MOD = os.environ.get("QWEN_MODELOS", "C:/IA/modelos")
# 512/320 cabe na RTX 3050 Ti (4 GB) mesmo com o VS Code aberto; boneco ~150 px, sobra para os 108 do jogo
RES = int(os.environ.get("QWEN_RES", "512"))  # lado da imagem gerada (quadrada, múltiplo de 32)
REF = int(os.environ.get("QWEN_REF", "320"))  # lado maior da base enviada como referência
CANDIDATOS = int(os.environ.get("QWEN_CANDIDATOS", "3"))  # por folha; fica a de quadros mais diferentes
VARIA_MIN = 0.20  # mudança média entre quadros seguidos; andares do jogo 0,28-0,35, chute da Natália 0,59
TMP = os.environ.get("QWEN_TMP", "C:/IA/tmp")  # arquivos de trabalho no SSD
# Estilo de prompt aprovado (arrancada do Danilo). Imagem sempre quadrada, como as folhas atuais do jogo.
FUNDO = ("Background: solid flat magenta #FF00FF (pure magenta, RGB 255, 0, 255), one single uniform color filling the "
         "whole image, no gradient, no texture, no floor line, no shadow, no text, NO grid lines or cell borders, no "
         "effects, no motion lines, no dust. Crisp hard edges, no anti-aliasing against the background.\n"
         "16-bit retro arcade style, crisp pixels, thick dark outlines.")
LINHAS = {1: ["top"], 2: ["top", "bottom"], 3: ["top", "middle", "bottom"]}
COLUNAS = {1: [""], 2: ["left", "right"], 3: ["left", "center", "right"]}


def posicoes(rows, cols):
    return [f"{l}-{c}".strip("-") for l in LINHAS[rows] for c in COLUNAS[cols]]


def prompt(ficha, img):
    q = img["quadros"]
    pers = f"Character (identical in every pose): {ficha['personagem']}.\n\n"
    olhar = ("The reference image shows only his LOOK (face, clothes, colors, body proportions, art style): copy that "
             "exactly, but do NOT copy its standing pose.")
    if img.get("objeto") or img.get("campo"):
        return ("Pixel-art image based on the supplied character reference, for a cartoon retro arcade game. "
                f"{olhar}\n\n{pers}Draw a single image, not a sprite sheet: {q[0]}.\n\n{FUNDO}")
    r, c, n = img["rows"], img["cols"], len(q)
    pos = posicoes(r, c)
    vista = img.get("vista", "Strict side view, facing RIGHT (we see only one shoulder; the NEAR arm and leg are his "
                             "left ones, with the watch; the FAR arm and leg are his right ones).")
    varia = (" Like a classic frame-by-frame motion study: every frame is a DIFFERENT moment of the movement, with "
             "clearly different arm and leg positions; no two frames share the same pose." if img.get("varia", True)
             else "")
    return ("Pixel-art sprite sheet based on the supplied character reference, for a cartoon retro arcade game. "
            f"{olhar} Every frame has its own pose.\n\n{pers}"
            f"Layout: a {c}-column x {r}-row grid inside the square image: EXACTLY {n} figures in total, {c} per "
            f"row, {r} rows, one figure per cell (never two figures in one cell). Frame order: "
            + ", ".join(f"frame {k + 1} {p}" for k, p in enumerate(pos)) +
            ". Each pose stays inside its own cell, centered, same character size in every cell, the lowest foot on "
            "the same baseline, generous empty space between poses, nothing crossing into another cell, nothing cut "
            f"off.\n\n{vista} {img['acao']}.{varia}\n\n"
            + "".join(f"Frame {k + 1} ({pos[k]}): {d}\n" for k, d in enumerate(q))
            + (img["extra"] + "\n" if img.get("extra") else "") + "\n" + FUNDO)


def chave():
    k = os.environ.get("GEMINI_API_KEY")
    if not k and os.name == "nt":  # setx grava no registro; o terminal aberto antes não vê
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as reg:
                k = winreg.QueryValueEx(reg, "GEMINI_API_KEY")[0]
        except OSError:
            pass
    return k


def gerar(client, texto, refs):
    from google.genai import types
    r = client.models.generate_content(
        model=MODEL, contents=[*refs, texto],
        config=types.GenerateContentConfig(response_modalities=["IMAGE"],
                                           image_config=types.ImageConfig(aspect_ratio="1:1", image_size="1K")))
    for p in (r.candidates[0].content.parts if r.candidates and r.candidates[0].content else []):
        if p.inline_data and p.inline_data.data:
            return Image.open(io.BytesIO(p.inline_data.data)).convert("RGB")
    raise RuntimeError("o modelo não devolveu imagem: " + str(r.text or r.prompt_feedback))


def gerar_qwen(texto, refs, seed, n):
    """n candidatos numa chamada só (o texto e a base são lidos uma vez); sementes seed, seed+1..."""
    os.makedirs(TMP, exist_ok=True)
    for f in os.listdir(TMP):
        os.remove(os.path.join(TMP, f))
    cmd = [os.path.join(SD, "sd-cli.exe"),
           "--diffusion-model", os.path.join(MOD, "qwen-image-2.1-turbo-Q4_K_S.gguf"),
           "--vae", os.path.join(MOD, "qwen_image_2.1_vae_bf16.safetensors"),
           "--llm", os.path.join(MOD, "Qwen3VL-8B-Instruct-Q4_K_M.gguf"),
           "--llm_vision", os.path.join(MOD, "mmproj-Qwen3VL-8B-Instruct-Q8_0.gguf"),
           "--sigmas", "qwen-image-2.1-turbo", "--steps", "8", "--cfg-scale", "1.0", "--sampling-method", "euler",
           "--offload-to-cpu", "--flash-attn", "on", "--vae-tiling",
           "--model-args", "qwen_image_2_1_prefix_cache=false",  # o cache do prefixo não cabe nos 4 GB da placa
           # VS Code/Windows usam ~2 GB da placa: orçamento = VRAM livre na hora; texto no processador
           "--max-vram", os.environ.get("QWEN_VRAM", "0"), "--backend", "te=cpu",
           "-W", str(RES), "-H", str(RES), "--seed", str(seed), "-b", str(n), "-p", texto,
           "-o", os.path.join(TMP, "saida_%d.png"), "--output-begin-idx", "0"]
    for k, p in enumerate(refs):  # referência reduzida: menos tokens, cabe na VRAM
        im = Image.open(p).convert("RGB")
        im.thumbnail((REF, REF), Image.NEAREST)
        im.save(os.path.join(TMP, f"ref{k}.png"))
        cmd += ["-r", os.path.join(TMP, f"ref{k}.png")]
    t = time.time()
    res = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    saidas = [os.path.join(TMP, f"saida_{k}.png") for k in range(n)]
    ims = [Image.open(f).convert("RGB") for f in saidas if os.path.exists(f)]
    if not ims:
        log = res.stdout + res.stderr
        if "cannot make enough memory" in log:
            sys.exit("SEM VRAM: a placa de vídeo está ocupada por outros programas (veja nvidia-smi). Feche Chrome/Edge/"
                     "Discord ou ponha-os na GPU AMD (Configurações > Tela > Elementos gráficos > Economia de energia).")
        raise RuntimeError("sd-cli falhou:\n" + log[-3000:])
    print(f"  {len(ims)} candidato(s), {time.time() - t:.0f} s", flush=True)
    return ims


def figuras(im, rows):
    """Conta os bonecos: em cada faixa de linha, trechos de colunas com desenho separados por vão."""
    a = np.array(im).astype(int)
    fg = ~((a[..., 0] > 150) & (a[..., 2] > 150) & (a[..., 1] < 110))
    H = fg.shape[0]
    trechos = []
    for r in range(rows):
        col = fg[r * H // rows:(r + 1) * H // rows].sum(axis=0) > 2
        larg = 0
        for x in list(col) + [False]:
            if x:
                larg += 1
            elif larg:
                trechos.append(larg)
                larg = 0
    if not trechos:
        return 0
    tipico = float(np.median(trechos))
    return sum(t >= max(12, 0.4 * tipico) for t in trechos)  # pedaço solto estreito (cabelo, pé) não conta


def variacao(im, rows, cols):
    """Quanto o desenho muda de um quadro para o seguinte (1 - IoU das silhuetas, média). Perto de 0 = parado."""
    a = np.array(im).astype(int)
    fg = ~((a[..., 0] > 150) & (a[..., 2] > 150) & (a[..., 1] < 110))
    H, W = fg.shape
    sil = []
    for r in range(rows):
        for c in range(cols):
            m = fg[r * H // rows:(r + 1) * H // rows, c * W // cols:(c + 1) * W // cols]
            ys, xs = np.where(m)
            if len(ys) < 50:
                return 0.0  # célula vazia = quadro faltando
            m = m[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
            sil.append(np.array(Image.fromarray(m.astype(np.uint8) * 255).resize((48, 48))) > 127)
    # média da mudança de um quadro para o seguinte (em ciclos, quadro 1 e 4 têm a mesma silhueta: comparar todos
    # os pares daria "iguais" para todo andar)
    dif = [1 - (x & y).sum() / max(1, (x | y).sum()) for x, y in zip(sil, sil[1:])]
    return sum(dif) / len(dif) if dif else 1.0


def magenta(im):
    """Fração da borda da imagem que é magenta (fundo certo ~1)."""
    w, h = im.size
    px = [im.getpixel((x, y)) for x in range(0, w, 8) for y in (0, h - 1)] + \
         [im.getpixel((x, y)) for y in range(0, h, 8) for x in (0, w - 1)]
    return sum(r > 200 and b > 200 and g < 80 for r, g, b in px) / len(px)


def reduzir(im, img, altura):
    """CLAUDE.md: guardar com ~3x a resolução do jogo, reduzido com vizinho mais próximo em fator inteiro."""
    cel = im.height / img.get("rows", 1)
    f = max(1, round(cel * 0.8 / (3 * altura))) if "anim" in img and not img.get("objeto") else 1
    return im if f == 1 else im.resize((im.width // f, im.height // f), Image.NEAREST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("id")
    ap.add_argument("--so", nargs="*")
    ap.add_argument("--refazer", nargs="*", default=[])
    ap.add_argument("--previa", action="store_true")
    ap.add_argument("--uma", action="store_true", help="só a próxima imagem que falta")
    ap.add_argument("--motor", choices=["qwen", "gemini"], default="qwen")
    a = ap.parse_args()
    ficha = json.load(open(os.path.join(HERE, a.id + ".json"), encoding="utf-8"))
    pid = ficha["id"]
    raw, orig = os.path.join(ROOT, "assets", "raw", pid), os.path.join(ROOT, "imagens", "assets_raw_original", pid)
    tent = os.path.join(orig, "tentativas")
    for d in (raw, orig, tent):
        os.makedirs(d, exist_ok=True)
    nome = lambda img: os.path.splitext(img["arquivo"])[0]
    so = a.so or a.refazer  # --refazer X sozinho = só X (não gera o resto que falta)
    lista = [i for i in ficha["imagens"] if not so or nome(i) in so]
    if a.previa:
        for img in lista:
            print(f"=== {img['arquivo']}\n{prompt(ficha, img)}\n")
        return
    if a.motor == "gemini":
        k = chave()
        if not k:
            sys.exit("Falta a chave: crie em https://aistudio.google.com/apikey e rode  setx GEMINI_API_KEY \"sua_chave\"")
        from google import genai
        client = genai.Client(api_key=k)
    lista = [i for i in lista if not os.path.exists(os.path.join(raw, i["arquivo"])) or nome(i) in a.refazer]
    if a.uma:
        lista = lista[:1]
    if not lista:
        print("NADA FALTANDO")
    for img in lista:
        dest = os.path.join(raw, img["arquivo"])
        refs = [os.path.join(ROOT, ficha["base"])] + [os.path.join(raw, r) for r in img.get("refs", [])]
        n = len([f for f in os.listdir(tent) if f.startswith(nome(img) + "_") and "_c" not in f[len(nome(img)):]]) + 1
        print(img["arquivo"], f"tentativa {n} ...", flush=True)
        if a.motor == "gemini":
            im = gerar(client, prompt(ficha, img), [Image.open(r).convert("RGB") for r in refs])
        else:
            n_c = CANDIDATOS if "rows" in img and img.get("varia", True) else 1
            ims = gerar_qwen(prompt(ficha, img), refs, 1000 + n * 7919, n_c)
            if "rows" in img:
                n_q = img["rows"] * img["cols"]
                conta = [figuras(c, img["rows"]) for c in ims]
                notas = [variacao(c, img["rows"], img["cols"]) if f == n_q else 0.0 for c, f in zip(ims, conta)]
                if all(f != n_q for f in conta):
                    print(f"  <- NÚMERO DE QUADROS ERRADO em todos ({conta}, pedido {n_q}), refazer")
                for k, c in enumerate(ims[1:], 2):  # os não escolhidos ficam em tentativas/ para comparar
                    c.save(os.path.join(tent, f"{nome(img)}_{n}_c{k}.png"))
                best = max(range(len(ims)), key=lambda k: notas[k])
                ims.insert(0, ims.pop(best))
                v = notas[best]
                print(f"  variação dos quadros {', '.join(f'{x:.2f}' for x in notas)} -> escolhida {v:.2f}"
                      f"{'  <- QUADROS IGUAIS, refazer' if v < VARIA_MIN and img.get('varia', True) else ''}")
            im = ims[0]
        im.save(os.path.join(tent, f"{nome(img)}_{n}.png"))
        im.save(os.path.join(orig, img["arquivo"]))
        reduzir(im, img, ficha["altura"]).save(dest)
        m = magenta(im)
        print(f"  {im.width}x{im.height}, borda magenta {m:.0%}{'  <- FUNDO ERRADO, refazer' if m < 0.9 else ''}")
    # entradas do characters.json para o que já existe
    out = {"anims": {}}
    for img in ficha["imagens"]:
        f = f"assets/raw/{pid}/{img['arquivo']}"
        if not os.path.exists(os.path.join(ROOT, f)):
            continue
        if "campo" in img:
            out[img["campo"]] = f
        elif "anim" in img:
            e = {"file": f}
            if not img.get("objeto"):
                e.update(rows=img["rows"], cols=img["cols"])
            e.update(img.get("json", {}))
            out["anims"][img["anim"]] = e
    json.dump(out, open(os.path.join(HERE, pid + "_anims.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print(f"{len(out['anims'])} anims em tools/personagem/{pid}_anims.json")


if __name__ == "__main__":
    main()
