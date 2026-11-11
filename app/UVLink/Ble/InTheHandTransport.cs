using InTheHand.Bluetooth;
using UVLink.Protocol;

namespace UVLink.Ble;

// Transport BLE via InTheHand.BluetoothLE (32feet) : BlueZ sur Linux, WinRT sur
// Windows, natif sur Android.
public sealed class InTheHandTransport : IUvLinkTransport
{
    private BluetoothDevice? _device;
    private GattCharacteristic? _control;
    private GattCharacteristic? _status;
    private GattCharacteristic? _audioRx;
    private GattCharacteristic? _audioTx;
    private GattCharacteristic? _event;
    private GattCharacteristic? _data;

    public bool IsConnected => _device?.Gatt.IsConnected == true;
    public event Action<byte[]>? ControlReceived;
    public event Action<byte[]>? StatusReceived;
    public event Action<byte[]>? AudioReceived;
    public event Action<byte[]>? EventReceived;
    public event Action<byte[]>? DataReceived;
    public event Action<string>? Disconnected;
    public event Action<string>? Log;

    public async Task<string> ConnectAsync(CancellationToken ct)
    {
        await DisconnectAsync();
        Log?.Invoke("vérification de l'adaptateur Bluetooth…");
        if (!await Bluetooth.GetAvailabilityAsync())
            throw new InvalidOperationException("Bluetooth indisponible sur cette machine");

        Log?.Invoke("scan BLE (service UVLink)...");
        var options = new RequestDeviceOptions();
        options.Filters.Add(new BluetoothLEScanFilter { Services = { UvLinkProtocol.ServiceUuid } });
        BluetoothDevice? found = null;
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
        while (found is null && DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            var devices = await Bluetooth.ScanForDevicesAsync(options, ct);
            found = devices.FirstOrDefault(d => d.Name == "UVLink") ?? devices.FirstOrDefault();
            if (found is null) await Task.Delay(500, ct);
        }
        if (found is null) throw new TimeoutException("aucun UVLink trouvé");

        Log?.Invoke($"connexion à {found.Name} ({found.Id})");
        found.GattServerDisconnected += OnDisconnected;
        // Android (InTheHand) : si BluetoothGatt.connect() échoue, la bibliothèque
        // lève OperationCanceledException (« operation canceled ») ; et sans
        // rappel d'état la tâche ne finit jamais, d'où la borne de 20 s.
        // Observé sur téléphone (2026-10-30) : la bibliothèque dit « annulé » alors
        // que la connexion lancée en arrière-plan par connectGatt() aboutit ensuite
        // (BluetoothGatt.connect() renvoie faux pendant que l'essai précédent est en
        // cours). On attend donc l'état « connecté », et on réessaie jusqu'à 3 fois.
        for (int attempt = 1; ; attempt++)
        {
            try { await found.Gatt.ConnectAsync().WaitAsync(TimeSpan.FromSeconds(20), ct); break; }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
                var until = DateTime.UtcNow + TimeSpan.FromSeconds(6);
                while (DateTime.UtcNow < until && !found.Gatt.IsConnected) await Task.Delay(250, ct);
                if (found.Gatt.IsConnected) { Log?.Invoke($"connexion GATT établie en arrière-plan (essai {attempt})"); break; }
                if (attempt >= 3)
                    throw new InvalidOperationException("connexion GATT refusée par le téléphone après 3 essais (Bluetooth coupé, permission « appareils à proximité » refusée, appareil hors de portée, ou liaison encore tenue par un autre appareil)");
                Log?.Invoke($"connexion GATT refusée (essai {attempt}), nouvel essai dans 1 s…");
                await Task.Delay(1000, ct);
            }
            catch (TimeoutException) { throw new TimeoutException("connexion GATT sans réponse (20 s)"); }
        }
        _device = found;
        // Android négocie 23 o par défaut : 20 o utiles, soit 8 notifications par
        // paquet audio de 40 ms. On demande 185 comme BlueZ (160 o d'ADPCM + 5).
        // Ailleurs la demande est ignorée (retourne false).
        try
        {
            bool mtuOk = await found.Gatt.RequestMtuAsync(185);
            Log?.Invoke($"MTU {found.Gatt.Mtu}" + (mtuOk ? "" : " (demande 185 non prise)"));
        }
        catch (Exception e) { Log?.Invoke("MTU : " + e.Message); }

