using System.Threading.Channels;

namespace UVLink.Audio;

// Moteur audio de l'appli :
//   poste -> appli : paquets AUDIO_RX -> ADPCM -> tampon de gigue -> haut-parleur
//   appli -> poste : micro -> blocs de 20 ms -> ADPCM -> AUDIO_TX (pendant le PTT)
// Les périphériques viennent des fabriques fournies par la plateforme
// (null = pas d'audio sur cette plateforme : on décode quand même pour le vumètre).
public sealed class AudioEngine : IDisposable
{
    // Cadence annoncée par le firmware dans chaque paquet (flags b2-b3) : 8 kHz
    // par défaut (NBFM : 3 kHz de bande), 320 échantillons = 40 ms = 25 paquets/s.
    public int SampleRate { get; private set; } = 8000;
    public const int PacketSamples = 320;

    private readonly Func<IAudioOutput?> _outputFactory;
    private readonly Func<IAudioInput?> _inputFactory;
    private readonly ImaAdpcm _dec = new();
    private readonly ImaAdpcm _enc = new();

    // lecture
    private IAudioOutput? _out;
    private Thread? _playThread;
    private readonly Queue<short[]> _jitter = new();
    private readonly object _lock = new();
    private volatile bool _playing;
    private bool _primed;

    // capture
    private IAudioInput? _in;
    private readonly short[] _micBuf = new short[PacketSamples];
    private int _micFill;
    private byte _txSeq;
    private bool _txFirst;
    private Channel<byte[]>? _txChannel;
    private Task? _txPump;
    private Func<byte[], Task>? _send;

    private int _rxPackets, _rxPacketsLast;
    private DateTime _rateT0 = DateTime.UtcNow;

    public float Volume { get; set; } = 0.8f;
    public float MicGain { get; set; } = 1.0f;
    public AudioSpectrum Spectrum { get; } = new(8000);   // spectre de l'audio reçu (SpectrumView), flux 8 kHz
    // Décodeur APRS de l'appli sur l'audio BLE (2026-10-31) : même chaîne que
    // l'ESP (Aprs/AprsRx.cs), second avis sur chaque trame. Événement sur le thread BLE.
    public Aprs.AprsRx Aprs { get; } = new();
    public bool AprsEnabled { get; set; } = true;
    public event Action<byte[]>? AprsFrame;
    // Décodeur POCSAG de l'appli (2026-11-10) : même chaîne que l'ESP (Pocsag/PocsagRx.cs)
    // sur l'audio BLE 8 kHz ; 512 / 1200 Bd fiables, 2400 Bd limite. Événement sur le thread BLE.
    public Pocsag.PocsagRx Pocsag { get; } = new(8000);
    public bool PocsagEnabled { get; set; } = true;
    public event Action<Pocsag.PocsagMessage>? PocsagMessage;
    // AGC d'émission (2026-10-30, téléphone : le micro Android sort un niveau très
    // faible, la jauge TX bougeait à peine alors que l'ESP recevait tous les
    // paquets). Vise une crête de 60 %, gain 1..24 : montée lente (x1,08 par bloc
    // de 40 ms, seulement au-dessus du souffle), descente immédiate en cas d'écrêtage.
    public bool TxAgc { get; set; } = true;
    private float _txAgcGain = 1f;
    public float TxAgcGain => _txAgcGain;
    public int RxLevel { get; private set; }        // 0..100 (crête du dernier paquet reçu)
    public int TxLevel { get; private set; }
    public bool IsPlaying => _playing;
    public bool IsCapturing => _in != null;
    public bool HasOutput { get; }
    public bool HasInput { get; }
    public event Action<string>? Log;

    public AudioEngine(Func<IAudioOutput?> outputFactory, Func<IAudioInput?> inputFactory)
    {
        _outputFactory = outputFactory; _inputFactory = inputFactory;
        Aprs.FrameDecoded += f => AprsFrame?.Invoke(f);
        Pocsag.MessageDecoded += m => PocsagMessage?.Invoke(m);
        // On sonde une fois pour savoir si la plateforme a de l'audio.
        using (var o = outputFactory()) HasOutput = o != null;
        using (var i = inputFactory()) HasInput = i != null;
    }

    // ---------------- lecture ----------------
    public void StartPlayback()
    {
        if (_playing) return;
        _out = _outputFactory();
        if (_out == null) { Log?.Invoke("audio : pas de sortie sur cette plateforme"); return; }
        try { _out.Start(SampleRate); }
        catch (Exception e) { Log?.Invoke("audio : sortie indisponible : " + e.Message); _out.Dispose(); _out = null; return; }
        lock (_lock) { _jitter.Clear(); _primed = false; }
        _playing = true;
        _playThread = new Thread(PlayLoop) { IsBackground = true, Name = "uvlink-play" };
        _playThread.Start();
        Log?.Invoke($"audio : lecture démarrée ({SampleRate} Hz)");
    }

    public void StopPlayback()
    {
        if (!_playing) return;
        _playing = false;
        _playThread?.Join(500);
        try { _out?.Stop(); _out?.Dispose(); } catch { }
        _out = null;
        lock (_lock) _jitter.Clear();
        Log?.Invoke("audio : lecture arrêtée");
    }

