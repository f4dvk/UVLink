namespace UVLink.Audio;

// IMA ADPCM (DVI), miroir de firmware/src/Adpcm.h : 4 bits/échantillon, nibble
// bas = premier échantillon. L'état (prédicteur + index) voyage dans chaque
// paquet BLE : un paquet perdu ne désynchronise pas le flux.
public sealed class ImaAdpcm
{
    private static readonly int[] IdxTab = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    private static readonly int[] StepTab =
    {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };

    public short Predictor { get; set; }
    public int Index { get; set; }

    public void Reset() { Predictor = 0; Index = 0; }

    public byte Encode(short sample)
    {
        int step = StepTab[Index];
        int diff = sample - Predictor;
        int nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        int vp = step >> 3;
        if (diff >= step) { nib |= 4; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { nib |= 2; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { nib |= 1; vp += step; }
        int p = (nib & 8) != 0 ? Predictor - vp : Predictor + vp;
        Predictor = (short)Math.Clamp(p, -32768, 32767);
        Index = Math.Clamp(Index + IdxTab[nib], 0, 88);
        return (byte)nib;
    }

    public short Decode(int nib)
    {
        int step = StepTab[Index];
        int vp = step >> 3;
        if ((nib & 4) != 0) vp += step;
        if ((nib & 2) != 0) vp += step >> 1;
        if ((nib & 1) != 0) vp += step >> 2;
        int p = (nib & 8) != 0 ? Predictor - vp : Predictor + vp;
        Predictor = (short)Math.Clamp(p, -32768, 32767);
        Index = Math.Clamp(Index + IdxTab[nib & 15], 0, 88);
        return Predictor;
    }

    // Paquet AUDIO_* : seq, flags, pred:i16, idx:u8, adpcm[...]
    public const int HeaderSize = 5;
    public const byte FlagStart = 1, FlagEnd = 2;

    public byte[] EncodePacket(byte seq, byte flags, ReadOnlySpan<short> pcm)
    {
        var pkt = new byte[HeaderSize + pcm.Length / 2];
        pkt[0] = seq; pkt[1] = flags;
        pkt[2] = (byte)Predictor; pkt[3] = (byte)((ushort)Predictor >> 8);
        pkt[4] = (byte)Index;
        for (int i = 0, k = HeaderSize; i + 1 < pcm.Length; i += 2)
            pkt[k++] = (byte)(Encode(pcm[i]) | (Encode(pcm[i + 1]) << 4));
        return pkt;
    }

    public short[] DecodePacket(ReadOnlySpan<byte> pkt)
    {
        if (pkt.Length < HeaderSize) return Array.Empty<short>();
        Predictor = (short)(pkt[2] | (pkt[3] << 8));
        Index = Math.Min((int)pkt[4], 88);
        var pcm = new short[(pkt.Length - HeaderSize) * 2];
        for (int i = HeaderSize, k = 0; i < pkt.Length; i++)
        {
            pcm[k++] = Decode(pkt[i] & 15);
            pcm[k++] = Decode(pkt[i] >> 4);
        }
        return pcm;
    }
}
