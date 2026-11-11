using System.Diagnostics;
using Linux.Bluetooth;
using Linux.Bluetooth.Extensions;
using UVLink.Ble;
using UVLink.Protocol;

namespace UVLink.Desktop;

// Transport BLE Linux par BlueZ (D-Bus) via Linux.Bluetooth. Le fournisseur
// Linux d'InTheHand.BluetoothLE repose sur la même lib mais son scan ne
// trouvait pas la carte ; ici on pilote BlueZ directement.
//
// Cycle de vie des abonnements (2026-10-28) : Linux.Bluetooth garde ses objets
// Device / GattCharacteristic (et leurs veilleurs D-Bus) tant qu'on ne les
// libère pas. À chaque reconnexion on empilait donc un gestionnaire de plus
// sur Value et Disconnected : après 3 coupures, chaque paquet audio arrivait
// 4 fois (77 puis 103 paquets/s dans le journal) et la lecture hachait. On
// retire maintenant les gestionnaires et on libère les objets à chaque coupure.
public sealed class BlueZTransport : IUvLinkTransport
{
    private static readonly string Svc = UvLinkProtocol.ServiceUuid.ToString();
    private Adapter? _adapter;
    private Device? _device;
    private GattCharacteristic? _control;
    private GattCharacteristic? _status;
    private GattCharacteristic? _audioRx;
    private GattCharacteristic? _audioTx;
    private GattCharacteristic? _event;
    private GattCharacteristic? _data;
    private bool _connected;
    private static readonly Dictionary<string, object> WriteReq = new() { ["type"] = "request" };
    private static readonly Dictionary<string, object> WriteCmd = new() { ["type"] = "command" };

    // Gestionnaires nommés pour pouvoir les retirer.
    private Task OnControlValue(GattCharacteristic s, GattCharacteristicValueEventArgs e) { ControlReceived?.Invoke(e.Value); return Task.CompletedTask; }
    private Task OnStatusValue (GattCharacteristic s, GattCharacteristicValueEventArgs e) { StatusReceived?.Invoke(e.Value);  return Task.CompletedTask; }
    private Task OnAudioValue  (GattCharacteristic s, GattCharacteristicValueEventArgs e) { AudioReceived?.Invoke(e.Value);   return Task.CompletedTask; }
    private Task OnEventValue  (GattCharacteristic s, GattCharacteristicValueEventArgs e) { EventReceived?.Invoke(e.Value);   return Task.CompletedTask; }
    private Task OnDataValue   (GattCharacteristic s, GattCharacteristicValueEventArgs e) { DataReceived?.Invoke(e.Value);    return Task.CompletedTask; }

    public bool IsConnected => _connected;
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
        var adapters = await BlueZManager.GetAdaptersAsync();
        var adapter = adapters.FirstOrDefault() ?? throw new InvalidOperationException("aucun adaptateur Bluetooth (BlueZ)");
        _adapter = adapter;
        Log?.Invoke($"adaptateur {await adapter.GetAliasAsync()} ; scan BLE (service UVLink)…");