    // Paquet AUDIO_RX reçu (thread BLE).
    public void OnPacket(byte[] pkt)
    {
        if (pkt.Length < ImaAdpcm.HeaderSize) return;
        int rate = ((pkt[1] >> 2) & 3) == 1 ? 8000 : 16000;
        if (rate != SampleRate)
        {
            bool was = _playing;
            if (was) StopPlayback();
            SampleRate = rate;
            Log?.Invoke($"audio : cadence {rate} Hz");
            if (was) StartPlayback();
        }
        var pcm = _dec.DecodePacket(pkt);
        int peak = 0;
        foreach (var s in pcm) { int a = Math.Abs((int)s); if (a > peak) peak = a; }
        RxLevel = peak * 100 / 32767;
        _rxPackets++;
        Spectrum.Push(pcm);                                 // onglet SPECTRE, écoute active ou non
        if (AprsEnabled)
        {
            if (Aprs.InputRate != rate) Aprs.InputRate = rate;
            Aprs.Push(pcm);
        }
        if (PocsagEnabled)
        {
            if ((int)Pocsag.InputRate != rate) Pocsag.InputRate = rate;
            Pocsag.Push(pcm);
        }
        if (!_playing) return;
        lock (_lock)
        {
            if ((pkt[1] & ImaAdpcm.FlagStart) != 0) { _jitter.Clear(); _primed = false; }
            _jitter.Enqueue(pcm);
            while (_jitter.Count > 12) _jitter.Dequeue();     // > 240 ms de retard : on rattrape
        }
    }

    public double RxPacketsPerSecond()
    {
        var now = DateTime.UtcNow;
        double dt = (now - _rateT0).TotalSeconds;
        if (dt < 0.5) return -1;
        double r = (_rxPackets - _rxPacketsLast) / dt;
        _rxPacketsLast = _rxPackets; _rateT0 = now;
        return r;
    }

    private void PlayLoop()
    {
        var silence = new short[PacketSamples];
        var scaled = new short[PacketSamples];
        while (_playing)
        {
            short[]? block = null;
            lock (_lock)
            {
                if (!_primed && _jitter.Count >= 3) _primed = true;   // 60 ms d'avance avant de démarrer
                if (_primed && _jitter.Count > 0) block = _jitter.Dequeue();
                else if (_primed && _jitter.Count == 0) _primed = false;
            }
            try
            {
                if (block == null) _out!.Write(silence);
                else
                {
                    float v = Volume;
                    int n = Math.Min(block.Length, scaled.Length);
                    for (int i = 0; i < n; i++) scaled[i] = (short)Math.Clamp(block[i] * v, -32768, 32767);
                    _out!.Write(scaled.AsSpan(0, n));
                }
            }
            catch (Exception e) { Log?.Invoke("audio : sortie : " + e.Message); _playing = false; }
        }
    }

    // ---------------- capture (PTT) ----------------
    public void StartCapture(Func<byte[], Task> send)
    {
        if (_in != null) return;
        _in = _inputFactory();
        if (_in == null) { Log?.Invoke("audio : pas de micro sur cette plateforme"); return; }
        _send = send;
        _micFill = 0; _txFirst = true; _enc.Reset();
        _txChannel = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(6) { FullMode = BoundedChannelFullMode.DropOldest });
        var ch = _txChannel;
        _txPump = Task.Run(async () =>
        {
            await foreach (var p in ch.Reader.ReadAllAsync())
            {
                try { await send(p); }
                catch (Exception e) { Log?.Invoke("audio : envoi : " + e.Message); }
            }
        });
        try { _in.Start(SampleRate, PacketSamples, OnMicBlock); }
        catch (Exception e) { Log?.Invoke("audio : micro indisponible : " + e.Message); _in.Dispose(); _in = null; ch.Writer.TryComplete(); return; }
        Log?.Invoke($"audio : micro ouvert ({_in})");
    }

    public async Task StopCaptureAsync()
    {
        var i = _in;
        if (i == null) return;
        _in = null;
        try { i.Stop(); i.Dispose(); } catch { }
        // dernier paquet (éventuellement partiel, complété de silence) avec le drapeau fin
        Array.Clear(_micBuf, _micFill, PacketSamples - _micFill);
        var last = _enc.EncodePacket(_txSeq++, (byte)((_txFirst ? ImaAdpcm.FlagStart : 0) | ImaAdpcm.FlagEnd | (SampleRate == 8000 ? 4 : 0)), _micBuf);
        _txChannel?.Writer.TryWrite(last);
        _txChannel?.Writer.TryComplete();
        if (_txPump != null) { try { await _txPump.WaitAsync(TimeSpan.FromSeconds(1)); } catch { } }
        _txChannel = null; _txPump = null;
        TxLevel = 0;
        Log?.Invoke("audio : micro fermé");
    }

    private void OnMicBlock(short[] block)
    {
        float g = MicGain;
        if (TxAgc)
        {
            int raw = 0;
            foreach (var s in block) { int a = Math.Abs((int)s); if (a > raw) raw = a; }
            float outPk = raw * g * _txAgcGain;
            if (outPk > 32767 * 0.95f) _txAgcGain = Math.Max(1f, 32767 * 0.95f / (raw * g));
            else if (raw > 300 && outPk < 32767 * 0.6f) _txAgcGain = Math.Min(24f, _txAgcGain * 1.08f);
            g *= _txAgcGain;
        }
        else _txAgcGain = 1f;
        int peak = 0;
        foreach (var s in block)
        {
            short v = g == 1f ? s : (short)Math.Clamp(s * g, -32768, 32767);
            _micBuf[_micFill++] = v;
            int a = Math.Abs((int)v); if (a > peak) peak = a;
            if (_micFill == PacketSamples)
            {
                byte flags = (byte)((_txFirst ? ImaAdpcm.FlagStart : 0) | (SampleRate == 8000 ? 4 : 0));
                var pkt = _enc.EncodePacket(_txSeq++, flags, _micBuf);
                _txFirst = false; _micFill = 0;
                _txChannel?.Writer.TryWrite(pkt);
            }
        }
        TxLevel = peak * 100 / 32767;
    }

    public void Dispose()
    {
        StopPlayback();
        _ = StopCaptureAsync();
    }
}
