#!/usr/bin/env python3
"""Synthétise une image SSTV (mire de couleurs ou image PNG) en PCM 16 bits
à 48 kHz, pour tester le décodeur SSTV du firmware et l'affichage dans
l'appli (uvlink_feed.py --mode 4 ou --mode 0).

    ./gen_sstv.py mire.pcm                        # Scottie 1, mire 320x256, image complète (~110 s)
    ./gen_sstv.py mire.pcm --mode pd120           # PD 120 (640x496)
    ./gen_sstv.py mire.pcm --lines 48             # seulement les 48 premières lignes (test rapide)
    ./gen_sstv.py photo.pcm --png image.png       # nécessite Pillow
    ./gen_sstv.py mire.pcm --no-start-sync        # Scottie sans la synchro de 9 ms avant la ligne 0
                                                  # (émetteur UV-K5 + RP2040, HTCommander)
    ./gen_sstv.py mire.pcm --rate 32051           # cadence de sortie (chemin phonie de l'ESP)

Modes (SSTV Handbook OK2MNM ch. 4, MMSSTV) : scottie1 (VIS 60), scottie2 (56),
scottiedx (76), martin1 (44), martin2 (40), pd50 (93), pd90 (99), pd120 (95),
pd160 (98), pd180 (96), pd240 (97), pd290 (94). Même table que
firmware/lib/decoders/sstv_demod.c.
  Scottie : sep 1,5 ms + V + sep + B + synchro 9 ms + porche 1,5 ms + R
  Martin  : synchro 4,862 ms + porche 0,572 + V + sep 0,572 + B + sep + R + sep
  PD      : synchro 20 ms + porche 2,08 + Y(pair) + R-Y + B-Y + Y(impair)  (2 lignes)
"""
import argparse
import math
import struct

import numpy as np

# nom : (vis, famille, largeur, hauteur, synchro, porche, sep, composante)  durées en ms
MODES = {
    "scottie1":  (60, "scottie", 320, 256, 9.0,   1.5,   1.5,   138.240),
    "scottie2":  (56, "scottie", 320, 256, 9.0,   1.5,   1.5,    88.064),
    "scottiedx": (76, "scottie", 320, 256, 9.0,   1.5,   1.5,   345.600),
    "martin1":   (44, "martin",  320, 256, 4.862, 0.572, 0.572, 146.432),
    "martin2":   (40, "martin",  320, 256, 4.862, 0.572, 0.572,  73.216),
    "pd50":      (93, "pd",      320, 256, 20.0,  2.08,  0.0,    91.520),
    "pd90":      (99, "pd",      320, 256, 20.0,  2.08,  0.0,   170.240),
    "pd120":     (95, "pd",      640, 496, 20.0,  2.08,  0.0,   121.600),
    "pd160":     (98, "pd",      512, 400, 20.0,  2.08,  0.0,   195.584),
    "pd180":     (96, "pd",      640, 496, 20.0,  2.08,  0.0,   183.040),
    "pd240":     (97, "pd",      640, 496, 20.0,  2.08,  0.0,   244.480),
    "pd290":     (94, "pd",      800, 616, 20.0,  2.08,  0.0,   228.800),
}


class Tone:
    """Synthèse FM à phase continue : add(hz, sec) ou add_scan(freqs, sec_total)."""

    def __init__(self, rate, amp):
        self.rate, self.amp, self.phase, self.acc = rate, amp, 0.0, 0.0
        self.chunks = []

    def _emit(self, hz_per_sample):
        inc = 2 * math.pi * hz_per_sample / self.rate
        ph = self.phase + np.cumsum(inc)
        self.phase = float(ph[-1]) if len(ph) else self.phase
        self.chunks.append(np.rint(self.amp * np.sin(ph)).astype(np.int16))

    def add(self, hz, sec):
        self.acc += sec * self.rate
        n = int(self.acc); self.acc -= n
        if n: self._emit(np.full(n, float(hz)))

    def add_scan(self, freqs, sec):
        """freqs : une fréquence par pixel, balayés uniformément pendant sec."""
        self.acc += sec * self.rate
        n = int(self.acc); self.acc -= n
        if n:
            idx = (np.arange(n) * len(freqs)) // n
            self._emit(np.asarray(freqs, dtype=float)[idx])

    def pcm(self):
        return np.concatenate(self.chunks) if self.chunks else np.zeros(0, np.int16)


def luma_hz(v):
    return 1500.0 + 800.0 * (np.asarray(v, dtype=float) / 255.0)


