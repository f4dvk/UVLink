#!/usr/bin/env python3
"""Client BLE de test pour UVLink (Ubuntu, BlueZ) — pas besoin de l'appli.

    pip install bleak
    ./uvlink_ble.py                 # scanne, se connecte au premier "UVLink"
    ./uvlink_ble.py AA:BB:CC:DD:EE:FF

Commandes : ping, info, vfo V RXMHZ [TXMHZ] [SQ] [NARROW], radio TXVFO DW,
ptt 1|0, mon 1|0, ch V CH, mode N, audio MODE [GAIN], rec SEC fichier.wav,
play fichier.wav | tone [SEC] [HZ], quit. Le STATUS est affiché à chaque
notification.
"""
import asyncio
import math
import struct
import sys
import time
import wave

from bleak import BleakClient, BleakScanner

SVC      = "9b0e0001-6f3a-4d2b-8e5c-3a1f2c4d5e60"
CONTROL  = "9b0e0002-6f3a-4d2b-8e5c-3a1f2c4d5e60"
STATUS   = "9b0e0003-6f3a-4d2b-8e5c-3a1f2c4d5e60"
AUDIO_RX = "9b0e0004-6f3a-4d2b-8e5c-3a1f2c4d5e60"
AUDIO_TX = "9b0e0005-6f3a-4d2b-8e5c-3a1f2c4d5e60"
RATE     = 8000        # cadence phonie (flags b2-b3 des paquets : 1 = 8 kHz, 0 = 16 kHz)
PKT_SAMPLES = 320      # 40 ms @ 8 kHz

# ---- IMA ADPCM (même codec que firmware/src/Adpcm.h) ----
_IDX  = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
         4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
         22385, 24623, 27086, 29794, 32767]


class Adpcm:
    def __init__(self):
        self.pred = 0
        self.idx = 0

    def decode(self, nib):
        step = _STEP[self.idx]
        vp = step >> 3
        if nib & 4: vp += step
        if nib & 2: vp += step >> 1
        if nib & 1: vp += step >> 2
        p = self.pred - vp if nib & 8 else self.pred + vp
        self.pred = max(-32768, min(32767, p))
        self.idx = max(0, min(88, self.idx + _IDX[nib & 15]))
        return self.pred

    def encode(self, s):
        step = _STEP[self.idx]
        diff = s - self.pred
        nib = 0
        if diff < 0: nib, diff = 8, -diff
        vp = step >> 3
        if diff >= step: nib |= 4; diff -= step; vp += step
        step >>= 1
        if diff >= step: nib |= 2; diff -= step; vp += step
        step >>= 1
        if diff >= step: nib |= 1; vp += step
        p = self.pred - vp if nib & 8 else self.pred + vp
        self.pred = max(-32768, min(32767, p))
        self.idx = max(0, min(88, self.idx + _IDX[nib]))
        return nib

    def decode_packet(self, pkt):
        """seq, flags, pred:i16, idx:u8, adpcm[...] -> liste d'int16"""
        self.pred, self.idx = struct.unpack("<hB", pkt[2:5])
        out = []
        for b in pkt[5:]:
            out.append(self.decode(b & 15)); out.append(self.decode(b >> 4))
        return out

    def encode_packet(self, seq, flags, pcm):
        hdr = struct.pack("<BBhB", seq & 255, flags, self.pred, self.idx)
        body = bytearray()
        for i in range(0, len(pcm) - 1, 2):
            body.append(self.encode(pcm[i]) | (self.encode(pcm[i + 1]) << 4))
        return hdr + bytes(body)


_rec = {"wav": None, "dec": Adpcm(), "n": 0, "seq": None, "lost": 0, "t0": 0}


def on_audio(_, pkt: bytearray):
    r = _rec
    if r["wav"] is None:
        return
    if r["seq"] is not None and (r["seq"] + 1) & 255 != pkt[0] and not (pkt[1] & 1):
        r["lost"] += 1
    r["seq"] = pkt[0]
    rate = 8000 if ((pkt[1] >> 2) & 3) == 1 else 16000
    if r["n"] == 0:
        r["wav"].setframerate(rate)
        if rate != RATE:
            print(f"  (cadence {rate} Hz)")
    pcm = r["dec"].decode_packet(pkt)
    r["wav"].writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
    r["n"] += 1
    if r["n"] == 1:
        print(f"  1er paquet après {time.time() - r['t0']:.2f} s")


