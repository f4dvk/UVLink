using NAudio.Wave;
using UVLink.Audio;

namespace UVLink.Desktop;

// Audio Windows par NAudio (WinMM) : WaveOutEvent + tampon, WaveInEvent.
// Compilé seulement pour la cible net8.0-windows10.0.19041.0.
public sealed class NAudioOutput : IAudioOutput
{
    private WaveOutEvent? _out;
    private BufferedWaveProvider? _buf;

    public void Start(int sampleRate)
    {
        _buf = new BufferedWaveProvider(new WaveFormat(sampleRate, 16, 1))
        {
            BufferDuration = TimeSpan.FromMilliseconds(600),
            DiscardOnBufferOverflow = true,
        };
        _out = new WaveOutEvent { DesiredLatency = 120, NumberOfBuffers = 4 };
        _out.Init(_buf);
        _out.Play();
    }

    public void Write(ReadOnlySpan<short> pcm)
    {
        if (_buf == null) return;
        // on attend qu'il y ait de la place : c'est ce qui cadence le flux
        while (_buf.BufferedDuration > TimeSpan.FromMilliseconds(160)) Thread.Sleep(5);
        var bytes = new byte[pcm.Length * 2];
        Buffer.BlockCopy(pcm.ToArray(), 0, bytes, 0, bytes.Length);
        _buf.AddSamples(bytes, 0, bytes.Length);
    }

    public void Stop()
    {
        _out?.Stop(); _out?.Dispose(); _out = null; _buf = null;
    }

    public void Dispose() => Stop();
}

public sealed class NAudioInput : IAudioInput
{
    private WaveInEvent? _in;
    private short[]? _acc;
    private int _fill;

    public void Start(int sampleRate, int blockSamples, Action<short[]> onBlock)
    {
        _acc = new short[blockSamples]; _fill = 0;
        _in = new WaveInEvent { WaveFormat = new WaveFormat(sampleRate, 16, 1), BufferMilliseconds = 20 };
        _in.DataAvailable += (_, e) =>
        {
            for (int i = 0; i + 1 < e.BytesRecorded; i += 2)
            {
                _acc[_fill++] = (short)(e.Buffer[i] | (e.Buffer[i + 1] << 8));
                if (_fill == blockSamples) { onBlock((short[])_acc.Clone()); _fill = 0; }
            }
        };
        _in.StartRecording();
    }

    public void Stop()
    {
        _in?.StopRecording(); _in?.Dispose(); _in = null;
    }

    public void Dispose() => Stop();
}
