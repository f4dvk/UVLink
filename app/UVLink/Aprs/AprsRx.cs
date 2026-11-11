namespace UVLink.Aprs;

// Récepteur AFSK 1200 Bd / AX.25 : portage C# de firmware/lib/decoders/aprs_rx.c
// (lui-même porté du pico_tnc de JN1DFF, BSD-3), à l'identique pour que l'appli
// et l'ESP décodent de la même façon. Chaîne : suppression de la composante
// continue -> détection de porteuse -> passe-bande FIR 900-2500 Hz -> limiteur
// -> discriminateur à retard et multiplication -> passe-bas -> trois trancheurs
// décalés (PLL de bit façon Dire Wolf, NRZI, HDLC) -> FCS, réparation d'un bit,
// réparation croisée entre trancheurs, dédoublonnage.
//
// Cadence interne FIXE 16 kHz (tables FIR générées pour 16 kHz). Le flux audio
// BLE à 8 kHz est interpolé x2 : à 8 kHz (6,7 échantillons par bit) le
// trancheur se trompe sur ~11 % des échantillons et aucune trame ne passe,
// alors que l'interpolation redonne 5/5 trames sur les fichiers d'essai
// (2026-10-31). Entrée : PCM 16 bits signé (8 ou 16 kHz), ramené au domaine
// « ADC 12 bits » du firmware (2048 ± 2048) pour garder les mêmes seuils.
public sealed class AprsRx
{
    public const int Fs = 16000;
    const int MaxFrame = 330, Slicers = 3;
    const int DelayUs = 446, CdtOn = 1200, CdtOff = 400, SliceMin = 24, LimMag = 6000;

    static readonly short[] Bpf16k =
    {
        13, -29, 0, 156, 294, -42, -1223, -2844, -3588, -2149, 1416, 5255, 6911, 5255, 1416, -2149,
        -3588, -2844, -1223, -42, 294, 156, 0, -29, 13,
    };
    static readonly short[] Lpf16k = { -60, 65, 640, 2061, 4153, 6087, 6876, 6087, 4153, 2061, 640, 65, -60 };

    sealed class Fir
    {
        readonly short[] _a; readonly int[] _x; int _idx;
        public Fir(short[] a) { _a = a; _x = new int[a.Length]; }
        public int Run(int v)
        {
            _x[_idx] = v;
            if (++_idx >= _a.Length) _idx = 0;
            long acc = 0; int k = _idx;
            for (int i = 0; i < _a.Length; i++) { acc += (long)_a[i] * _x[k]; if (++k >= _a.Length) k = 0; }
            return (int)(acc >> 12);
        }
    }

    sealed class Chan
    {
        public int BiasNum, Bit, PVal, Nrzi, State, BitCnt, DataLen;
        public uint Pll; public byte Flags, CurByte;
        public readonly byte[] Data = new byte[MaxFrame];
        // dernier candidat (après réparation) pour la réparation croisée
        public readonly byte[] CandData = new byte[MaxFrame]; public int CandLen; public uint CandAt; public bool CandValid;
    }

    readonly Fir _bpf = new(Bpf16k), _lpf = new(Lpf16k);
    readonly int[] _delay = new int[16]; readonly int _delayN; int _delayIdx;
    readonly uint _pllStep;
    readonly Chan[] _ch = new Chan[Slicers];
    int _dc = 2048 << 8, _cdtLvl, _lpHi, _lpLo, _env; bool _cdt;
    readonly byte[] _last = new byte[MaxFrame]; int _lastLen; uint _lastAt, _now;
    int _inRate = 8000; int _prev; bool _havePrev;

    public event Action<byte[]>? FrameDecoded;      // trame AX.25 valide, sans les 2 octets de FCS
    public int Packets { get; private set; }
    public int FcsBad { get; private set; }
    public int Fixed { get; private set; }
    public int HdlcCandidates { get; private set; }
    public bool Carrier => _cdt;

    public AprsRx()
    {
        int d = (Fs * DelayUs + 500000) / 1000000; _delayN = Math.Clamp(d, 1, 15);
        _pllStep = (uint)(((ulong)1 << 32) * 1200 / Fs);
        int[] bias = { -3, 0, 3 };
        for (int i = 0; i < Slicers; i++) _ch[i] = new Chan { BiasNum = bias[i] };
    }

    // Cadence du flux d'entrée (8000 : interpolé x2 ; 16000 : direct).
    public int InputRate { get => _inRate; set { _inRate = value == 16000 ? 16000 : 8000; _havePrev = false; } }

    public void Push(ReadOnlySpan<short> pcm)
    {
        foreach (short s in pcm)
        {
            int adc = 2048 + (s >> 4);
            if (_inRate == 8000)
            {
                if (_havePrev) Sample((_prev + adc) >> 1); else _havePrev = true;
                _prev = adc;
            }
            Sample(adc);
        }
    }

