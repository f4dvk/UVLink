using System.Runtime.InteropServices;
using UVLink.Audio;

namespace UVLink.Desktop;

// Audio Linux par ALSA (libasound2, toujours présent sur un bureau Ubuntu ;
// le périphérique "default" passe par PipeWire / PulseAudio). API "simple" :
// snd_pcm_set_params + writei / readi bloquants.
internal static class Alsa
{
    private const string Lib = "libasound.so.2";
    public const int StreamPlayback = 0, StreamCapture = 1;
    public const int FormatS16Le = 2, AccessRwInterleaved = 3;

    [DllImport(Lib)] public static extern int snd_pcm_open(out IntPtr pcm, string name, int stream, int mode);
    [DllImport(Lib)] public static extern int snd_pcm_set_params(IntPtr pcm, int format, int access, uint channels, uint rate, int softResample, uint latencyUs);
    [DllImport(Lib)] public static extern nint snd_pcm_writei(IntPtr pcm, short[] buffer, nuint frames);
    [DllImport(Lib)] public static extern nint snd_pcm_readi(IntPtr pcm, short[] buffer, nuint frames);
    [DllImport(Lib)] public static extern int snd_pcm_recover(IntPtr pcm, int err, int silent);
    [DllImport(Lib)] public static extern int snd_pcm_drop(IntPtr pcm);
    [DllImport(Lib)] public static extern int snd_pcm_close(IntPtr pcm);
    [DllImport(Lib)] public static extern IntPtr snd_strerror(int err);

    public static string Err(int e) => Marshal.PtrToStringAnsi(snd_strerror(e)) ?? e.ToString();

    public static bool Available()
    {
        try { snd_strerror(0); return true; } catch { return false; }
    }

    public static IntPtr Open(int stream, int rate, uint latencyUs)
    {
        int r = snd_pcm_open(out var pcm, "default", stream, 0);
        if (r < 0) throw new InvalidOperationException("ALSA open : " + Err(r));
        r = snd_pcm_set_params(pcm, FormatS16Le, AccessRwInterleaved, 1, (uint)rate, 1, latencyUs);
        if (r < 0) { snd_pcm_close(pcm); throw new InvalidOperationException("ALSA set_params : " + Err(r)); }
        return pcm;
    }
}

public sealed class AlsaOutput : IAudioOutput
{
    private IntPtr _pcm;

    public void Start(int sampleRate) => _pcm = Alsa.Open(Alsa.StreamPlayback, sampleRate, 120_000);

    public void Write(ReadOnlySpan<short> pcm)
    {
        if (_pcm == IntPtr.Zero) return;
        var buf = pcm.ToArray();
        int off = 0;
        while (off < buf.Length)
        {
            var chunk = off == 0 ? buf : buf[off..];
            nint n = Alsa.snd_pcm_writei(_pcm, chunk, (nuint)chunk.Length);
            if (n < 0)
            {
                int r = Alsa.snd_pcm_recover(_pcm, (int)n, 1);
                if (r < 0) throw new InvalidOperationException("ALSA write : " + Alsa.Err(r));
                continue;
            }
            off += (int)n;
        }
    }

    public void Stop()
    {
        if (_pcm == IntPtr.Zero) return;
        Alsa.snd_pcm_drop(_pcm);
        Alsa.snd_pcm_close(_pcm);
        _pcm = IntPtr.Zero;
    }

    public void Dispose() => Stop();
}

public sealed class AlsaInput : IAudioInput
{
    private IntPtr _pcm;
    private Thread? _thread;
    private volatile bool _run;

    public void Start(int sampleRate, int blockSamples, Action<short[]> onBlock)
    {
        _pcm = Alsa.Open(Alsa.StreamCapture, sampleRate, 80_000);
        _run = true;
        _thread = new Thread(() =>
        {
            while (_run)
            {
                var buf = new short[blockSamples];
                int off = 0;
                while (_run && off < blockSamples)
                {
                    var tmp = off == 0 ? buf : new short[blockSamples - off];
                    nint n = Alsa.snd_pcm_readi(_pcm, tmp, (nuint)tmp.Length);
                    if (n < 0) { if (Alsa.snd_pcm_recover(_pcm, (int)n, 1) < 0) { _run = false; break; } continue; }
                    if (off != 0) Array.Copy(tmp, 0, buf, off, (int)n);
                    off += (int)n;
                }
                if (_run) onBlock(buf);
            }
        }) { IsBackground = true, Name = "uvlink-mic" };
        _thread.Start();
    }

    public void Stop()
    {
        _run = false;
        if (_pcm == IntPtr.Zero) return;
        Alsa.snd_pcm_drop(_pcm);           // débloque readi
        _thread?.Join(500);
        Alsa.snd_pcm_close(_pcm);
        _pcm = IntPtr.Zero;
    }

    public void Dispose() => Stop();
}
