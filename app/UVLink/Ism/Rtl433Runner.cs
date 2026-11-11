using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using UVLink.Protocol;

namespace UVLink.Ism;

// ISM 433/868 : l'ESP envoie les impulsions OOK (porteuse / silence, en µs)
// lues sur le souffle du discriminateur (EVENT ISM, fw ≥ 0.14). Ici on recolle
// les fragments, on écrit les paquets au format « .ook » de rtl_433 et on lance
// rtl_433 (installé sur le PC) toutes les secondes sur les nouveaux paquets :
// ses décodeurs OOK (capteurs météo, télécommandes...) s'appliquent tels quels.
// Tous les paquets sont aussi journalisés dans Documents/UVLink/ism/AAAAMMJJ.ook
// pour analyse hors ligne (`rtl_433 -r fichier.ook -F json`).
// Expérimental : seules les porteuses assez fortes pour éteindre le souffle, et
// les impulsions plus longues que la réaction de la FI du poste, ressortent.
public sealed class IsmDecode
{
    public DateTime Time = DateTime.Now;
    public string Model = "", Id = "", Summary = "", Json = "";
}

public sealed class Rtl433Runner : IDisposable
{
    public event Action<IsmDecode>? Decoded;
    public event Action? Changed;
    public int Packets { get; private set; }
    public int Pulses { get; private set; }
    public int Runs { get; private set; }
    public int Decodes { get; private set; }
    public bool Remotes { get; set; }                 // mode télécommandes (sous-ensemble + décodeurs flex)
    public string? Exe { get; private set; }         // chemin de rtl_433, null = absent
    // Android : rtl_433 est embarqué dans l'APK comme « bibliothèque » native
    // (lib/<abi>/librtl_433.so, en fait un exécutable statique construit par
    // tools/build_rtl433_android.sh) ; MainActivity renseigne ici son chemin dans
    // nativeLibraryDir, seul emplacement d'où Android ≥ 10 accepte d'exécuter un fichier.
    public static string? PlatformExe;
    public string Status => Exe == null
        ? (OperatingSystem.IsAndroid() ? "rtl_433 absent de cet APK : impulsions journalisées seulement" : "rtl_433 introuvable (installer le paquet rtl-433) : impulsions journalisées seulement")
        : $"rtl_433 : {Runs} passages, {Decodes} décodages";
    public string LogPath { get; private set; } = "";

    private readonly Dictionary<int, List<(ushort, ushort)>> _frags = new();
    private readonly List<List<(ushort, ushort)>> _pending = new();
    private readonly object _lock = new();
    private readonly System.Threading.Timer _timer;
    private string? _remoteArgs;
    private bool _busy;

    public Rtl433Runner()
    {
        Exe = Find();
        _timer = new System.Threading.Timer(_ => Tick(), null, 1000, 1000);
    }

    private static string? Find()
    {
        if (OperatingSystem.IsAndroid() || OperatingSystem.IsIOS()) return PlatformExe != null && File.Exists(PlatformExe) ? PlatformExe : null;
        string name = OperatingSystem.IsWindows() ? "rtl_433.exe" : "rtl_433";
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
            try { var f = Path.Combine(dir, name); if (File.Exists(f)) return f; } catch { }
        foreach (var f in new[] { "/usr/local/bin/rtl_433", "/usr/bin/rtl_433", "/opt/homebrew/bin/rtl_433" }) if (File.Exists(f)) return f;
        return null;
    }

    public void Add(IsmEvent e)
    {
        lock (_lock)
        {
            if (!_frags.TryGetValue(e.Id, out var list)) _frags[e.Id] = list = new();
            if (e.Frag == 0) list.Clear();
            list.AddRange(e.Pairs);
            if (!e.Last) return;
            _frags.Remove(e.Id);
            Packets++; Pulses += list.Count;
            _pending.Add(list);
            AppendLog(list);
        }
        Changed?.Invoke();
    }

