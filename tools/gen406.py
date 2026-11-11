#!/usr/bin/env python3
"""Synthétise une salve COSPAS-SARSAT 1G (bi-phase L, 400 bauds) en PCM 16 bits
pour tester le décodeur sans balise : `uvlink_feed.py` l'injecte dans le
firmware à la place de l'ADC, ou on la joue dans un fichier WAV.

    ./gen406.py salve.pcm                       # trame de test ELT-DT France (144 bits)
    ./gen406.py salve.wav --hex FFFE2F8E39...   # autre trame (36 hex = 144 bits, 28 = 112)
    ./gen406.py salve.pcm --hiss 0              # sans souffle avant / après

Même générateur que rp2040/test/host/test_slicer.c (gen_burst) : porteuse de
tête à +amp pendant 50 ms, bits Manchester ('1' = +A puis -A), puis bruit ;
encadré ici par du souffle FM (sans porteuse) pour exercer le détecteur de
salve relatif au plancher.
"""
import argparse
import random
import struct
import wave

FRAMES = {
    "eltdt":   "FFFED08E39048D158AC01E3AA482856824CE",   # ELT-DT, France, position composite
    "offsets": "FFFE2F8E390000000AE018A81700EDA84498",   # ELT-DT, France, offsets
    "rls":     "FFFED090FD15E0059FEFFC28BEB861F0FABE",   # RLS Location, Turkiye, position par défaut
}


def gen(hexframe, rate, amp, hiss_rms, lead_ms, pre_s, post_s, seed=1):
    rng = random.Random(seed)
    bits = []
    for h in hexframe:
        v = int(h, 16)
        bits += [(v >> 3) & 1, (v >> 2) & 1, (v >> 1) & 1, v & 1]
    nbits = 144 if len(bits) >= 144 else 112
    bits = bits[:nbits]
    spb = rate // 400
    out = []
    hiss = lambda: int(rng.gauss(0, hiss_rms)) if hiss_rms > 0 else 0
    noise = lambda: rng.randint(-200, 200)
    out += [hiss() for _ in range(int(pre_s * rate))]
    out += [amp + noise() for _ in range(rate * lead_ms // 1000)]
    for b in bits:
        first = amp if b else -amp
        for k in range(spb):
            out.append((first if k < spb // 2 else -first) + noise())
    out += [noise() for _ in range(rate // 20)]
    out += [hiss() for _ in range(int(post_s * rate))]
    return [max(-32768, min(32767, s)) for s in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", help="fichier .pcm (int16 LE brut) ou .wav")
    ap.add_argument("--hex", default=FRAMES["eltdt"], help="trame complète en hexa (synchro bit + trame)")
    ap.add_argument("--frame", choices=FRAMES.keys(), help="trame de test nommée")
    ap.add_argument("--rate", type=int, default=16000)
    # Le firmware voit PCM/2 (12 bits << 3) : 8000 -> 4000 (crête >= 3000 = armement absolu),
    # 1500 -> 750 de souffle. Un vrai récepteur donne un souffle bien plus fort que
    # la salve (détecteur relatif), mais ce générateur ne reproduit pas la capture du
    # récepteur par la porteuse ; on passe donc par l'armement absolu.
    ap.add_argument("--amp", type=int, default=8000, help="amplitude des bits (PCM)")
    ap.add_argument("--hiss", type=int, default=1500, help="RMS du souffle FM avant/après (PCM), 0 = silence")
    ap.add_argument("--lead", type=int, default=160, help="porteuse de tête, ms (balise réelle : 160)")
    ap.add_argument("--pre", type=float, default=2.0, help="souffle avant, s")
    ap.add_argument("--post", type=float, default=2.0, help="souffle après, s")
    ap.add_argument("--repeat", type=int, default=1, help="nombre de salves")
    a = ap.parse_args()
    hexframe = FRAMES[a.frame] if a.frame else a.hex
    pcm = []
    for i in range(a.repeat):
        pcm += gen(hexframe, a.rate, a.amp, a.hiss, a.lead, a.pre, a.post, seed=i + 1)
    data = struct.pack("<%dh" % len(pcm), *pcm)
    if a.out.lower().endswith(".wav"):
        with wave.open(a.out, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(a.rate); w.writeframes(data)
    else:
        with open(a.out, "wb") as f:
            f.write(data)
    print(f"{a.out} : {len(pcm)} échantillons à {a.rate} Hz ({len(pcm) / a.rate:.2f} s), trame {len(hexframe) * 4} bits {hexframe}")


if __name__ == "__main__":
    main()