        // Android 16 + InTheHand 4.0.40 : la découverte peut ne jamais rendre la main
        // (FetchUuidsWithSdp, 32feet #467) -> bornée à 15 s.
        var svc = await found.Gatt.GetPrimaryServiceAsync(UvLinkProtocol.ServiceUuid).WaitAsync(TimeSpan.FromSeconds(15), ct)
                  ?? throw new InvalidOperationException("service UVLink absent");
        _control = await svc.GetCharacteristicAsync(UvLinkProtocol.ControlUuid)
                   ?? throw new InvalidOperationException("caractéristique CONTROL absente");
        _status = await svc.GetCharacteristicAsync(UvLinkProtocol.StatusUuid)
                  ?? throw new InvalidOperationException("caractéristique STATUS absente");

        _audioRx = await svc.GetCharacteristicAsync(UvLinkProtocol.AudioRxUuid)
                   ?? throw new InvalidOperationException("caractéristique AUDIO_RX absente");
        _audioTx = await svc.GetCharacteristicAsync(UvLinkProtocol.AudioTxUuid)
                   ?? throw new InvalidOperationException("caractéristique AUDIO_TX absente");

        _event = await svc.GetCharacteristicAsync(UvLinkProtocol.EventUuid)
                 ?? throw new InvalidOperationException("caractéristique EVENT absente");
        _data = await svc.GetCharacteristicAsync(UvLinkProtocol.DataUuid)
                ?? throw new InvalidOperationException("caractéristique DATA absente");

        _control.CharacteristicValueChanged += (_, e) => { if (e.Value is { } v) ControlReceived?.Invoke(v); };
        _status.CharacteristicValueChanged += (_, e) => { if (e.Value is { } v) StatusReceived?.Invoke(v); };
        _audioRx.CharacteristicValueChanged += (_, e) => { if (e.Value is { } v) AudioReceived?.Invoke(v); };
        _event.CharacteristicValueChanged += (_, e) => { if (e.Value is { } v) EventReceived?.Invoke(v); };
        _data.CharacteristicValueChanged += (_, e) => { if (e.Value is { } v) DataReceived?.Invoke(v); };
        await _control.StartNotificationsAsync();
        await _status.StartNotificationsAsync();
        await _audioRx.StartNotificationsAsync();
        await _event.StartNotificationsAsync();
        await _data.StartNotificationsAsync();
        Log?.Invoke("notifications CONTROL + STATUS + AUDIO_RX + EVENT + DATA actives");
        return $"{found.Name} {found.Id}";
    }

    public async Task DisconnectAsync()
    {
        var d = _device;
        _device = null; _control = null; _status = null; _audioRx = null; _audioTx = null; _event = null; _data = null;
        if (d is null) return;
        d.GattServerDisconnected -= OnDisconnected;
        try { d.Gatt.Disconnect(); } catch { /* déjà coupé */ }
        await Task.CompletedTask;
    }

    // Android n'accepte qu'une opération GATT à la fois : une écriture lancée
    // pendant qu'une autre est en cours est refusée, et la bibliothèque peut alors
    // attendre un rappel qui ne vient jamais (2026-10-30 : PTT off perdu derrière
    // le flux audio, poste resté en émission). Toutes les écritures passent par un
    // verrou et sont bornées à 3 s.
    private readonly SemaphoreSlim _gattLock = new(1, 1);
    private async Task Serialized(Func<Task> op, CancellationToken ct)
    {
        await _gattLock.WaitAsync(ct);
        try { await op().WaitAsync(TimeSpan.FromSeconds(3), ct); }
        finally { _gattLock.Release(); }
    }

    public Task WriteControlAsync(byte[] data, CancellationToken ct)
    {
        var c = _control ?? throw new InvalidOperationException("non connecté");
        return Serialized(() => c.WriteValueWithResponseAsync(data), ct);
    }

    public Task WriteAudioAsync(byte[] data, CancellationToken ct)
    {
        var c = _audioTx;
        return c is null ? Task.CompletedTask : Serialized(() => c.WriteValueWithoutResponseAsync(data), ct);
    }

    public Task WriteDataAsync(byte[] data, CancellationToken ct)
    {
        var c = _data;
        return c is null ? Task.CompletedTask : Serialized(() => c.WriteValueWithoutResponseAsync(data), ct);
    }

    private void OnDisconnected(object? sender, EventArgs e)
    {
        _device = null; _control = null; _status = null; _audioRx = null; _audioTx = null; _event = null; _data = null;
        Disconnected?.Invoke("liaison BLE coupée");
    }

    public async ValueTask DisposeAsync() => await DisconnectAsync();
}