async def record(cli, sec, path):
    r = _rec
    r["wav"] = wave.open(path, "wb"); r["wav"].setnchannels(1); r["wav"].setsampwidth(2)
    r["n"] = 0; r["lost"] = 0; r["seq"] = None; r["t0"] = time.time()
    await cli.start_notify(AUDIO_RX, on_audio)
    await asyncio.sleep(sec)
    await cli.stop_notify(AUDIO_RX)
    w = r["wav"]; r["wav"] = None; w.close()
    dt = time.time() - r["t0"]
    print(f"{r['n']} paquets en {dt:.1f} s ({r['n'] / dt:.1f}/s, {r['n'] * PKT_SAMPLES / dt:.0f} ech/s), "
          f"{r['lost']} trous -> {path}")


async def play(cli, pcm, ack=False):
    enc = Adpcm()
    n = len(pcm) // PKT_SAMPLES
    t0 = time.time()
    for i in range(n):
        flags = (1 if i == 0 else 0) | (2 if i == n - 1 else 0) | (4 if RATE == 8000 else 0)
        pkt = enc.encode_packet(i, flags, pcm[i * PKT_SAMPLES:(i + 1) * PKT_SAMPLES])
        await cli.write_gatt_char(AUDIO_TX, pkt, response=ack)
        # cadence temps réel : un paquet toutes les PKT_SAMPLES/RATE s, avec 2 paquets d'avance
        await asyncio.sleep(max(0, t0 + (i - 2) * PKT_SAMPLES / RATE - time.time()))
    print(f"{n} paquets envoyés en {time.time() - t0:.2f} s")


def load_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != RATE:
            print(f"il faut un WAV mono 16 bits {RATE} Hz"); return None
        raw = w.readframes(w.getnframes())
    return list(struct.unpack(f"<{len(raw) // 2}h", raw))


def tone(sec, hz):
    return [int(20000 * math.sin(2 * math.pi * hz * i / RATE)) for i in range(int(sec * RATE))]


def on_control(_, data: bytearray):
    cmd = data[0]
    if cmd == 0x01:
        print(f"<- PING proto={data[1]} fw={data[2]}.{data[3]}")
    elif cmd == 0x02:
        print(f"<- INFO poste={'oui' if data[1] else 'non'} hostproto=H{data[2]} fw={data[3:].decode(errors='replace')!r}")
    elif cmd == 0x11:
        vfo, rx, tx, mod, nar, crx, ctx, pwr, sq, fl = struct.unpack("<BIIBBHHBBB", data[1:20])
        print(f"<- VFO{vfo} rx={rx/1e6:.5f} tx={tx/1e6:.5f} mod={mod} narrow={nar} "
              f"ctcss={crx/10}/{ctx/10} pwr={pwr} sq={sq} flags={fl}")
    elif cmd == 0xFF:
        print(f"<- commande inconnue 0x{data[1]:02X}")
    elif cmd == 0x40:
        on_event(None, bytearray([6]) + data[1:])
    else:
        print(f"<- 0x{cmd:02X} ok={data[1] if len(data) > 1 else '?'}")


def on_status(_, d: bytearray):
    ver, present, fl, sm, rssi, batt, rx0, tx0, rx1, tx1, mode, gps, dc = struct.unpack("<BBBBhHIIIIBBH", d[:29])
    print(f"[status] poste={present} sig={fl & 1} tx={(fl >> 1) & 1} dw={(fl >> 2) & 1} "
          f"S={sm} rssi={rssi}dBm batt={batt}mV vfo0={rx0/1e6:.4f}/{tx0/1e6:.4f} "
          f"vfo1={rx1/1e6:.4f}/{tx1/1e6:.4f} mode={mode} adc_dc={dc}")


EVENT = "9b0e0006-6f3a-4d2b-8e5c-3a1f2c4d5e60"
DATA  = "9b0e0007-6f3a-4d2b-8e5c-3a1f2c4d5e60"
_sstv_lines = 0


def _cstr(b, o):
    e = b.find(b"\0", o)
    e = len(b) if e < 0 else e
    return b[o:e].decode("latin1"), e + 1