        // Quand la carte a redémarré (flash, reset) alors que BlueZ garde son
        // périphérique en cache, la découverte GATT reste bloquée
        // ("ServicesResolved" n'arrive jamais). Dans ce cas on purge le
        // périphérique côté BlueZ et on recommence par un vrai scan.
        for (int attempt = 0; ; attempt++)
        {
            var found = await FindAsync(adapter, forceScan: attempt > 0, ct);
            var name = await found.GetAliasAsync();
            var addr = await found.GetAddressAsync();
            Log?.Invoke($"connexion à {name} ({addr})");
            found.Disconnected += OnDisconnected;
            try
            {
                await found.ConnectAsync();
                await found.WaitForPropertyValueAsync("Connected", true, TimeSpan.FromSeconds(10));
                await found.WaitForPropertyValueAsync("ServicesResolved", true, TimeSpan.FromSeconds(8));
            }
            catch (Exception e) when (attempt == 0 && !ct.IsCancellationRequested)
            {
                Log?.Invoke($"liaison sans découverte GATT ({e.Message}) : purge du cache BlueZ et nouvel essai");
                found.Disconnected -= OnDisconnected;
                try { await found.DisconnectAsync(); } catch { }
                try { await adapter.RemoveDeviceAsync(found.ObjectPath); }
                catch (Exception x) { Log?.Invoke("purge impossible : " + x.Message); }
                try { found.Dispose(); } catch { }
                await Task.Delay(500, ct);
                continue;
            }
            catch
            {
                found.Disconnected -= OnDisconnected;
                try { found.Dispose(); } catch { }
                throw;
            }
            _device = found;

            try
            {
                var svc = await found.GetServiceAsync(Svc) ?? throw new InvalidOperationException("service UVLink absent");
                _control = await svc.GetCharacteristicAsync(UvLinkProtocol.ControlUuid.ToString()) ?? throw new InvalidOperationException("CONTROL absente");
                _status  = await svc.GetCharacteristicAsync(UvLinkProtocol.StatusUuid.ToString())  ?? throw new InvalidOperationException("STATUS absente");
                _audioRx = await svc.GetCharacteristicAsync(UvLinkProtocol.AudioRxUuid.ToString()) ?? throw new InvalidOperationException("AUDIO_RX absente");
                _audioTx = await svc.GetCharacteristicAsync(UvLinkProtocol.AudioTxUuid.ToString()) ?? throw new InvalidOperationException("AUDIO_TX absente");
                _event   = await svc.GetCharacteristicAsync(UvLinkProtocol.EventUuid.ToString())   ?? throw new InvalidOperationException("EVENT absente");
                _data    = await svc.GetCharacteristicAsync(UvLinkProtocol.DataUuid.ToString())    ?? throw new InvalidOperationException("DATA absente");
                _control.Value += OnControlValue;
                _status.Value  += OnStatusValue;
                _audioRx.Value += OnAudioValue;
                _event.Value   += OnEventValue;
                _data.Value    += OnDataValue;
                await _control.StartNotifyAsync();
                await _status.StartNotifyAsync();
                await _audioRx.StartNotifyAsync();
                await _event.StartNotifyAsync();
                await _data.StartNotifyAsync();
            }
            catch
            {
                await DisconnectAsync();
                throw;
            }
            _connected = true;
            Log?.Invoke("notifications CONTROL + STATUS + AUDIO_RX + EVENT + DATA actives");
            return $"{name} {addr}";
        }
    }

    private async Task<Device> FindAsync(Adapter adapter, bool forceScan, CancellationToken ct)
    {
        if (!forceScan)
            foreach (var d in await adapter.GetDevicesAsync().WaitAsync(TimeSpan.FromSeconds(5), ct))
            {
                if (await Matches(d))
                {
                    // Lien BlueZ résiduel (connecté sans nous) : tant qu'il existe, l'ESP
                    // ne publie pas et ne peut pas être rejoint -> on le coupe d'abord.
                    try
                    {
                        if (await d.GetConnectedAsync())
                        {
                            Log?.Invoke("liaison BlueZ résiduelle vers UVLink : déconnexion forcée");
                            await d.DisconnectAsync();
                            await Task.Delay(1500, ct);
                        }
                    }
                    catch (Exception e) { Log?.Invoke("déconnexion résiduelle impossible : " + e.Message); }
                    return d;
                }
                try { d.Dispose(); } catch { }
            }

        var tcs = new TaskCompletionSource<Device>(TaskCreationOptions.RunContinuationsAsynchronously);
        bool scanning = true;
        DeviceChangeEventHandlerAsync onFound = (a, e) =>
        {
            if (scanning)
                Task.Run(async () => { try { if (await Matches(e.Device)) tcs.TrySetResult(e.Device); } catch { } });
            return Task.CompletedTask;
        };
        adapter.DeviceFound += onFound;
        try
        {
            // BlueZ coincé = appels D-Bus qui ne répondent jamais : tout est borné.
            await adapter.SetDiscoveryFilterAsync(new Dictionary<string, object> { ["UUIDs"] = new[] { Svc }, ["Transport"] = "le" }).WaitAsync(TimeSpan.FromSeconds(5), ct);
            await adapter.StartDiscoveryAsync().WaitAsync(TimeSpan.FromSeconds(5), ct);
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(TimeSpan.FromSeconds(15));
            await using var reg = cts.Token.Register(() => tcs.TrySetCanceled());
            try { return await tcs.Task; }
            catch (OperationCanceledException) { throw new TimeoutException("aucun UVLink trouvé"); }
        }
        finally
        {
            scanning = false;
            adapter.DeviceFound -= onFound;
            try { await adapter.StopDiscoveryAsync().WaitAsync(TimeSpan.FromSeconds(3)); } catch { }
        }
    }

    private static async Task<bool> Matches(Device d)
    {
        try
        {
            var uuids = await d.GetUUIDsAsync();
            if (uuids != null && uuids.Any(u => string.Equals(u, Svc, StringComparison.OrdinalIgnoreCase))) return true;
            return string.Equals(await d.GetAliasAsync(), "UVLink", StringComparison.Ordinal);
        }
        catch { return false; }
    }

    // Retire les gestionnaires et libère les objets Linux.Bluetooth (veilleurs
    // D-Bus) du périphérique courant. Renvoie le Device pour un éventuel
    // DisconnectAsync explicite.
    // Les Dispose() de Linux.Bluetooth attendent la fin de leurs veilleurs D-Bus :
    // appelés depuis le thread de l'interface (chien de garde, bouton), ils
    // peuvent le bloquer (appli « figée », 2026-10-30). On retire les
    // gestionnaires tout de suite, et on libère les objets sur le pool de
    // threads, sans attendre.
    private Device? Release()
    {
        var d = _device;
        var (c, s, a, t, e, da) = (_control, _status, _audioRx, _audioTx, _event, _data);
        _device = null; _control = null; _status = null; _audioRx = null; _audioTx = null; _event = null; _data = null; _connected = false;
        if (c  != null) c.Value  -= OnControlValue;
        if (s  != null) s.Value  -= OnStatusValue;
        if (a  != null) a.Value  -= OnAudioValue;
        if (e  != null) e.Value  -= OnEventValue;
        if (da != null) da.Value -= OnDataValue;
        if (d != null) d.Disconnected -= OnDisconnected;
        _ = Task.Run(() =>
        {
            foreach (var x in new IDisposable?[] { c, s, a, t, e, da })
                try { x?.Dispose(); } catch { }
        });
        return d;
    }

    public async Task DisconnectAsync()
    {
        var d = Release();
        if (d is null) return;
        // DisconnectAsync BlueZ peut ne jamais répondre quand le contrôleur est
        // coincé : borné à 3 s, puis libération en tâche de fond.
        try { await Task.Run(() => d.DisconnectAsync()).WaitAsync(TimeSpan.FromSeconds(3)); } catch { }
        _ = Task.Run(() => { try { d.Dispose(); } catch { } });
    }

    public Task WriteControlAsync(byte[] data, CancellationToken ct)
    {
        var c = _control ?? throw new InvalidOperationException("non connecté");
        return c.WriteValueAsync(data, WriteReq);
    }

    public Task WriteAudioAsync(byte[] data, CancellationToken ct)
    {
        var c = _audioTx;
        return c is null ? Task.CompletedTask : c.WriteValueAsync(data, WriteCmd);
    }

    public Task WriteDataAsync(byte[] data, CancellationToken ct)
    {
        var c = _data;
        return c is null ? Task.CompletedTask : c.WriteValueAsync(data, WriteCmd);
    }

    private Task OnDisconnected(Device sender, BlueZEventArgs e)
    {
        if (!ReferenceEquals(sender, _device)) { sender.Disconnected -= OnDisconnected; return Task.CompletedTask; }
        var d = Release();
        if (d != null) _ = Task.Run(() => { try { d.Dispose(); } catch { } });
        Disconnected?.Invoke("liaison BLE coupée");
        return Task.CompletedTask;
    }

    // Contrôleur coincé (Intel 7265 + BlueZ après une liaison rompue : plus
    // aucun acquittement, puis « Operation already in progress » en boucle).
    // Un Powered off/on BlueZ échoue dans cet état ; seul rfkill (bloquer /
    // débloquer la radio) le remet d'aplomb. /dev/rfkill est accessible à
    // l'utilisateur de la session (ACL uaccess), pas besoin de root.
    public async Task<bool> RecoverAsync()
    {
        await DisconnectAsync();
        var adapter = _adapter;
        try
        {
            if (adapter != null)
            {
                await adapter.SetPoweredAsync(false);
                await Task.Delay(1000);
                await adapter.SetPoweredAsync(true);
                await Task.Delay(2000);
                if (await adapter.GetPoweredAsync()) { Log?.Invoke("adaptateur Bluetooth réinitialisé (power off/on)"); return true; }
            }
        }
        catch (Exception e) { Log?.Invoke("power off/on refusé (" + e.Message + ") : rfkill"); }
        try
        {
            await Rfkill("block");
            await Task.Delay(2000);
            await Rfkill("unblock");
            await Task.Delay(4000);
            Log?.Invoke("adaptateur Bluetooth réinitialisé (rfkill)");
            return true;
        }
        catch (Exception e) { Log?.Invoke("rfkill impossible : " + e.Message); return false; }
    }

    private static async Task Rfkill(string verb)
    {
        var psi = new ProcessStartInfo("rfkill", verb + " bluetooth") { UseShellExecute = false, RedirectStandardError = true };
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("rfkill introuvable");
        string err = await p.StandardError.ReadToEndAsync();
        await p.WaitForExitAsync();
        if (p.ExitCode != 0) throw new InvalidOperationException($"rfkill {verb} : {err.Trim()}");
    }

    public async ValueTask DisposeAsync() => await DisconnectAsync();
}
