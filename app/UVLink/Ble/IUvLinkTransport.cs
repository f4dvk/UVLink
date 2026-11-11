namespace UVLink.Ble;

// Transport vers le firmware : BLE en pratique, mais l'abstraction permet un
// faux transport (tests) ou un autre lien plus tard.
public interface IUvLinkTransport : IAsyncDisposable
{
    bool IsConnected { get; }
    event Action<byte[]>? ControlReceived;
    event Action<byte[]>? StatusReceived;
    event Action<byte[]>? AudioReceived;      // paquets AUDIO_RX (ADPCM)
    event Action<byte[]>? EventReceived;      // EVENT : résultats des décodeurs
    event Action<byte[]>? DataReceived;       // DATA : fragments de trames AX.25
    event Action<string>? Disconnected;
    event Action<string>? Log;

    // Cherche un UVLink, s'y connecte, abonne CONTROL + STATUS + AUDIO_RX + EVENT + DATA. Renvoie nom/adresse.
    Task<string> ConnectAsync(CancellationToken ct);
    Task DisconnectAsync();
    // Pile Bluetooth locale coincée (connexions qui échouent en boucle) :
    // réinitialise l'adaptateur si la plateforme le permet. Renvoie false sinon.
    Task<bool> RecoverAsync() => Task.FromResult(false);
    Task WriteControlAsync(byte[] data, CancellationToken ct);
    // AUDIO_TX, écriture sans réponse (un paquet de 40 ms).
    Task WriteAudioAsync(byte[] data, CancellationToken ct);
    // DATA, écriture sans réponse (trame AX.25 à émettre, étape 5).
    Task WriteDataAsync(byte[] data, CancellationToken ct);
}
