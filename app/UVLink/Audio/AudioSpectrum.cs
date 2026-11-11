namespace UVLink.Audio;

// Spectre de l'audio reçu (PCM 8 kHz décodé) : FFT 1024 points (fenêtre de
// Hann) sur les derniers échantillons, 512 raies de 0 à 4 kHz en dB sous la
// pleine échelle. Alimenté par AudioEngine (paquets reçus, silence compris),
// lu par SpectrumView à ~20 images/s.
public sealed class AudioSpectrum
{
    public const int N = 1024, Bins = N / 2;
    private readonly short[] _ring = new short[4096];
    private int _w;
    private long _pushed;
    private readonly object _lock = new();
    private readonly float[] _re = new float[N], _im = new float[N], _win = new float[N];
    private readonly int[] _rev = new int[N];
    private readonly float[] _cos = new float[N / 2], _sin = new float[N / 2];
    private long _computedAt = -1;

    public int SampleRate { get; }
    public DateTime LastPushUtc { get; private set; }
    public AudioSpectrum(int sampleRate)
    {
        SampleRate = sampleRate;
        for (int i = 0; i < N; i++) _win[i] = (float)(0.5 - 0.5 * Math.Cos(2 * Math.PI * i / (N - 1)));
        int bits = (int)Math.Log2(N);
        for (int i = 0; i < N; i++) { int r = 0; for (int b = 0; b < bits; b++) if ((i >> b & 1) != 0) r |= 1 << (bits - 1 - b); _rev[i] = r; }
        for (int i = 0; i < N / 2; i++) { _cos[i] = (float)Math.Cos(2 * Math.PI * i / N); _sin[i] = (float)-Math.Sin(2 * Math.PI * i / N); }
    }

    public void Push(ReadOnlySpan<short> pcm)
    {
        lock (_lock)
        {
            foreach (var s in pcm) { _ring[_w] = s; _w = (_w + 1) & (_ring.Length - 1); }
            _pushed += pcm.Length;
            LastPushUtc = DateTime.UtcNow;
        }
    }

    // Remplit db[0..Bins-1] ; renvoie false si rien de nouveau depuis le dernier calcul.
    public bool Compute(float[] db)
    {
        lock (_lock)
        {
            if (_pushed == _computedAt || _pushed < N) return false;
            _computedAt = _pushed;
            int start = (_w - N) & (_ring.Length - 1);
            for (int i = 0; i < N; i++) { _re[_rev[i]] = _ring[(start + i) & (_ring.Length - 1)] * _win[i] / 32768f; _im[_rev[i]] = 0; }
        }
        for (int len = 2; len <= N; len <<= 1)
        {
            int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; j++)
                {
                    float c = _cos[j * step], s = _sin[j * step];
                    int a = i + j, b = a + half;
                    float tr = _re[b] * c - _im[b] * s, ti = _re[b] * s + _im[b] * c;
                    _re[b] = _re[a] - tr; _im[b] = _im[a] - ti; _re[a] += tr; _im[a] += ti;
                }
        }
        const float scale = 2f / (N * 0.5f);   // fenêtre de Hann : gain cohérent 0,5
        for (int k = 0; k < Bins; k++)
        {
            float mag = MathF.Sqrt(_re[k] * _re[k] + _im[k] * _im[k]) * scale;
            db[k] = 20f * MathF.Log10(mag + 1e-7f);
        }
        return true;
    }
}