    public void Sample(int adc12)
    {
        int raw = adc12 & 0x0FFF;
        _now++;
        _dc += ((raw << 8) - _dc) >> 9;
        int s = raw - (_dc >> 8);

        _cdtLvl += ((s * s) - _cdtLvl) >> 7;
        if (!_cdt && _cdtLvl > CdtOn) _cdt = true;
        else if (_cdt && _cdtLvl < CdtOff)
        {
            _cdt = false; _lpHi = _lpLo = 0;
            foreach (var c in _ch) c.State = 0;
            return;
        }
        if (!_cdt) return;

        int v = _bpf.Run(s);
        v = v > 0 ? LimMag : v < 0 ? -LimMag : 0;
        int d = _delay[_delayIdx]; _delay[_delayIdx] = v;
        if (++_delayIdx >= _delayN) _delayIdx = 0;
        int m = (v >> 4) * (d >> 4);
        int lp = _lpf.Run(m);

        if (lp > _lpHi) _lpHi += (lp - _lpHi) >> 2; else _lpHi -= (_lpHi - _lpLo) >> 10;
        if (lp < _lpLo) _lpLo += (lp - _lpLo) >> 2; else _lpLo += (_lpHi - _lpLo) >> 10;
        int lpc = lp - ((_lpHi + _lpLo) >> 1);
        int mag = lpc < 0 ? -lpc : lpc;
        _env += (mag - _env) >> 6;
        int th = _env >> 2; if (th < SliceMin) th = SliceMin;

        foreach (var c in _ch)
        {
            int x = lpc - ((_env * c.BiasNum) >> 3);
            if (x > th) c.Bit = 0; else if (x < -th) c.Bit = 1;
            int prevSign = (int)c.Pll >> 31;
            unchecked { c.Pll += _pllStep; }
            if (((int)c.Pll >> 31) < prevSign) { HdlcBit(c, c.Bit == c.Nrzi ? 1 : 0); c.Nrzi = c.Bit; }
            if (c.Bit != c.PVal) { unchecked { c.Pll -= (uint)((int)c.Pll >> 2); } c.PVal = c.Bit; }
        }
    }

    void HdlcBit(Chan c, int bit)
    {
        c.Flags = (byte)((c.Flags << 1) | (bit & 1));
        if (c.State == 0)
        {
            if (c.Flags == 0x7E) { c.State = 1; c.DataLen = 0; c.BitCnt = 0; c.CurByte = 0; }
            return;
        }
        if ((c.Flags & 0x3F) == 0x3F) { Output(c); c.State = 0; return; }
        if ((c.Flags & 0x3F) == 0x3E) return;
        c.CurByte = (byte)((c.CurByte >> 1) | ((bit & 1) << 7));
        if (++c.BitCnt >= 8)
        {
            if (c.DataLen < MaxFrame) c.Data[c.DataLen++] = c.CurByte; else c.State = 0;
            c.BitCnt = 0;
        }
    }

    public static ushort FcsResidue(ReadOnlySpan<byte> d, int len)
    {
        ushort crc = 0xFFFF;
        for (int i = 0; i < len; i++)
        {
            crc ^= d[i];
            for (int b = 0; b < 8; b++) crc = (crc & 1) != 0 ? (ushort)((crc >> 1) ^ 0x8408) : (ushort)(crc >> 1);
        }
        return (ushort)(crc ^ 0xFFFF);
    }

    static bool LooksLikeUi(ReadOnlySpan<byte> d, int len)
    {
        int pos = 6;
        for (int blk = 0; blk < 10; blk++)
        {
            if (pos >= len) return false;
            if ((d[pos] & 1) != 0) return pos + 3 <= len && d[pos + 1] == 0x03 && d[pos + 2] == 0xF0;
            pos += 7;
        }
        return false;
    }

    bool TryFixCross(Chan chain, byte[] d, int len)
    {
        const int MaxDiff = 5;
        foreach (var o in _ch)
        {
            if (o == chain || !o.CandValid || o.CandLen != len) continue;
            if (_now - o.CandAt > (uint)(Fs / 20)) continue;
            var diff = new int[MaxDiff]; int nd = 0; bool tooMany = false;
            for (int i = 0; i < len; i++)
            {
                if (d[i] == o.CandData[i]) continue;
                if (nd == MaxDiff) { tooMany = true; break; }
                diff[nd++] = i;
            }
            if (tooMany || nd == 0) continue;
            var merged = new byte[len]; Array.Copy(d, merged, len);
            for (uint mask = 1; mask < (1u << nd); mask++)
            {
                for (int k = 0; k < nd; k++) merged[diff[k]] = (mask & (1u << k)) != 0 ? o.CandData[diff[k]] : d[diff[k]];
                if (FcsResidue(merged, len) == 0x0F47 && LooksLikeUi(merged, len - 2)) { Array.Copy(merged, d, len); return true; }
            }
        }
        return false;
    }

    void Output(Chan c)
    {
        int len = c.DataLen;
        if (len < 18) { if (len > 4) HdlcCandidates++; return; }
        if (len > MaxFrame) { HdlcCandidates++; return; }
        HdlcCandidates++;
        var d = c.Data;
        bool ok = FcsResidue(d, len) == 0x0F47;
        if (!ok)
        {
            for (int i = 0; i < len && !ok; i++)
                for (int b = 0; b < 8 && !ok; b++)
                {
                    d[i] ^= (byte)(1 << b);
                    if (FcsResidue(d, len) == 0x0F47 && LooksLikeUi(d, len - 2)) ok = true;
                    else d[i] ^= (byte)(1 << b);
                }
            if (!ok) ok = TryFixCross(c, d, len);
            if (ok) Fixed++;
        }
        c.CandLen = len; c.CandAt = _now; c.CandValid = true; Array.Copy(d, c.CandData, len);
        if (!ok) { FcsBad++; return; }

        int plen = len - 2;
        if (_lastLen == plen && _now - _lastAt < (uint)(Fs * 3 / 2) && _last.AsSpan(0, plen).SequenceEqual(d.AsSpan(0, plen))) return;
        Array.Copy(d, _last, plen); _lastLen = plen; _lastAt = _now;
        Packets++;
        var frame = new byte[plen]; Array.Copy(d, frame, plen);
        FrameDecoded?.Invoke(frame);
    }
}
