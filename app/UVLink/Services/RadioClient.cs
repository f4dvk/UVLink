using UVLink.Ble;
using UVLink.Protocol;

namespace UVLink.Services;

// Couche requête/réponse au-dessus du transport : une commande à la fois,
// réponse appariée sur le 1er octet, STATUS diffusé tel quel.
public sealed class RadioClient : IAsyncDisposable
{
    private readonly IUvLinkTransport _t;
    private readonly SemaphoreSlim _lock = new(1, 1);
    private TaskCompletionSource<byte[]>? _pending;
    private byte _pendingCmd;

    public RadioStatus? Status { get; private set; }
    public bool IsConnected => _t.IsConnected;

    public event Action<RadioStatus>? StatusChanged;
    public event Action<byte[]>? AudioReceived;
    public event Action<DecoderEvent>? EventReceived;
    public event Action<byte[]>? FrameReceived;       // trame AX.25 complète (sans FCS)
    public event Action<string>? Log;
    public event Action<string>? Disconnected;
    private readonly Ax25Reassembler _ax25 = new();

    public RadioClient(IUvLinkTransport transport)
    {
        _t = transport;
        _t.ControlReceived += OnControl;
        _t.AudioReceived += p => AudioReceived?.Invoke(p);
        _t.EventReceived += d =>
        {
            var e = DecoderEventParser.Parse(d);
            if (e != null) EventReceived?.Invoke(e);
            else Log?.Invoke($"EVENT inconnu : {Convert.ToHexString(d)}");
        };
        _t.DataReceived += d => { var f = _ax25.Feed(d); if (f != null) FrameReceived?.Invoke(f); };
        _t.StatusReceived += d => { var s = UvLinkProtocol.ParseStatus(d); if (s != null) { Status = s; StatusChanged?.Invoke(s); } };
        _t.Log += m => Log?.Invoke(m);
        _t.Disconnected += m => { Status = null; Disconnected?.Invoke(m); };
    }

    public Task<string> ConnectAsync(CancellationToken ct) => _t.ConnectAsync(ct);
    public Task DisconnectAsync() => _t.DisconnectAsync();
    public Task<bool> RecoverAsync() => _t.RecoverAsync();
    public Task SendAudioAsync(byte[] pkt) => _t.IsConnected ? _t.WriteAudioAsync(pkt, CancellationToken.None) : Task.CompletedTask;

    public async Task<byte[]> SendAsync(byte[] cmd, CancellationToken ct, int timeoutMs = 1500)
    {
        await _lock.WaitAsync(ct);
        try
        {
            var tcs = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
            _pending = tcs; _pendingCmd = cmd[0];
            await _t.WriteControlAsync(cmd, ct);
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(timeoutMs);
            await using var reg = cts.Token.Register(() => tcs.TrySetException(new TimeoutException($"pas de réponse à 0x{cmd[0]:X2}")));
            return await tcs.Task;
        }
        finally { _pending = null; _lock.Release(); }
    }

    public async Task<bool> OkAsync(byte[] cmd, CancellationToken ct)
    {
        var r = await SendAsync(cmd, ct);
        return r.Length >= 2 && r[0] != UvLinkProtocol.CmdUnknown && r[1] != 0;
    }

    public async Task<(byte proto, byte major, byte minor)> PingAsync(CancellationToken ct)
    {
        var r = await SendAsync(UvLinkProtocol.Ping(), ct);
        return r.Length >= 4 ? (r[1], r[2], r[3]) : ((byte)0, (byte)0, (byte)0);
    }

    public async Task<sbyte[]?> GetRssiCorrAsync(CancellationToken ct)
        => UvLinkProtocol.ParseRssiCorr(await SendAsync(UvLinkProtocol.GetRssiCorr(), ct));

    public async Task<(bool present, byte hostProto, string fw)> GetInfoAsync(CancellationToken ct)
    {
        var r = await SendAsync(UvLinkProtocol.GetInfo(), ct);
        if (r.Length < 3) return (false, 0, "");
        return (r[1] != 0, r[2], System.Text.Encoding.ASCII.GetString(r, 3, r.Length - 3));
    }

    public async Task<VfoParams?> GetVfoAsync(byte vfo, CancellationToken ct)
        => UvLinkProtocol.ParseVfo(await SendAsync(UvLinkProtocol.GetVfo(vfo), ct));

    public async Task<GpsEvent?> GetGpsAsync(CancellationToken ct)
    {
        var r = await SendAsync(UvLinkProtocol.GetGps(), ct);
        if (r.Length < 21) return null;
        var b = new byte[r.Length]; b[0] = 6; Array.Copy(r, 1, b, 1, r.Length - 1);
        return DecoderEventParser.Parse(b) as GpsEvent;
    }

    private void OnControl(byte[] d)
    {
        var p = _pending;
        if (p != null && d.Length > 0 && (d[0] == _pendingCmd || d[0] == UvLinkProtocol.CmdUnknown))
            p.TrySetResult(d);
        else if (d.Length >= 3 && d[0] == UvLinkProtocol.CmdCalAudio)
            Log?.Invoke($"calibration audio terminée : gain fixe x{d[2]}, AGC coupé (mémorisé dans l'ESP)");
        else
            Log?.Invoke($"CONTROL non sollicité : {Convert.ToHexString(d)}");
    }

    public async ValueTask DisposeAsync() => await _t.DisposeAsync();
}
