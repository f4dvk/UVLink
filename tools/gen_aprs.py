#!/usr/bin/env python3
"""Synthétise des paquets APRS (AX.25 UI, AFSK Bell 202 1200 bauds) en PCM
16 bits pour tester le décodeur APRS du firmware sans poste (uvlink_feed.py
--mode 1, 13 200 Hz).

    ./gen_aprs.py paquets.pcm
    ./gen_aprs.py paquets.wav --src F4DVK-9 --info '!4859.12N/00301.45E>UVLink test' --repeat 3
"""
import argparse
import math
import random
import struct
import wave


def ax25_addr(call, last=False, h=False):
    call = call.upper()
    base, ssid = (call.split("-") + ["0"])[:2]
    b = bytes((ord(c) << 1) for c in base.ljust(6)[:6])
    s = 0x60 | ((int(ssid) & 15) << 1) | (1 if last else 0) | (0x80 if h else 0)
    return b + bytes([s])


def fcs(data):
    crc = 0xFFFF
    for byte in data:
        for i in range(8):
            bit = (byte >> i) & 1
            if (crc & 1) ^ bit:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return (~crc) & 0xFFFF


def frame(src, dst, path, info):
    addrs = [ax25_addr(dst), ax25_addr(src, last=not path)]
    for i, p in enumerate(path):
        addrs.append(ax25_addr(p, last=(i == len(path) - 1)))
    f = b"".join(addrs) + b"\x03\xF0" + info.encode("latin1")
    return f + struct.pack("<H", fcs(f))


def hdlc_bits(frames, preflags=40, postflags=8):
    bits = []
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    for f in frames:
        bits += flag * preflags
        ones = 0
        for byte in f:
            for i in range(8):
                bit = (byte >> i) & 1
                bits.append(bit)
                if bit:
                    ones += 1
                    if ones == 5:
                        bits.append(0); ones = 0
                else:
                    ones = 0
        bits += flag * postflags
    return bits


def afsk(bits, rate, amp, baud=1200, f_mark=1200, f_space=2200, noise=0):
    rng = random.Random(1)
    out = []
    nrzi = 1
    phase = 0.0
    spb = rate / baud
    acc = 0.0
    for bit in bits:
        if bit == 0:
            nrzi ^= 1                      # NRZI : 0 = transition
        f = f_mark if nrzi else f_space
        acc += spb
        n = int(acc); acc -= n
        for _ in range(n):
            phase += 2 * math.pi * f / rate
            out.append(int(amp * math.sin(phase)) + (int(rng.gauss(0, noise)) if noise else 0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--rate", type=int, default=13200)
    ap.add_argument("--amp", type=int, default=8000)
    ap.add_argument("--noise", type=int, default=300, help="bruit gaussien (RMS PCM)")
    ap.add_argument("--src", default="F4DVK-9")
    ap.add_argument("--dst", default="APUVLK")
    ap.add_argument("--path", default="WIDE1-1,WIDE2-1")
    ap.add_argument("--info", default="!4859.12N/00301.45E>UVLink test 406")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--gap", type=float, default=1.0, help="silence entre paquets, s")
    a = ap.parse_args()
    path = [p for p in a.path.split(",") if p]
    infos = [a.info, f">UVLink statut {a.src}", f":F4DVK    :coucou depuis UVLink{{01"]
    pcm = []
    for i in range(a.repeat):
        f = frame(a.src, a.dst, path, infos[i % len(infos)])
        pcm += [0] * int(a.gap * a.rate)
        pcm += afsk(hdlc_bits([f]), a.rate, a.amp, noise=a.noise)
    pcm += [0] * int(a.gap * a.rate)
    pcm = [max(-32768, min(32767, s)) for s in pcm]
    data = struct.pack("<%dh" % len(pcm), *pcm)
    if a.out.lower().endswith(".wav"):
        with wave.open(a.out, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(a.rate); w.writeframes(data)
    else:
        open(a.out, "wb").write(data)
    print(f"{a.out} : {len(pcm)} échantillons à {a.rate} Hz ({len(pcm) / a.rate:.1f} s), {a.repeat} paquet(s) {a.src}>{a.dst},{a.path}")


if __name__ == "__main__":
    main()