    private void AppendLog(List<(ushort, ushort)> pk)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "UVLink", "ism");
            Directory.CreateDirectory(dir);
            LogPath = Path.Combine(dir, DateTime.Now.ToString("yyyyMMdd") + ".ook");
            bool fresh = !File.Exists(LogPath);
            using var w = new StreamWriter(LogPath, true);
            if (fresh) w.Write(Header());
            w.WriteLine($";ook  {DateTime.Now:HH:mm:ss}");
            foreach (var (p, g) in pk) w.WriteLine($"{p} {g}");
        }
        catch { /* journal facultatif */ }
    }

    private static string Header() => ";pulse data\n;version 1\n;timescale 1us\n;samplerate 32000\n;freq1 433920000\n";

    private void Tick()
    {
        List<List<(ushort, ushort)>> batch;
        lock (_lock)
        {
            if (_busy || _pending.Count == 0 || Exe == null) return;
            batch = new(_pending); _pending.Clear(); _busy = true;
        }
        try { Run(batch); }
        finally { _busy = false; }
    }

    private void Run(List<List<(ushort, ushort)>> batch)
    {
        var sb = new StringBuilder(Header());
        foreach (var pk in batch) { sb.Append(";ook\n"); foreach (var (p, g) in pk) sb.Append(p).Append(' ').Append(g).Append('\n'); }
        sb.Append(";end\n");
        string file = Path.Combine(Path.GetTempPath(), $"uvlink_{Environment.ProcessId}.ook");
        File.WriteAllText(file, sb.ToString());
        string args = $"-r \"{file}\" -F json -M time:iso" + (Remotes ? " " + RemoteArgs() : "");
        var psi = new ProcessStartInfo(Exe!, args) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
        using var proc = Process.Start(psi);
        if (proc == null) return;
        string output = proc.StandardOutput.ReadToEnd();
        proc.WaitForExit(10000);
        Runs++;
        foreach (var line in output.Split('\n'))
        {
            if (!line.TrimStart().StartsWith("{")) continue;
            var d = Parse(line.Trim());
            if (d == null) continue;
            Decodes++;
            Decoded?.Invoke(d);
        }
        Changed?.Invoke();
    }

    // Mode télécommandes : les protocoles de rtl_433 dont le nom évoque une télécommande (liste relue
    // depuis `rtl_433 -R help`, donc valable quelle que soit la version), plus les codes fixes en
    // décodeurs « flex » comme dans Portsdown 4 (CAME 320/640, Nice FLO 700/1400, Holtek HT12 430/870).
    private string RemoteArgs()
    {
        if (_remoteArgs != null) return _remoteArgs;
        var ids = new List<int>();
        try
        {
            var psi = new ProcessStartInfo(Exe!, "-R help") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
            using var proc = Process.Start(psi);
            string help = (proc?.StandardOutput.ReadToEnd() ?? "") + (proc?.StandardError.ReadToEnd() ?? "");
            proc?.WaitForExit(5000);
            string[] keys = { "remote", "keeloq", "somfy", "nice", "came", "security+", "megacode", "doorbell", "car key", "keyfob", "garage", "gate", "shutter", "curtain", "hcs200", "ev1527", "silvercrest", "ht680", "akhan", "rojaflex", "markisol", "io-homecontrol", "proflame", "honda", "ford", "hyundai", "linear", "chamberlain", "hormann", "faac" };
            string[] excl = { "sensor", "thermo", "temperature", "weather", "meter" };
            foreach (var l in help.Split('\n'))
            {
                var m = System.Text.RegularExpressions.Regex.Match(l, @"\[(\d+)\]\*?\s+(.*)");
                if (!m.Success) continue;
                string name = m.Groups[2].Value.ToLowerInvariant();
                if (keys.Any(k => name.Contains(k)) && !excl.Any(k => name.Contains(k))) ids.Add(int.Parse(m.Groups[1].Value));
            }
        }
        catch { }
        var sb = new StringBuilder();
        foreach (var id in ids) sb.Append("-R ").Append(id).Append(' ');
        sb.Append("-X \"n=CAME,m=OOK_PWM,s=320,l=640,r=3000,g=1200,t=100,bits>=12,bits<=24,invert=1\" ");
        sb.Append("-X \"n=Nice-FLO,m=OOK_PWM,s=700,l=1400,r=5000,g=2500,t=150,bits>=12,bits<=12,invert=1\" ");
        sb.Append("-X \"n=Holtek-HT12,m=OOK_PWM,s=430,l=870,r=3000,g=1500,t=100,bits>=12,bits<=13,invert=1\"");
        return _remoteArgs = sb.ToString();
    }

    private static IsmDecode? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var d = new IsmDecode { Json = json };
            if (root.TryGetProperty("model", out var model)) d.Model = model.ToString();
            var parts = new List<string>();
            foreach (var pr in root.EnumerateObject())
            {
                string k = pr.Name; string v = pr.Value.ValueKind == JsonValueKind.String ? pr.Value.GetString() ?? "" : pr.Value.ToString();
                switch (k)
                {
                    case "time": case "model": case "mic": case "protocol": break;
                    case "id": d.Id = v; break;
                    case "channel": parts.Add($"ch {v}"); break;
                    case "temperature_C": parts.Add(Num(v, 1) + " °C"); break;
                    case "temperature_F": parts.Add(Num(v, 1) + " °F"); break;
                    case "humidity": parts.Add(v + " %"); break;
                    case "battery_ok": parts.Add(v == "1" || v == "True" ? "pile OK" : "pile faible"); break;
                    case "wind_avg_km_h": parts.Add("vent " + Num(v, 1) + " km/h"); break;
                    case "wind_max_km_h": parts.Add("rafales " + Num(v, 1) + " km/h"); break;
                    case "wind_dir_deg": parts.Add("dir " + v + "°"); break;
                    case "rain_mm": parts.Add("pluie " + Num(v, 1) + " mm"); break;
                    case "pressure_hPa": parts.Add(Num(v, 1) + " hPa"); break;
                    case "code": parts.Add("code " + v); break;
                    case "button": parts.Add("bouton " + v); break;
                    case "counter": parts.Add("compteur " + v); break;
                    case "data": parts.Add("données " + v); break;
                    case "codes": parts.Add("codes " + v); break;
                    default: parts.Add(k + " " + v); break;
                }
            }
            d.Summary = string.Join("   ", parts);
            return d;
        }
        catch { return null; }
    }
    private static string Num(string v, int dec) => double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var x) ? x.ToString("F" + dec, CultureInfo.CurrentCulture) : v;

    public void Clear() { lock (_lock) { _pending.Clear(); _frags.Clear(); Packets = Pulses = Runs = Decodes = 0; } Changed?.Invoke(); }
    public void Dispose() => _timer.Dispose();
}
