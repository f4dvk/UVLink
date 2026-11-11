#!/usr/bin/env python3
"""Transforme un fichier d'impulsions rtl_433 « .ook » en audio discriminateur
FM synthétique (PCM 16 bits, 32 kHz par défaut) tel que le sort l'UV-K5 en
audio plat : souffle gaussien filtré sans porteuse, quasi-silence pendant les
impulsions, transitions adoucies par la FI (~120 µs). Sert au banc du
détecteur (test/host/test_ook.c) et à l'injection dans l'ESP
(uvlink_feed.py --mode 6).

    ./gen_ook.py nexus.ook sortie.pcm [--rate 32000] [--noise-rms 540] [--residual 0.05] [--lead 0.3]
"""
import argparse, math, random, struct, wave

def load_ook(path):
    packets, cur = [], []
    for line in open(path):
        line = line.strip()
        if line.startswith(';'):
            if line in (';ook', ';fsk') and cur: packets.append(cur); cur = []
            continue
        a = line.split()
        if len(a) >= 2: cur.append((int(a[0]), int(a[1])))
    if cur: packets.append(cur)
    return packets

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ook"); ap.add_argument("out")
    ap.add_argument("--rate", type=int, default=32000)
    ap.add_argument("--noise-rms", type=float, default=540, help="RMS du souffle (LSB)")
    ap.add_argument("--residual", type=float, default=0.05, help="souffle résiduel pendant la porteuse (fraction)")
    ap.add_argument("--lpf", type=float, default=9000, help="bande du souffle (Hz)")
    ap.add_argument("--fi-us", type=float, default=120, help="constante de temps des transitions (µs)")
    ap.add_argument("--lead", type=float, default=0.3, help="souffle avant / après et entre paquets (s)")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    packets = load_ook(a.ook)
    rnd = random.Random(a.seed); r = a.rate
    # enveloppe « porteuse » échantillon par échantillon
    env = []
    lead = int(a.lead * r)
    env += [0.0] * lead
    for pk in packets:
        for p, g in pk:
            env += [1.0] * int(p * r / 1e6 + 0.5)
            env += [0.0] * int(g * r / 1e6 + 0.5)
        env += [0.0] * lead
    # transitions adoucies (1er ordre), souffle filtré
    k_fi = 1 - math.exp(-1 / (a.fi_us * 1e-6 * r)); k_n = 1 - math.exp(-2 * math.pi * a.lpf / r)
    e = 0.0; n = 0.0; out = []
    gain = a.noise_rms * math.sqrt(2 / k_n)   # compensation grossière du filtrage
    for v in env:
        e += k_fi * (v - e)
        n += k_n * (rnd.gauss(0, 1) - n)
        amp = (1 - e) + a.residual * e
        out.append(max(-32767, min(32767, int(gain * n * amp))))
    data = struct.pack("<%dh" % len(out), *out)
    if a.out.lower().endswith(".wav"):
        with wave.open(a.out, "wb") as w: w.setnchannels(1); w.setsampwidth(2); w.setframerate(r); w.writeframes(data)
    else: open(a.out, "wb").write(data)
    print(f"{a.out} : {len(packets)} paquet(s), {sum(len(p) for p in packets)} impulsions, {len(out)/r:.2f} s à {r} Hz")

if __name__ == "__main__": main()
