namespace UVLink.Pocsag;

// Décodeur POCSAG 512 / 1200 / 2400 bauds de l'appli, sur l'audio BLE (8 kHz) :
// portage C# de firmware/lib/decoders/pocsag.c (lui-même port du md_pocsag.c de
// Portsdown 4, issu du Pocsag.java de HTMOD), à l'identique pour que l'appli et
// l'ESP décodent de la même façon. Chaîne par débit : anti-continu -> moyenne
// glissante (~1/4 de bit) -> bascule à hystérésis (35 % de la crête suivie) ->
// DPLL calée sur les transitions -> intégration sur le bit -> synchro
// 0x7CD215D8 (ou inversée) -> lots de 16 mots -> BCH(31,21) jusqu'à 2 bits.
//
// À 8 kHz : 15,6 échantillons par bit à 512 Bd, 6,7 à 1200 Bd, 3,3 à 2400 Bd
// (ce dernier est limite : la NBFM coupe à 3 kHz, l'ESP à 32 kHz fait mieux).
// Il faut l'« audio plat » du poste pour 1200 / 2400 Bd.
public sealed class PocsagMessage
{
    public int Baud;
    public uint Address;
    public int Function;
    public bool Numeric, ToneOnly, Partial;
    public int Corrected, Bad;
    public string Text = "";
}

public sealed class PocsagRx
{
    const uint Sync = 0x7CD215D8u, Idle = 0x7A89C197u, Gen = 0x769u;
    const int PayloadMax = 160;
    const string NumericChars = "0123456789*U -)(";

    public event Action<PocsagMessage>? MessageDecoded;
    public int Messages { get; private set; }
    public int Syncs { get; private set; }
    public int BadWords { get; private set; }
    public int Hidden { get; private set; }
    public bool ShowPartial { get; set; }
    public int Force { get; set; }                  // 0 auto, 1 texte, 2 numérique
    public bool Synced { get { foreach (var d in _d) if (d.Synced) return true; return false; } }

    // ---------------- BCH(31,21) ----------------
    static readonly int[] SynErrors = new int[1024];
    static readonly uint[] SynMask = new uint[1024];
    static PocsagRx()
    {
        Array.Fill(SynErrors, -1);
        for (int i = 0; i < 31; i++)
        {
            uint s1 = BchRem(1u << i); SynErrors[s1] = 1; SynMask[s1] = 1u << i;
            for (int j = i + 1; j < 31; j++)
            {
                uint m = (1u << i) | (1u << j), s = BchRem(m);
                if (SynErrors[s] == -1) { SynErrors[s] = 2; SynMask[s] = m; }
            }
        }
    }
    static uint BchRem(uint v)
    {
        uint r = v;
        for (int i = 30; i >= 10; i--) if (((r >> i) & 1) != 0) r ^= Gen << (i - 10);
        return r;
    }
    static int Pop(uint v) => System.Numerics.BitOperations.PopCount(v);
    // mot corrigé, nombre de bits corrigés, ou -1 si plus de 2 erreurs
    static int BchCorrect(uint cw, out uint fixedCw)
    {
        fixedCw = 0;
        uint c31 = cw >> 1, s = BchRem(c31), mask;
        if (s == 0) mask = 0;
        else if (SynErrors[s] == -1) return -1;
        else mask = SynMask[s];
        uint f = c31 ^ mask, parity = (uint)(Pop(f) & 1);
        int errors = Pop(mask) + (parity != (cw & 1) ? 1 : 0);
        if (errors > 2) return -1;
        fixedCw = (f << 1) | parity;
        return errors;
    }

    // ---------------- messages ----------------
    static uint Rev(uint v, int n) { uint r = 0; for (int i = 0; i < n; i++) if (((v >> i) & 1) != 0) r |= 1u << (n - 1 - i); return r; }

