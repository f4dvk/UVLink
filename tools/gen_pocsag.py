#!/usr/bin/env python3
"""Synthétise une rafale POCSAG (512, 1200 ou 2400 bauds) en PCM 16 bits, telle
qu'elle sort du discriminateur FM (NRZ ±1 filtré), pour tester le décodeur du
firmware sans poste (uvlink_feed.py --mode 5, 32 000 Hz), celui de l'appli
(8 000 Hz) ou le banc PC (test/host).

    ./gen_pocsag.py rafale.pcm                                   # 1200 Bd, 32 kHz, deux messages
    ./gen_pocsag.py rafale.wav --baud 512 --rate 8000 --noise 0.3
    ./gen_pocsag.py x.pcm --msg 28328:0:"ALERTE SDIS 02 FEU DE CHEMINEE" --msg 1234567:2:0612345678 --invert

--msg RIC:FONCTION:TEXTE (texte alphanumérique 7 bits ; si TEXTE ne contient que
des chiffres et * U - ) ( espace, il est codé en numérique ; vide = bip seul).
--noise : écart-type du bruit blanc rapporté à l'amplitude du signal.
--lpf : passe-bas 1er ordre (Hz) imitant la bande du récepteur (0 = aucun).
"""
import argparse
import math
import random
import struct
import wave

SYNC = 0x7CD215D8
IDLE = 0x7A89C197
GEN = 0x769
NUMERIC = "0123456789*U -)("


def bch_rem(v):
    for i in range(30, 9, -1):
        if (v >> i) & 1:
            v ^= GEN << (i - 10)
    return v


def codeword(data21):
    """21 bits de données -> mot de 32 bits (BCH(31,21) + parité paire)."""
    c31 = (data21 << 10) | bch_rem(data21 << 10)
    return (c31 << 1) | (bin(c31).count("1") & 1)


def rev(v, n):
    return int(format(v, f"0{n}b")[::-1], 2)


def address_word(ric, func):
    return codeword(((ric >> 3) & 0x3FFFF) << 2 | (func & 3))


def message_words(text, numeric):
    bits = []
    if numeric:
        for ch in text:
            bits += [int(b) for b in format(rev(NUMERIC.index(ch), 4), "04b")]
        while len(bits) % 20:
            bits += [1, 1, 0, 0]          # bourrage : espace (0xC)
    else:
        for ch in text:
            bits += [int(b) for b in format(rev(ord(ch) & 0x7F, 7), "07b")]
        bits += [0, 0, 0, 0] * 2          # fin de texte (NUL)
        while len(bits) % 20:
            bits.append(0)
    words = []
    for i in range(0, len(bits), 20):
        w = int("".join(map(str, bits[i:i + 20])), 2)
        words.append(codeword((1 << 20) | w))
    return words


def build_batches(msgs):
    """Liste de lots de 16 mots : chaque message commence à la trame (RIC & 7)."""
    batches = []
    cur = [IDLE] * 16
    pos = 0                                # prochain emplacement libre dans cur
    for ric, func, text in msgs:
        frame = (ric & 7) * 2
        if frame < pos:
            batches.append(cur); cur = [IDLE] * 16; pos = 0
        pos = frame
        words = [address_word(ric, func)]
        if text:
            numeric = all(c in NUMERIC for c in text)
            words += message_words(text, numeric)
        for w in words:
            if pos == 16:
                batches.append(cur); cur = [IDLE] * 16; pos = 0
            cur[pos] = w; pos += 1
    batches.append(cur)
    return batches


def bits_of(batches):
    bits = [i & 1 for i in range(576)]      # préambule 1010...
    for b in batches:
        for w in [SYNC] + b:
            bits += [int(x) for x in format(w, "032b")]
    return bits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--baud", type=int, default=1200, choices=(512, 1200, 2400))
    ap.add_argument("--rate", type=int, default=32000)
    ap.add_argument("--msg", action="append", help="RIC:FONCTION:TEXTE (répétable)")
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--lpf", type=float, default=-1, help="passe-bas Hz (défaut : 1,2 x débit ; 0 = aucun)")
    ap.add_argument("--invert", action="store_true", help="polarité inverse du discriminateur")
    ap.add_argument("--amp", type=float, default=8000)
    ap.add_argument("--lead", type=float, default=0.3, help="silence (bruit) avant et après, s")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    msgs = []
    for m in a.msg or ["28328:0:ALERTE SDIS 02 FEU DE CHEMINEE RUE DU MOULIN", "1234567:2:0612345678"]:
        ric, func, text = m.split(":", 2)
        msgs.append((int(ric), int(func), text))
    bits = bits_of(build_batches(msgs))
    spb = a.rate / a.baud
    lpf = a.lpf if a.lpf >= 0 else 1.2 * a.baud
    alpha = 1.0 if lpf == 0 else 1 - math.exp(-2 * math.pi * lpf / a.rate)
    rnd = random.Random(a.seed)
    out = []
    y = 0.0
    lead = int(a.lead * a.rate)
    n = int(len(bits) * spb)
    for i in range(lead + n + lead):
        k = i - lead
        if 0 <= k < n:
            b = bits[int(k / spb)]
            x = 1.0 if (b == 1) != a.invert else -1.0
        else:
            x = 0.0
        y += alpha * (x - y)
        v = a.amp * (y + a.noise * rnd.gauss(0, 1))
        out.append(max(-32767, min(32767, int(v))))
    data = struct.pack("<%dh" % len(out), *out)
    if a.out.lower().endswith(".wav"):
        with wave.open(a.out, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(a.rate); w.writeframes(data)
    else:
        with open(a.out, "wb") as f:
            f.write(data)
    print(f"{a.out} : {len(msgs)} message(s), {a.baud} Bd, {a.rate} Hz, {len(out) / a.rate:.2f} s, bruit {a.noise}, passe-bas {lpf:.0f} Hz")


if __name__ == "__main__":
    main()
