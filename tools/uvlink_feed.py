#!/usr/bin/env python3
"""Injecte un enregistrement audio dans les décodeurs du firmware, à la place
de l'ADC, par la console USB (commande `F`). Permet de tester SARSAT / APRS /
sondes / SSTV sur la carte sans poste ni signal.

    pip install pyserial
    ./uvlink_feed.py /dev/ttyACM0 salve.pcm --mode 2              # SARSAT (16 kHz)
    ./uvlink_feed.py /dev/ttyACM0 paquets.wav --mode 1 --wait 20   # APRS (13,2 kHz), 20 s d'attente avant
                                                                   # (le temps de connecter l'appli en BLE)

Le fichier doit être à la cadence du mode (16 000 SARSAT, 13 200 APRS,
80 000 sondes, 48 000 SSTV, 32 000 phonie, POCSAG et ISM) : un WAV est rééchantillonné (linéaire) si besoin,
un .pcm (int16 LE brut) est supposé déjà à la bonne cadence.

Attention : ouvrir le port USB-C redémarre la carte (et coupe le BLE) ; le
script attend le redémarrage, puis `--wait` secondes avant d'injecter.
"""
import argparse
import struct
import sys
import time
import wave

import serial

RATES = {0: 32000, 1: 13200, 2: 16000, 3: 80000, 4: 48000, 5: 32000, 6: 32000}
NAMES = {0: "phonie", 1: "APRS", 2: "SARSAT", 3: "sonde", 4: "SSTV", 5: "POCSAG", 6: "ISM"}


def load(path, rate):
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            raw = w.readframes(n)
        if sw != 2:
            sys.exit("WAV 16 bits attendu")
        s = struct.unpack("<%dh" % (n * ch), raw)
        if ch > 1:
            s = s[::ch]
        if fr != rate:
            out, step, pos = [], fr / rate, 0.0
            while pos < len(s) - 1:
                i = int(pos); f = pos - i
                out.append(int(s[i] + (s[i + 1] - s[i]) * f))
                pos += step
            print(f"rééchantillonné {fr} -> {rate} Hz")
            s = out
        return list(s)
    with open(path, "rb") as f:
        raw = f.read()
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


ACK = b"\x06"
_pending = b""
_t0 = time.time()


def pump(ser, until, show=True, wait_ack=False):
    """Affiche la console pendant `until` s ; avec wait_ack, rend True dès qu'un
    octet ACK (0x06) arrive (le firmware acquitte chaque bloc injecté)."""
    global _pending
    end = time.time() + until
    while time.time() < end:
        chunk = ser.read(256)
        if not chunk:
            continue
        got_ack = ACK in chunk
        _pending += chunk.replace(ACK, b"")
        while b"\n" in _pending:
            line, _pending = _pending.split(b"\n", 1)
            if show:
                print(f"  {time.time() - _t0:7.2f}  " + line.decode(errors="replace").rstrip())
        if wait_ack and got_ack:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("file")
    ap.add_argument("--mode", type=int, default=2, help="0 phonie, 1 APRS, 2 SARSAT, 3 sonde, 4 SSTV, 5 POCSAG, 6 ISM")
    ap.add_argument("--wait", type=float, default=0, help="secondes d'attente avant l'injection (connexion BLE)")
    ap.add_argument("--tail", type=float, default=5, help="secondes d'écoute de la console après")
    ap.add_argument("--no-reset-wait", action="store_true", help="la carte ne redémarre pas à l'ouverture du port")
    ap.add_argument("--stats", action="store_true", help="demande les statistiques décodeurs (console 'd') à la fin")
    ap.add_argument("--realtime", action="store_true", help="injecte à la cadence réelle (sinon aussi vite que possible)")
    a = ap.parse_args()
    rate = RATES[a.mode]
    pcm = load(a.file, rate)
    data = struct.pack("<%dh" % len(pcm), *pcm)
    print(f"{len(pcm)} échantillons ({len(pcm) / rate:.1f} s) à {rate} Hz, mode {NAMES[a.mode]}")

    ser = serial.Serial(a.port, 115200, timeout=0.02)
    if not a.no_reset_wait:
        print("attente du redémarrage de la carte…")
        pump(ser, 3.0)
    ser.write(f"M {a.mode}\n".encode())
    pump(ser, 1.0)
    if a.wait:
        print(f"attente {a.wait:.0f} s (connecte l'appli maintenant)")
        pump(ser, a.wait)
    ser.write(f"F {len(pcm)}\n".encode())
    pump(ser, 0.5)
    global _t0
    t0 = _t0 = time.time()
    sent = 0
    chunks = 0
    slow = 0
    while sent < len(data):
        if a.realtime:
            while sent / 2 > (time.time() - t0) * rate:
                pump(ser, 0.005)
        n = ser.write(data[sent:sent + 4096])
        ser.flush()
        sent += n
        chunks += 1
        t1 = time.time()
        if not pump(ser, 12.0, wait_ack=True):
            print(f"t={time.time() - t0:.2f} s : pas d'acquittement du firmware après le bloc {chunks} ({sent} octets), abandon"); break
        if time.time() - t1 > 0.5:
            slow += 1
            print(f"t={time.time() - t0:.2f} s : acquittement du bloc {chunks} en {time.time() - t1:.2f} s")
    if slow:
        print(f"{slow} acquittement(s) en plus de 0,5 s")
    print(f"envoyé {sent} octets ({sent // 2} échantillons) en {time.time() - t0:.1f} s")
    pump(ser, a.tail)
    if a.stats:
        ser.write(b"d\n")
        pump(ser, 1.0)


if __name__ == "__main__":
    main()