    static string DecodeNumeric(uint[] payload, int n)
    {
        var sb = new System.Text.StringBuilder(n * 5);
        for (int i = 0; i < n; i++) for (int k = 4; k >= 0; k--) sb.Append(NumericChars[(int)Rev((payload[i] >> (k * 4)) & 0xF, 4)]);
        return sb.ToString().Trim(' ', '\n', '\t', '\r');
    }
    // texte 7 bits (poids faible en tête) ; caractères de commande inattendus -> '\x7f' (comptés comme douteux, rendus '?')
    static string DecodeAlpha(uint[] payload, int n)
    {
        var sb = new System.Text.StringBuilder(n * 3);
        uint acc = 0; int nbits = 0;
        for (int i = 0; i < n; i++)
            for (int b = 19; b >= 0; b--)
            {
                acc = (acc << 1) | ((payload[i] >> b) & 1);
                if (++nbits < 7) continue;
                uint c = Rev(acc, 7); acc = 0; nbits = 0;
                if (c == 10) sb.Append('\n');
                else if (c >= 32 && c <= 126) sb.Append((char)c);
                else if (c != 0 && c != 3 && c != 4 && c != 0x17 && c != 13) sb.Append('\x7f');
            }
        return sb.ToString().TrimEnd(' ', '\n', '\t', '\r');
    }
    static bool GuessNumeric(int function, string num, string alpha)
    {
        if (alpha.Length == 0) return true;
        int odd = 0, bad = 0;
        foreach (char c in num) if ("0123456789 -".IndexOf(c) < 0) odd++;
        foreach (char c in alpha) if (c == '\x7f') bad++;
        double numOdd = odd / (double)Math.Max(1, num.Length), alphaOdd = bad / (double)alpha.Length;
        if (numOdd < alphaOdd) return true;
        if (numOdd > alphaOdd) return false;
        return function == 0 && num.Length <= 16;
    }

    void Emit(Demod d)
    {
        bool numeric = false, partial = false; string text = "";
        if (d.NPayload > 0)
        {
            string num = DecodeNumeric(d.Payload, d.NPayload), alpha = DecodeAlpha(d.Payload, d.NPayload);
            numeric = Force == 2 || (Force == 0 && GuessNumeric(d.Function, num, alpha));
            if (d.Bad > 0 && !numeric)
            {
                int q = 0; foreach (char c in alpha) if (c == '\x7f') q++;
                if (q * 4 >= alpha.Length) partial = true;       // résidu de fin de rafale : texte illisible
            }
            text = (numeric ? num : alpha).Replace('\x7f', '?');
        }
        if (partial && !ShowPartial) { Hidden++; return; }
        Messages++;
        MessageDecoded?.Invoke(new PocsagMessage
        {
            Baud = d.Baud, Address = d.Address, Function = d.Function, Numeric = numeric, ToneOnly = d.NPayload == 0,
            Partial = partial, Corrected = Math.Min(255, d.Corrected), Bad = Math.Min(255, d.Bad), Text = text,
        });
    }

    // ---------------- démodulateur (un par débit) ----------------
    sealed class Demod
    {
        public int Baud; public float Step; public int MaLen; public float[] Ring = Array.Empty<float>(); public int RingIdx;
        public float MaSum, PeakDecay, Peak; public bool State; public float Phase; public int Integ;
        public uint Shift; public bool Synced, Inverted; public int BitsInCw, CwIndex, BadInBatch;
        public bool Building; public uint Address; public int Function; public readonly uint[] Payload = new uint[PayloadMax];
        public int NPayload, Corrected, Bad;
        public void Init(float rate, int baud)
        {
            float spb = rate / baud;
            Baud = baud; Step = 1f / spb; MaLen = Math.Max(1, (int)(spb / 4 + 0.5f)); Ring = new float[MaLen];
            RingIdx = 0; MaSum = 0; Peak = 0; State = false; Phase = 0; Integ = 0; Shift = 0; Synced = false; Building = false;
            PeakDecay = MathF.Exp(-1f / (spb * 32));
        }
    }

    readonly Demod[] _d;
    float _dcX, _dcY;
    float _rate;