def mire(w, h):
    """Mire : 8 barres de couleur, dégradé de gris, damier. Tableau h x w x 3."""
    img = np.zeros((h, w, 3), np.uint8)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    y1, y2 = h * 160 // 256, h * 208 // 256
    for i, c in enumerate(bars):
        img[:y1, i * w // 8:(i + 1) * w // 8] = c
    g = (np.arange(w) * 255 // (w - 1)).astype(np.uint8)
    img[y1:y2] = np.stack([g, g, g], axis=1)[None, :, :]
    cw, ch = w // 16, h // 16
    yy, xx = np.mgrid[y2:h, 0:w]
    img[y2:] = np.where((((xx // cw) + (yy // ch)) % 2 == 0)[:, :, None], 255, 0)
    return img


def vis_header(t, vis):
    # VIS : leader 1900 (300 ms), break 1200 (10 ms), leader 1900 (300 ms), start 1200 (30 ms),
    # 7 bits LSB first (1 = 1100 Hz, 0 = 1300 Hz) + parité paire + stop 1200 (30 ms)
    t.add(1900, 0.300); t.add(1200, 0.010); t.add(1900, 0.300); t.add(1200, 0.030)
    par = 0
    for i in range(7):
        b = (vis >> i) & 1; par ^= b
        t.add(1100 if b else 1300, 0.030)
    t.add(1100 if par else 1300, 0.030)
    t.add(1200, 0.030)


def ycrcb(rgb):
    """BT.601 plage studio (MMSSTV) : Y 16..235, Cr/Cb centrés sur 128."""
    r, g, b = [rgb[..., i].astype(float) for i in range(3)]
    y = 16 + (65.738 * r + 129.057 * g + 25.064 * b) / 256
    cr = 128 + (112.439 * r - 94.154 * g - 18.285 * b) / 256
    cb = 128 + (-37.945 * r - 74.494 * g + 112.439 * b) / 256
    return y, cr, cb


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--mode", default="scottie1", choices=sorted(MODES))
    ap.add_argument("--lines", type=int, default=0, help="nombre de lignes (défaut : toute l'image)")
    ap.add_argument("--png")
    ap.add_argument("--amp", type=int, default=10000)
    ap.add_argument("--rate", type=int, default=48000, help="cadence d'échantillonnage (Hz)")
    ap.add_argument("--no-start-sync", action="store_true", help="Scottie : pas de synchro 9 ms avant la ligne 0")
    a = ap.parse_args()
    vis, fam, w, h, sync, porch, sep, chan = MODES[a.mode]
    sync, porch, sep, chan = sync / 1000, porch / 1000, sep / 1000, chan / 1000
    if a.png:
        from PIL import Image
        img = np.asarray(Image.open(a.png).convert("RGB").resize((w, h)), dtype=np.uint8)
    else:
        img = mire(w, h)
    nlines = min(a.lines, h) if a.lines > 0 else h
    t = Tone(a.rate, a.amp)
    vis_header(t, vis)
    if fam == "scottie":
        if not a.no_start_sync:
            t.add(1200, sync)                            # starting sync (une seule fois)
        for y in range(nlines):
            t.add(1500, sep); t.add_scan(luma_hz(img[y, :, 1]), chan)
            t.add(1500, sep); t.add_scan(luma_hz(img[y, :, 2]), chan)
            t.add(1200, sync); t.add(1500, porch); t.add_scan(luma_hz(img[y, :, 0]), chan)
    elif fam == "martin":
        for y in range(nlines):
            t.add(1200, sync); t.add(1500, porch)
            t.add_scan(luma_hz(img[y, :, 1]), chan); t.add(1500, sep)
            t.add_scan(luma_hz(img[y, :, 2]), chan); t.add(1500, sep)
            t.add_scan(luma_hz(img[y, :, 0]), chan); t.add(1500, sep)
    else:
        yy, cr, cb = ycrcb(img)
        for y in range(0, nlines, 2):
            y2 = min(y + 1, h - 1)
            t.add(1200, sync); t.add(1500, porch)
            t.add_scan(luma_hz(yy[y]), chan)
            t.add_scan(luma_hz((cr[y] + cr[y2]) / 2), chan)
            t.add_scan(luma_hz((cb[y] + cb[y2]) / 2), chan)
            t.add_scan(luma_hz(yy[y2]), chan)
    t.add(1900, 0.5)
    pcm = t.pcm()
    open(a.out, "wb").write(pcm.tobytes())
    print(f"{a.out} : {len(pcm)} échantillons à {a.rate} Hz ({len(pcm) / a.rate:.1f} s), {nlines} ligne(s) {a.mode} {w}x{h}")


if __name__ == "__main__":
    main()
