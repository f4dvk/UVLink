using Android.Media;
using UVLink.Audio;
using Encoding = Android.Media.Encoding;

namespace UVLink.Android;

// Audio du téléphone pour AudioEngine (mono 16 bits, 8 kHz) : AudioTrack en
// lecture (haut-parleur, usage « média »), AudioRecord au micro. Même contrat
// que l'ALSA du bureau : Write() bloque le temps que le périphérique absorbe
// les échantillons (c'est ce qui cadence le flux), le micro livre des blocs de
// blockSamples depuis un thread dédié.
public sealed class AndroidOutput : IAudioOutput
{
    private AudioTrack? _track;

    public void Start(int sampleRate)
    {
        int min = AudioTrack.GetMinBufferSize(sampleRate, ChannelOut.Mono, Encoding.Pcm16bit);
        int bytes = Math.Max(min, sampleRate * 2 * 240 / 1000);     // >= 240 ms de réserve (paquets BLE de 40 ms, gigue)
        _track = new AudioTrack.Builder()
            .SetAudioAttributes(new AudioAttributes.Builder()!.SetUsage(AudioUsageKind.Media)!.SetContentType(AudioContentType.Speech)!.Build()!)
            .SetAudioFormat(new AudioFormat.Builder()!.SetEncoding(Encoding.Pcm16bit)!.SetSampleRate(sampleRate)!.SetChannelMask(ChannelOut.Mono)!.Build()!)
            .SetBufferSizeInBytes(bytes)
            .SetTransferMode(AudioTrackMode.Stream)
            .Build();
        _track.Play();
    }

    public void Write(ReadOnlySpan<short> pcm)
    {
        var t = _track;
        if (t == null) return;
        var buf = pcm.ToArray();
        int off = 0;
        while (off < buf.Length)
        {
            int n = t.Write(buf, off, buf.Length - off);          // bloquant (mode flux)
            if (n < 0) throw new InvalidOperationException("AudioTrack.Write : " + n);
            if (n == 0) Thread.Sleep(5);
            off += n;
        }
    }

    public void Stop()
    {
        var t = _track; _track = null;
        if (t == null) return;
        try { t.Pause(); t.Flush(); t.Stop(); } catch { }
        t.Release();
    }

    public void Dispose() => Stop();
}

public sealed class AndroidInput : IAudioInput
{
    public string Source { get; private set; } = "";
    public override string ToString() => "AudioRecord " + Source;
    private AudioRecord? _rec;
    private Thread? _thread;
    private volatile bool _run;

    public void Start(int sampleRate, int blockSamples, Action<short[]> onBlock)
    {
        int min = AudioRecord.GetMinBufferSize(sampleRate, ChannelIn.Mono, Encoding.Pcm16bit);
        int bytes = Math.Max(min, sampleRate * 2 * 160 / 1000);
        // Micro brut d'abord : la source « communication vocale » (annulation d'écho,
        // AGC du téléphone) sortait un niveau très faible (2026-10-30, 252 paquets
        // reçus par l'ESP mais jauge TX quasi nulle). L'AGC d'émission de l'appli
        // fait le reste. Repli sur VoiceCommunication si Mic est refusé.
        foreach (var src in new[] { AudioSource.Mic, AudioSource.VoiceCommunication })
        {
            var r = new AudioRecord(src, sampleRate, ChannelIn.Mono, Encoding.Pcm16bit, bytes);
            if (r.State == State.Initialized) { _rec = r; Source = src.ToString(); break; }
            r.Release();
        }
        if (_rec == null) throw new InvalidOperationException("micro indisponible (permission RECORD_AUDIO refusée, ou 8 kHz mono non pris en charge)");
        _rec.StartRecording();
        _run = true;
        var rec = _rec;
        _thread = new Thread(() =>
        {
            while (_run)
            {
                var buf = new short[blockSamples];
                int off = 0;
                while (_run && off < blockSamples)
                {
                    int n = rec.Read(buf, off, blockSamples - off);  // bloquant
                    if (n < 0) { _run = false; break; }
                    off += n;
                }
                if (_run) onBlock(buf);
            }
        }) { IsBackground = true, Name = "uvlink-mic" };
        _thread.Start();
    }

    public void Stop()
    {
        _run = false;
        var r = _rec; _rec = null;
        if (r == null) return;
        try { r.Stop(); } catch { }                 // débloque Read
        _thread?.Join(500);
        r.Release();
    }

    public void Dispose() => Stop();
}