    public PocsagRx(float rate = 8000, int baudMask = 7)
    {
        var list = new List<Demod>();
        int[] bauds = { 512, 1200, 2400 };
        if ((baudMask & 7) == 0) baudMask = 7;
        for (int i = 0; i < 3; i++) if ((baudMask & (1 << i)) != 0) { var d = new Demod(); d.Init(rate, bauds[i]); list.Add(d); }
        _d = list.ToArray(); _rate = rate;
    }
    public float InputRate
    {
        get => _rate;
        set { if (value == _rate) return; _rate = value; foreach (var d in _d) d.Init(value, d.Baud); _dcX = _dcY = 0; }
    }

    public void Push(ReadOnlySpan<short> pcm) { foreach (short s in pcm) Sample(s); }

    public void Sample(float x)
    {
        float y = x - _dcX + 0.999f * _dcY;             // passe-haut (~1,3 Hz à 8 kHz)
        _dcX = x; _dcY = y;
        foreach (var d in _d) Sample(d, y);
    }

    void Sample(Demod d, float x)
    {
        d.MaSum += x - d.Ring[d.RingIdx];
        d.Ring[d.RingIdx] = x;
        if (++d.RingIdx >= d.MaLen) d.RingIdx = 0;
        float v = d.MaSum / d.MaLen, a = MathF.Abs(v);
        d.Peak = a > d.Peak ? a : d.Peak * d.PeakDecay;
        float thr = d.Peak * 0.35f;
        bool ns = d.State ? v > -thr : v > thr;
        if (ns != d.State)
        {
            float k = d.Synced ? 0.06f : 0.2f;          // transition attendue en limite de bit
            d.State = ns;
            d.Phase += d.Phase < 0.5f ? -d.Phase * k : (1f - d.Phase) * k;
        }
        d.Integ += d.State ? 1 : -1;
        d.Phase += d.Step;
        if (d.Phase >= 1f) { d.Phase -= 1f; Bit(d, d.Integ > 0); d.Integ = 0; }
    }

    void Bit(Demod d, bool b)
    {
        d.Shift = (d.Shift << 1) | (b ? 1u : 0u);
        if (!d.Synced)
        {
            if (Pop(d.Shift ^ Sync) <= 1) StartSync(d, false);
            else if (Pop(d.Shift ^ ~Sync) <= 1) StartSync(d, true);
            return;
        }
        if (++d.BitsInCw < 32) return;
        d.BitsInCw = 0;
        uint cw = d.Inverted ? ~d.Shift : d.Shift;
        if (d.CwIndex == 16)                            // entre deux lots : synchro, sinon fin de transmission
        {
            if (Pop(cw ^ Sync) <= 2) { d.CwIndex = 0; d.BadInBatch = 0; } else LoseSync(d);
            return;
        }
        Codeword(d, cw, d.CwIndex);
        d.CwIndex++;
    }

    void StartSync(Demod d, bool inv) { d.Synced = true; d.Inverted = inv; d.BitsInCw = 0; d.CwIndex = 0; d.BadInBatch = 0; Syncs++; }
    void LoseSync(Demod d) { Finish(d); d.Synced = false; d.CwIndex = 0; d.BitsInCw = 0; }
    void Finish(Demod d) { if (!d.Building) return; d.Building = false; Emit(d); }
    static void AddPayload(Demod d, uint w) { if (d.NPayload < PayloadMax) d.Payload[d.NPayload++] = w; }

    void Codeword(Demod d, uint raw, int index)
    {
        int n = BchCorrect(raw, out uint cw);
        if (n < 0)
        {
            BadWords++;
            if (++d.BadInBatch >= 6) { LoseSync(d); return; }       // trop d'erreurs : bruit
            if (d.Building) { d.Bad++; AddPayload(d, (raw >> 11) & 0xFFFFF); }
            return;
        }
        if (cw == Idle) { Finish(d); return; }
        if ((cw & 0x80000000u) == 0)                    // mot d'adresse
        {
            Finish(d);
            d.Building = true;
            d.Address = (((cw >> 13) & 0x3FFFF) << 3) | (uint)(index / 2);
            d.Function = (int)((cw >> 11) & 3);
            d.NPayload = 0; d.Corrected = n; d.Bad = 0;
        }
        else if (d.Building) { d.Corrected += n; AddPayload(d, (cw >> 11) & 0xFFFFF); }
    }

    public void Flush() { foreach (var d in _d) LoseSync(d); }
}