def on_event(_, d: bytearray):
    global _sstv_lines
    t, p = d[0], bytes(d[1:])
    try:
        if t == 1:
            peak, rms, dc, clip, verdict = struct.unpack("<HHHBB", p[:8])
            extra = struct.unpack("<HHH", p[8:14]) if len(p) >= 14 else ()
            print(f"[level] crete={peak} rms={rms} dc={dc} clip={clip}% verdict={verdict} {extra}")
        elif t == 2:
            fb, proto, test, haspos, cc, lat, lon = struct.unpack("<BBBBHii", p[:14])
            hexid = p[14:29].rstrip(b"\0").decode()
            rep = p[29] if len(p) > 29 else 0
            bch = p[30] if len(p) > 30 else 0
            raw = _cstr(p, 31)[0] if len(p) > 31 else ""
            print(f"[SARSAT] {fb} bits ID={hexid} proto={proto} test={test} pays={cc} pos={haspos} {lat/1e5:.5f} {lon/1e5:.5f} "
                  f"repeat={rep} bch+{bch} brut={raw}")
        elif t == 3:
            kind, fl, st, sc, lat, lon, cse, spd, alt, dist, brg = struct.unpack("<BBccIIHHhHH", p[:22])
            src, o = _cstr(p, 22); name, o = _cstr(p, o); via, o = _cstr(p, o); text, o = _cstr(p, o)
            print(f"[APRS] {src} kind={kind} flags={fl:#x} sym={st.decode('latin1')}{sc.decode('latin1')} "
                  f"{struct.unpack('<i', struct.pack('<I', lat))[0]/1e5:.5f} {struct.unpack('<i', struct.pack('<I', lon))[0]/1e5:.5f} "
                  f"{cse}deg {spd}km/h {alt}m dist={dist/10:.1f}km brg={brg} name={name!r} via={via!r} : {text}")
        elif t == 4:
            kind, haspos, lat, lon, alt = struct.unpack("<BBiii", p[:14])
            print(f"[SONDE] kind={kind} pos={haspos} {lat/1e5:.5f} {lon/1e5:.5f} alt={alt}")
        elif t == 5:
            line, x0, n = struct.unpack("<HHB", p[:5])
            if line == 0xFFFF:
                _sstv_lines = 0; print("[SSTV] nouvelle image")
            elif x0 == 0:
                _sstv_lines += 1
                px = struct.unpack("<%dH" % n, p[5:5 + 2 * n])
                rgb = lambda v: (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
                print(f"[SSTV] ligne {line} px0={rgb(px[0])} px40={rgb(px[40])} px60={rgb(px[60])}")
        elif t == 6:
            fl, lat, lon, spd, cse, alt, sats, h, m, s = struct.unpack("<BiiHHhBBBB", p[:20])
            print(f"[GPS] fix={fl & 1} vu={(fl >> 1) & 1} {lat/1e5:.5f} {lon/1e5:.5f} {spd}km/h {cse}deg {alt}m {sats}sat {h:02}:{m:02}:{s:02}")
        elif t == 7:
            src, line, inv = p[0], p[1], p[2]
            txt = p[3:].decode("latin1")
            print(f"[text{src}] " + ("--- effacer ---" if line == 255 else f"L{line}{'*' if inv else ' '} {txt}"))
        elif t == 8:
            mode, fl, lines, syncs, ppm = struct.unpack("<BBHHi", p[:10])
            print(f"[SSTV info] mode={mode} valide={fl & 1} finale={(fl >> 1) & 1} lignes={lines} synchros={syncs} {ppm:+d} ppm")
        elif t == 9:
            frag, baud, ric, func, fl, corr, bad = struct.unpack("<BHIBBBB", p[:11])
            txt = _cstr(p, 11)[0] if len(p) > 11 else ""
            kind = "bip seul" if fl & 2 else ("numerique" if fl & 1 else "alpha")
            print(f"[POCSAG{baud}] frag={frag & 0x7F}{'(fin)' if frag & 0x80 else ''} RIC {ric:07d} F{func} {kind}"
                  f"{' douteux' if fl & 4 else ''} bch+{corr} bad={bad} : {txt}")
        elif t == 10:
            frag, pid = p[0], p[1]
            pairs = [struct.unpack_from("<HH", p, 2 + 4 * i) for i in range((len(p) - 2) // 4)]
            print(f"[ISM] paquet {pid} frag={frag & 0x7F}{'(fin)' if frag & 0x80 else ''} {len(pairs)} paires (µs) : "
                  + " ".join(f"{a}/{b}" for a, b in pairs[:12]) + (" ..." if len(pairs) > 12 else ""))
        else:
            print(f"[event 0x{t:02X}] {p.hex()}")
    except struct.error:
        print(f"[event 0x{t:02X} tronque] {p.hex()}")


_ax25 = bytearray()


def on_data(_, d: bytearray):
    global _ax25
    frag = d[0]
    if (frag & 0x7F) == 0:
        _ax25 = bytearray()
    _ax25 += d[1:]
    if frag & 0x80:
        f = bytes(_ax25)
        def addr(o):
            s = "".join(chr(f[o + i] >> 1) for i in range(6)).strip()
            ssid = (f[o + 6] >> 1) & 15
            return s + (f"-{ssid}" if ssid else "")
        if len(f) >= 16:
            path, o = [], 14
            if not f[13] & 1:
                while o + 7 <= len(f):
                    path.append(addr(o) + ("*" if f[o + 6] & 0x80 else "")); last = f[o + 6] & 1; o += 7
                    if last: break
            info = f[o + 2:].decode("latin1", errors="replace")
            print(f"[AX.25] {addr(7)}>{addr(0)}{',' + ','.join(path) if path else ''}:{info}")
        else:
            print(f"[AX.25] {f.hex()}")


def build(cmd: str) -> bytes | None:
    a = cmd.split()
    if not a:
        return None
    try:
        if a[0] == "ping":
            return b"\x01"
        if a[0] == "info":
            return b"\x02"
        if a[0] == "vfo":
            v = int(a[1]); rx = int(float(a[2]) * 1e6)
            tx = int(float(a[3]) * 1e6) if len(a) > 3 else rx
            sq = int(a[4]) if len(a) > 4 else 1
            nar = int(a[5]) if len(a) > 5 else 0
            return struct.pack("<BBIIBBHHBBB", 0x10, v, rx, tx, 0, nar, 0, 0, 7, sq, 0)
        if a[0] == "getvfo":
            return bytes([0x11, int(a[1])])
        if a[0] == "radio":
            return bytes([0x12, int(a[1]), int(a[2]), 0])
        if a[0] == "ptt":
            return bytes([0x13, int(a[1])])
        if a[0] == "mon":
            return bytes([0x14, int(a[1])])
        if a[0] == "ch":
            return struct.pack("<BBH", 0x15, int(a[1]), int(a[2]))
        if a[0] == "mode":
            return bytes([0x20, int(a[1])])
        if a[0] == "audio":      # audio MODE [GAIN] : flux RX 0 off / 1 toujours / 2 auto
            return bytes([0x21, int(a[1]), int(a[2]) if len(a) > 2 else 0, 0])
        if a[0] == "gps":
            return b"\x40"
        if a[0] == "pos":        # pos LAT LON (degrés décimaux) : position de l'opérateur
            return struct.pack("<Bii", 0x41, int(float(a[1]) * 1e5), int(float(a[2]) * 1e5))
    except (IndexError, ValueError):
        pass
    print("commandes : ping | info | vfo V RXMHZ [TXMHZ] [SQ] [NARROW] | getvfo V | "
          "radio TXVFO DW | ptt 1|0 | mon 1|0 | ch V CH | mode N (0 phonie 1 APRS 2 SARSAT 3 sonde 4 SSTV 5 POCSAG 6 ISM) | "
          "audio MODE [GAIN] | gps | pos LAT LON | rec SEC f.wav | play f.wav | tone [SEC] [HZ] | quit")
    return None


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "-" else None
    if addr is None:
        print("scan...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: SVC in (ad.service_uuids or []) or (d.name or "") == "UVLink", timeout=10)
        if dev is None:
            print("aucun UVLink trouvé"); return
        addr = dev.address
    print(f"connexion à {addr}")
    async with BleakClient(addr) as cli:
        await cli.start_notify(CONTROL, on_control)
        await cli.start_notify(STATUS, on_status)
        await cli.start_notify(EVENT, on_event)
        await cli.start_notify(DATA, on_data)
        await cli.write_gatt_char(CONTROL, b"\x01", response=True)
        loop = asyncio.get_running_loop()
        # commandes passées sur la ligne de commande après l'adresse : exécutées puis on écoute
        # ("sleep N" = attendre N s), ex. : uvlink_ble.py - "mode 2" "sleep 60" quit
        scripted = list(sys.argv[2:])
        while True:
            if scripted:
                line = scripted.pop(0)
                print("> " + line)
                if line.startswith("sleep"):
                    await asyncio.sleep(float(line.split()[1])); continue
            else:
                line = await loop.run_in_executor(None, input, "> ")
            a = line.split()
            if not a:
                continue
            if a[0] == "quit":
                break
            if a[0] == "rec" and len(a) > 2:
                await record(cli, float(a[1]), a[2]); continue
            if a[0] == "play" and len(a) > 1:
                pcm = load_wav(a[1])
                if pcm: await play(cli, pcm)
                continue
            if a[0] in ("tone", "toneack"):   # toneack : écritures avec réponse (test)
                await play(cli, tone(float(a[1]) if len(a) > 1 else 2, float(a[2]) if len(a) > 2 else 1000), ack=a[0] == "toneack"); continue
            pkt = build(line)
            if pkt:
                await cli.write_gatt_char(CONTROL, pkt, response=True)
                await asyncio.sleep(0.3)


if __name__ == "__main__":
    asyncio.run(main())
