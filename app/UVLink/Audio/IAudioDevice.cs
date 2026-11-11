namespace UVLink.Audio;

// Périphériques audio du PC / téléphone, mono 16 bits. Implémentations :
// ALSA (UVLink.Desktop, Linux), NAudio (UVLink.Desktop, Windows),
// AudioTrack / AudioRecord (UVLink.Android, AndroidAudio.cs).
public interface IAudioOutput : IDisposable
{
    void Start(int sampleRate);
    // Bloque le temps que le périphérique absorbe les échantillons (cadence le flux).
    void Write(ReadOnlySpan<short> pcm);
    void Stop();
}

public interface IAudioInput : IDisposable
{
    // Appelle onBlock avec des blocs de `blockSamples` échantillons, depuis un thread dédié.
    void Start(int sampleRate, int blockSamples, Action<short[]> onBlock);
    void Stop();
}
