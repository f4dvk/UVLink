using System.Collections.ObjectModel;
using System.Globalization;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using UVLink.Audio;
using UVLink.Ble;
using UVLink.Map;
using UVLink.Protocol;
using UVLink.Services;

namespace UVLink.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private readonly RadioClient _radio;
    private readonly AudioEngine _audio;
    private CancellationTokenSource? _cts;
    private readonly DispatcherTimer _meterTimer;
    private int _meterTicks;

    public MainViewModel() : this(App.TransportFactory(), App.AudioOutputFactory, App.AudioInputFactory) { }

    public MainViewModel(IUvLinkTransport transport, Func<IAudioOutput?> outputFactory, Func<IAudioInput?> inputFactory)
    {
        _radio = new RadioClient(transport);
        Vfos = new[] { new VfoCard(0, OnVfoCardChanged), new VfoCard(1, OnVfoCardChanged) };
        foreach (var c in Vfos) c.SelectTx = card => SelectedVfo = card.Index;
        Vfos[0].IsTx = true;
        _radio.Log += m => Post(() => AddLog(m));
        MapController.Changed += UpdateMapCount;
        foreach (var b in RssiBands) b.Changed += OnRssiBandChanged;
        // GPS du téléphone (Android) : repli quand le module GPS de l'ESP n'a pas de fix
        _loc = App.LocationFactory();
        if (_loc != null)
        {
            _loc.Changed += (la, lo, acc) => Post(() => OnPhoneLocation(la, lo, acc));
            App.LocationPermissionGranted = () => Post(() => { if (PhoneGps) StartPhoneGps(); });
            PhoneGpsText = "position du téléphone utilisée quand le module GPS de l'ESP n'a pas de fix";
            if (Settings.GetBool("phone_gps")) PhoneGps = true;
        }
        else PhoneGpsText = "pas de localisation sur cette plateforme";
        _radio.Disconnected += m => Post(() => { IsConnected = false; ConnectionText = m; AddLog(m); OnLinkLost(); ScheduleReconnect(); });
        _radio.StatusChanged += s => Post(() => ApplyStatus(s));
        _radio.EventReceived += e => Post(() => OnDecoderEvent(e));
        _radio.FrameReceived += f => Post(() => OnFrame(f));

        _audio = new AudioEngine(outputFactory, inputFactory);
        _audio.Log += m => Post(() => AddLog(m));
        _radio.AudioReceived += p => _audio.OnPacket(p);
        _audio.AprsFrame += f => Post(() => OnAppAprsFrame(f));
        _audio.PocsagMessage += m => Post(() => OnDecoderEvent(new PocsagEvent(0, true, m.Baud, m.Address, m.Function, m.Numeric, m.ToneOnly, m.Partial, m.Corrected, m.Bad, m.Text) { Source = "appli" }));
        PocsagCharset = Math.Clamp(int.TryParse(Settings.Get("pocsag_charset"), out var pc) ? pc : 1, 0, PocsagCharsetNames.Length - 1);   // FR par défaut
        PocsagHideTone = Settings.GetBool("pocsag_hide_tone");
        PocsagAppDecoder = Settings.GetBool("pocsag_app", true);
        IsmMode = Math.Clamp(int.TryParse(Settings.Get("ism_mode"), out var im) ? im : 0, 0, 1);
        Ism.Decoded += d => Post(() => OnIsmDecoded(d));
        Ism.Changed += () => Post(UpdateIsmText);
        UpdateIsmText();
        AudioDeviceText = _audio.HasOutput ? (_audio.HasInput ? "sortie + micro" : "sortie seule") : "pas d'audio sur cette plateforme";

        _sstvImage = NewSstvBitmap(SstvWidth, SstvHeight);
        ClearSstv();

        _meterTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _meterTimer.Tick += (_, _) =>
        {
            RxLevel = _audio.RxLevel; TxLevel = _audio.TxLevel;
            if (++_aprsTextTick >= 10) { _aprsTextTick = 0; UpdateAprsAppText(); UpdatePocsagAppText(); }
            // Surveillance PASSIVE de la liaison : l'ESP envoie un STATUS au moins
            // toutes les secondes ; 15 s sans STATUS = liaison morte (pile Bluetooth
            // du PC coincée alors que le lien radio reste « connecté »). Pas de PING
            // de contrôle rapproché : un PING par seconde pendant le flux audio avait
            // été soupçonné de faire décrocher le contrôleur Intel 7265 (2026-10-30 ;
            // cause réelle : pilote btusb dégradé). Un seul PING par minute ci-dessous.
            if (++_keepAliveTicks >= 10)
            {
                _keepAliveTicks = 0;
                if (IsConnected && !IsBusy && !_pinging && _lastStatusUtc != default && (DateTime.UtcNow - _lastStatusUtc).TotalSeconds > 15)
                    _ = KeepAliveAsync();
                // Signal de vie vers l'ESP : une écriture par minute suffit à lui
                // prouver que l'appli est là (sans écriture pendant 3 min, il coupe
                // une liaison muette et republie, cf. firmware config.h). Résultat
                // ignoré : la surveillance passive ci-dessus reste seule juge.
                if (IsConnected && !IsBusy && ++_lifePingTicks >= 60)
                {
                    _lifePingTicks = 0;
                    _ = _radio.PingAsync(CancellationToken.None).ContinueWith(t => { _ = t.Exception; }, TaskContinuationOptions.OnlyOnFaulted);
                }
            }
            if (_meterTicks % 300 == 0) { MapController.Purge(); UpdateMapCount(); }   // carte : expiration toutes les 30 s
            if (++_meterTicks % 10 == 0)
            {
                var r = _audio.RxPacketsPerSecond();
                AudioRateText = !IsConnected ? "" : r < 0 ? AudioRateText : r < 1 ? "flux audio : silence" : $"flux audio : {r:F0} paquets/s";
                if (IsConnected && _meterTicks % 100 == 0) Console.WriteLine($"{DateTime.Now:HH:mm:ss} {AudioRateText} (niveau RX {RxLevel} %)");
            }
        };
        _meterTimer.Start();
    }

    // ---- audio ----
    [ObservableProperty] private bool _audioOn = true;
    [ObservableProperty] private double _volume = 80;
    [ObservableProperty] private double _micGain = 100;   // gain micro (%), appliqué aux échantillons avant l'ADPCM (10..400)
    [ObservableProperty] private int _rxLevel;
    [ObservableProperty] private int _txLevel;
    [ObservableProperty] private string _audioRateText = "";
    [ObservableProperty] private string _audioDeviceText = "";
    [ObservableProperty] private int _audioRxMode = 2;
    [ObservableProperty] private int _audioGain = 16;
    public string[] AudioRxModeNames { get; } = UvLinkProtocol.AudioRxModeNames;

    partial void OnAudioOnChanged(bool value)
    {
        if (value && IsConnected) _audio.StartPlayback(); else _audio.StopPlayback();
    }
    partial void OnVolumeChanged(double value) => _audio.Volume = (float)(value / 100.0);
    partial void OnMicGainChanged(double value) => _audio.MicGain = (float)(value / 100.0);
    [ObservableProperty] private bool _txAgcOn = true;      // AGC d'émission (micro du téléphone trop faible)
    partial void OnTxAgcOnChanged(bool value) => _audio.TxAgc = value;

    [RelayCommand]
    private Task ApplyAudioAsync()
        => RunAsync($"SET_AUDIO {AudioRxModeNames[AudioRxMode]} gain x{AudioGain} AGC {(AgcOn ? "on" : "off")}",
                    ct => _radio.OkAsync(UvLinkProtocol.SetAudio((byte)AudioRxMode, (byte)Math.Clamp(AudioGain, 1, 64), (byte)(AgcOn ? 2 : 1)), ct));

    // Calibration du gain RX : l'ESP mesure la crête pendant 3 s (parler à
    // niveau normal), fixe le gain, coupe l'AGC et mémorise. Le résultat
    // revient par un CONTROL non sollicité (journal) puis dans le STATUS.
    [RelayCommand]
    private Task CalibrateAudioAsync()
        => RunAsync("CALIBRATION audio : parle à niveau normal pendant 3 s…",
                    ct => _radio.OkAsync(UvLinkProtocol.CalibrateAudio(3), ct));

    private void OnLinkLost()
    {
        _vfoPushed = false;
        _audio.StopPlayback();
        _ = _audio.StopCaptureAsync();
        AudioRateText = "";
        LevelText = ""; _espGpsText = "GPS : —"; _espGpsValid = false; UpdateGpsText();
    }

    // ---- décodeurs ----
    public const int SstvWidth = 320, SstvHeight = 256;        // taille par défaut (Scottie, Martin, PD 50/90)
    private string _sstvModeName = "SSTV";
    [ObservableProperty] private string _levelText = "";
    [ObservableProperty] private string _levelVerdictText = "";
    [ObservableProperty] private bool _levelOk;
    [ObservableProperty] private int _levelRms;                 // 0..100, RMS / 80
    [ObservableProperty] private string _gpsText = "GPS : —";
    [ObservableProperty] private string _sstvInfoText = "en attente d'une image (Scottie 1/2/DX, Martin 1/2, PD 50 à 290 ; détection automatique en phonie)";
    [ObservableProperty] private string _myLat = "";
    [ObservableProperty] private string _myLon = "";
    [ObservableProperty] private int _selectedTab;
    public static readonly string[] TabNames = { "JOURNAL", "DÉCODEUR", "APRS", "CARTE", "RÉGLAGES", "SPECTRE", "SSTV", "POCSAG", "ISM" };
    [ObservableProperty] private string _currentTabName = TabNames[0];
    partial void OnSelectedTabChanged(int oldValue, int newValue) { CurrentTabName = newValue >= 0 && newValue < TabNames.Length ? TabNames[newValue] : ""; if (!IsFullPageTab(oldValue)) _tabBeforeMap = oldValue; }
    [RelayCommand] private void SelectTab(string? idx) { if (int.TryParse(idx, out var i)) SelectedTab = i; }
    [RelayCommand] private void SelectMode(string? idx) { if (int.TryParse(idx, out var i) && i >= 0 && i < ModeNames.Length) SelectedMode = i; }   // pastille « décodeur » de l'accueil
    // Pages plein écran CARTE / RÉGLAGES / SSTV / POCSAG (téléphone, MainView.UpdateMapPage) : RETOUR = onglet d'avant
    private static bool IsFullPageTab(int t) => t is 3 or 4 or 6 or 7 or 8;
    private int _tabBeforeMap;
    [RelayCommand] private void MapBack() => SelectedTab = IsFullPageTab(_tabBeforeMap) ? 0 : _tabBeforeMap;
    [ObservableProperty] private string _rxFreqText = "---.----";   // VFO écouté : « A 144.8000 » (bandeau de la page CARTE)

    // ---- correction RSSI dBm par bande (table dBmCorrTable du v6), mémorisée dans l'ESP ----
    public ObservableCollection<RssiBand> RssiBands { get; } =
        new(UvLinkProtocol.RssiBandNames.Select((n, i) => new RssiBand(i, n)));
    private CancellationTokenSource? _rssiDebounce;
    private void OnRssiBandChanged()
    {
        if (!AutoSend) return;
        Debounced(ref _rssiDebounce, () => RunAsync("SET_RSSI_CORR " + string.Join(" ", RssiBands.Select(b => b.Value)),
                                                     ct => _radio.OkAsync(UvLinkProtocol.SetRssiCorr(RssiBands.Select(b => (sbyte)b.Value).ToArray()), ct)), 500);
    }
    private async Task LoadRssiCorrAsync()
    {
        try
        {
            var c = await _radio.GetRssiCorrAsync(CancellationToken.None);
            if (c == null) { AddLog("correction RSSI : non lue (firmware ESP < 0.9 ?)"); return; }
            _quiet++;
            try { for (int i = 0; i < 7; i++) RssiBands[i].Value = c[i]; }
            finally { _quiet--; }
        }
        catch (Exception e) { AddLog("correction RSSI : " + e.Message); }
    }

    // ---- spectre audio (onglet SPECTRE) ----
    public AudioSpectrum Spectrum => _audio.Spectrum;
    // --demo-spectre : signal synthétique (porteuse 1 kHz + balayage + souffle) pour vérifier le rendu sans poste
    public void DemoSpectrum()
    {
        SelectedTab = 5;
        var rnd = new Random(1); double ph1 = 0, ph2 = 0, sweep = 300; int n = 0;
        var t = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(40) };
        t.Tick += (_, _) =>
        {
            var blk = new short[320];
            for (int i = 0; i < blk.Length; i++)
            {
                ph1 += 2 * Math.PI * 1000 / 8000; ph2 += 2 * Math.PI * sweep / 8000;
                blk[i] = (short)(6000 * Math.Sin(ph1) + 3000 * Math.Sin(ph2) + 400 * (rnd.NextDouble() - 0.5));
            }
            sweep += 8; if (sweep > 3800) sweep = 300;
            Spectrum.Push(blk);
            if (++n > 2000) t.Stop();
        };
        t.Start();
    }

    // ---- carte ----
    public MapController MapController { get; } = new();
    public string[] MapExpiryNames { get; } = { "15 min", "30 min", "1 h", "3 h", "12 h", "jamais" };
    private static readonly TimeSpan?[] MapExpiryValues = { TimeSpan.FromMinutes(15), TimeSpan.FromMinutes(30), TimeSpan.FromHours(1), TimeSpan.FromHours(3), TimeSpan.FromHours(12), null };
    [ObservableProperty] private int _mapExpiryIndex = 2;      // 1 h sans nouvelle position -> le marqueur s'efface
    [ObservableProperty] private bool _mapShowAprs = true;
    [ObservableProperty] private bool _mapShowSondes = true;
    [ObservableProperty] private bool _mapShowSarsat = true;
    [ObservableProperty] private string _mapCountText = "";
    partial void OnMapExpiryIndexChanged(int value) { MapController.Expiry = MapExpiryValues[Math.Clamp(value, 0, MapExpiryValues.Length - 1)]; MapController.Purge(); }
    partial void OnMapShowAprsChanged(bool value)   => MapController.ShowAprs = value;
    partial void OnMapShowSondesChanged(bool value) => MapController.ShowSondes = value;
    partial void OnMapShowSarsatChanged(bool value) => MapController.ShowSarsat = value;
    private void UpdateMapCount() => MapCountText = $"APRS {MapController.AprsCount}   sondes {MapController.SondeCount}   SARSAT {MapController.SarsatCount}";
    [RelayCommand] private void MapZoomIn()  => MapController.ZoomIn();
    [RelayCommand] private void MapZoomOut() => MapController.ZoomOut();
    [RelayCommand] private void MapFitAll()  => MapController.FitAll();
    [RelayCommand] private void MapClear()   { MapController.Clear(); MapInfoVisible = false; }

    // ---- fiche station (clic sur une épingle de la carte, façon aprs.fi) ----
    [ObservableProperty] private bool _mapInfoVisible;
    [ObservableProperty] private string _mapInfoTitle = "";
    [ObservableProperty] private string _mapInfoText = "";
    private readonly Dictionary<string, string> _rawBySrc = new();  // dernière trame TNC2 vue par indicatif (ESP ou appli)
    private void RememberRaw(string tnc2) { int i = tnc2.IndexOf('>'); if (i > 0) _rawBySrc[tnc2[..i]] = tnc2; }
    public void MapSelect(string? layer, string? key)
    {
        var m = MapController.Find(layer, key);
        if (m == null) { MapInfoVisible = false; return; }
        var sb = new System.Text.StringBuilder();
        string Ago(DateTime utc) { var d = DateTime.UtcNow - utc; return d.TotalSeconds < 60 ? $"il y a {(int)d.TotalSeconds} s" : d.TotalMinutes < 60 ? $"il y a {(int)d.TotalMinutes} min" : $"il y a {d.TotalHours:F1} h"; }
        string When(DateTime utc) => utc.ToLocalTime().ToString("dd/MM HH:mm:ss");
        if (m.Aprs is { } a)
        {
            MapInfoTitle = m.Key + (a.Adrasec ? "   ADRASEC" : "");
            sb.AppendLine($"Dernière trame : {When(m.LastSeen)} ({Ago(m.LastSeen)}), source {a.Source}");
            sb.AppendLine($"Première trame : {When(m.FirstSeen)}, {m.Count} trame{(m.Count > 1 ? "s" : "")}");
            if (a.Kind == 2 && a.Name.Length > 0) sb.AppendLine($"Objet « {a.Name} » émis par {a.Src}");
            sb.Append($"Position : {a.Lat:F5} {a.Lon:F5}");
            if (a.HasRange) sb.Append($"   {a.DistKm:F1} km au {a.BearingDeg}° de moi");
            sb.AppendLine();
            if (a.HasCourse || a.HasAlt)
                sb.AppendLine((a.HasCourse ? $"Cap {a.CourseDeg}°, {a.SpeedKmh} km/h" : "") + (a.HasCourse && a.HasAlt ? "   " : "") + (a.HasAlt ? $"altitude {a.AltM} m" : ""));
            if (a.Text.Length > 0) sb.AppendLine($"Commentaire : {a.Text}");
            if (m.Status.Length > 0) sb.AppendLine($"Statut : {m.Status}");
            sb.AppendLine(a.Via.Length == 0 ? "Chemin : direct" : $"Chemin : via {a.Via}");
            string raw = a.Raw.Length > 0 ? a.Raw : _rawBySrc.GetValueOrDefault(a.Src, "");
            if (raw.Length > 0) sb.AppendLine($"Trame : {raw}");
        }
        else
        {
            MapInfoTitle = (layer == "SARSAT" ? "Balise 406 " : "") + m.Label.Replace('\n', ' ');
            sb.AppendLine($"Dernière réception : {When(m.LastSeen)} ({Ago(m.LastSeen)})");
            sb.AppendLine($"Première réception : {When(m.FirstSeen)}, {m.Count} trame{(m.Count > 1 ? "s" : "")}");
            sb.AppendLine($"Position : {m.Lat:F5} {m.Lon:F5}");
            if (m.Detail.Length > 0) sb.AppendLine(m.Detail);
            if (m.Trail.Count > 1) sb.AppendLine($"Trajectoire : {m.Trail.Count} points");
        }
        MapInfoText = sb.ToString().TrimEnd();
        MapInfoVisible = true;
    }
    [RelayCommand] private void MapInfoClose() => MapInfoVisible = false;
    // --demo-carte : trois marqueurs fictifs (station, sonde avec trace, balise de test) autour de Laon
    // --demo-ism : les impulsions d'un fichier .ook (UVLINK_DEMO_OOK, sinon une trame Nexus-TH synthétique)
    // injectées comme si l'ESP les envoyait (EVENT ISM fragmenté) : vérifie recollage -> rtl_433 -> onglet ISM
    public void DemoIsm()
    {
        SelectedTab = 8;
        var pairs = new List<(ushort, ushort)>();
        string? path = Environment.GetEnvironmentVariable("UVLINK_DEMO_OOK");
        if (path != null && File.Exists(path))
            foreach (var l in File.ReadLines(path)) { var a = l.Split(' '); if (a.Length == 2 && ushort.TryParse(a[0], out var pu) && ushort.TryParse(a[1], out var ga)) pairs.Add((pu, ga)); }
        else
        {
            string bits = "010110101000000011010111111100110111";       // Nexus-TH id 90, canal 1, 21,5 °C, 55 %
            for (int rep = 0; rep < 12; rep++) { foreach (var b in bits) pairs.Add((500, (ushort)(b == '1' ? 2000 : 1000))); pairs.Add((500, 4000)); }
        }
        const int per = 44; int id = 7;
        for (int off = 0, idx = 0; off < pairs.Count; off += per, idx++)
        {
            int n = Math.Min(per, pairs.Count - off);
            OnDecoderEvent(new IsmEvent(idx, off + n >= pairs.Count, id, pairs.GetRange(off, n).ToArray()));
        }
    }
    public void DemoMap()
    {
        SelectedTab = 3;
        MapController.SetMyPosition(49.5641, 3.6199);
        OnDecoderEvent(new AprsEvent(1, true, true, true, true, false, '/', '>', 49.60, 3.55, 90, 45, 120, 6.2, 312, "F4DVK-9", "", "WIDE1-1", "en route")
                       { Raw = "F4DVK-9>APDR16,WIDE1-1:!4936.00N/00333.00E>090/024/A=000394 en route" });
        OnDecoderEvent(new AprsEvent(3, false, false, false, false, false, '\0', '\0', 0, 0, 0, 0, 0, 0, 0, "F4DVK-9", "", "WIDE1-1", "Mobile UVLink"));
        OnDecoderEvent(new AprsEvent(1, true, false, false, false, false, '/', '-', 49.52, 3.70, 0, 0, 0, 0, 0, "F1ABC", "", "", "QTH"));
        MapSelect("APRS", "F4DVK-9");                       // fiche station ouverte, pour la capture
        for (int i = 0; i < 12; i++)
            OnDecoderEvent(new SondeEvent(3, true, 49.70 - i * 0.004, 3.40 + i * 0.006, 8000 - i * 300, 0x9F, -21.5 + i, 60, 12.0, -5.2, 135, 10, "16:01:09", "106-2-15288"));
        OnDecoderEvent(new SarsatEvent(144, 5, true, true, 227, 49.56777, 3.61888, "1C72091A2B3FDFF", false, 0, ""));
        // poste factice (cartes, jauge batterie) : STATUS comme l'ESP l'enverrait
        ApplyStatus(new RadioStatus(true, true, false, true, false, 0, 0, false, 7, -101, 7370, 434200000, 434200000, 401000000, 401000000, 3, false, 2048, false, 64, false, 0, 1270, 300, 0));
    }
    public ObservableCollection<string> DecoderLines { get; } = new();     // "écran" texte du décodeur actif
    public ObservableCollection<string> DecodeHistory { get; } = new();    // balises SARSAT / sondes décodées
    public ObservableCollection<AprsRow> AprsPackets { get; } = new();
    public ObservableCollection<PocsagRow> PocsagMessages { get; } = new();   // onglet POCSAG (ESP + appli)
    public ObservableCollection<IsmRow> IsmRows { get; } = new();             // onglet ISM (rtl_433 sur les impulsions de l'ESP)
    public ObservableCollection<string> AprsMonitor { get; } = new();      // trames brutes TNC2
    [ObservableProperty] private WriteableBitmap _sstvImage;   // remplacée à chaque image si la taille change
    public event Action? SstvUpdated;
    private static WriteableBitmap NewSstvBitmap(int w, int h) =>
        new(new PixelSize(w, h), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Opaque);

    private void OnDecoderEvent(DecoderEvent e)
    {
        switch (e)
        {
            case LevelEvent l:
                LevelText = $"crête {l.Peak}   RMS {l.Rms}   DC {l.Dc}   ADC {l.AdcMin}..{l.AdcMax}" + (l.FloorRms > 0 ? $"   plancher {l.FloorRms}" : "") + (l.ClipPct > 0 ? $"   écrêtage {l.ClipPct} %" : "");
                LevelVerdictText = l.VerdictText; LevelOk = l.Verdict == 0;
                LevelRms = Math.Clamp(l.Rms / 80, 0, 100);
                break;
            case TextEvent t:
                if (t.Line == 255) DecoderLines.Clear();
                else
                {
                    while (DecoderLines.Count <= t.Line) DecoderLines.Add("");
                    DecoderLines[t.Line] = (t.Invert ? "▌ " : "  ") + t.Text;
                }
                break;
            case SarsatEvent s:
            {
                string pos = s.HasPosition ? $"{s.Lat:F5} {s.Lon:F5}" : "sans position";
                var line = $"SARSAT {s.HexId}  {s.ProtocolText}  pays {s.Country}  {pos}{(s.IsTest ? "  TEST" : "")}{(s.BchFixed > 0 ? $"  BCH +{s.BchFixed}" : "")}{(s.Repeat ? "  (répétition)" : "")}";
                DecodeHistory.Insert(0, $"{DateTime.Now:HH:mm:ss} {line}");
                AddLog(line);
                LastDecodeText = $"{DateTime.Now:HH:mm:ss}  {line}"; _lastDecodeTab = 1;
                MapController.Update(s);
                break;
            }
            case SondeEvent so:
            {
                var line = $"SONDE {(so.Type == 0x20 ? "M20" : so.Type != 0 ? "M10" : so.KindText)}  " + (so.HasPos ? $"{so.Lat:F5} {so.Lon:F5}  alt {so.AltM} m" : "position inconnue");
                if (so.TempC is double tc) line += $"  T {tc:F1} °C";
                if (so.RhPct is int rh) line += $"  RH {rh} %";
                if (so.SpeedH is double sh) line += $"  HS {sh:F1} m/s  VS {so.SpeedV:+0.0;-0.0} m/s  cap {so.Heading}°";
                if (so.GpsTime != null) line += $"  {so.GpsTime} UTC";
                if (so.Sats is int ns) line += $"  {ns} sat";
                if (so.Serial != null) line += $"  SN {so.Serial}";
                DecodeHistory.Insert(0, $"{DateTime.Now:HH:mm:ss} {line}");
                AddLog(line);
                LastDecodeText = $"{DateTime.Now:HH:mm:ss}  {line}"; _lastDecodeTab = 1;
                MapController.Update(so);
                break;
            }
            case AprsEvent a:
            {
                // même trame déjà vue par l'autre décodeur (ESP / appli) dans les 10 s -> une seule ligne, source « ESP+appli »
                string key = $"{a.Src}|{a.Kind}|{a.Lat:F5}|{a.Lon:F5}|{a.Name}|{a.Text}";
                var now = DateTime.Now;
                _recentAprs.RemoveAll(r => (now - r.At).TotalSeconds > 10);
                int dup = _recentAprs.FindIndex(r => r.Key == key && r.Row.Source != a.Source);
                if (dup >= 0)
                {
                    var old = _recentAprs[dup].Row;
                    int idx = AprsPackets.IndexOf(old);
                    var merged = new AprsRow(a, "ESP+appli");
                    if (idx >= 0) AprsPackets[idx] = merged;
                    _recentAprs[dup] = (key, _recentAprs[dup].At, merged);
                    AddLog($"APRS {a.Src} : aussi décodée par {(a.Source == "ESP" ? "l'ESP" : "l'appli")}");
                    break;
                }
                MapController.Update(a);
                LastDecodeText = $"{DateTime.Now:HH:mm:ss}  APRS {a.Src} {a.KindText} {(a.HasPos ? $"{a.Lat:F4} {a.Lon:F4}" : "")} {a.Text}"; _lastDecodeTab = 2;
                var row = new AprsRow(a);
                _recentAprs.Add((key, now, row));
                AprsPackets.Insert(0, row);
                while (AprsPackets.Count > 300) AprsPackets.RemoveAt(AprsPackets.Count - 1);
                AddLog($"APRS [{a.Source}] {a.Src} {a.KindText} {(a.HasPos ? $"{a.Lat:F4} {a.Lon:F4} " : "")}{a.Text}");
                break;
            }
            case SstvLineEvent sl:
                if (sl.Line == 0xFFFF)
                {
                    int w = sl.Width > 0 ? sl.Width : SstvWidth, h = sl.Height > 0 ? sl.Height : SstvHeight;
                    if (SstvImage.PixelSize.Width != w || SstvImage.PixelSize.Height != h) SstvImage = NewSstvBitmap(w, h);
                    _sstvModeName = sl.Mode > 0 ? SstvModes.Name(sl.Mode) : "Scottie 1";
                    string size = sl.ModeWidth > 0 ? $" {sl.ModeWidth}×{sl.ModeHeight}" + (sl.ModeWidth != w ? $" (affichée en {w}×{h})" : "") : "";
                    ClearSstv(); SstvInfoText = $"{_sstvModeName}{size} : nouvelle image";
                    // détection automatique en phonie (fw ≥ 0.11) : on montre l'image qui arrive
                    LastDecodeText = $"{DateTime.Now:HH:mm:ss}  SSTV {_sstvModeName} : image en réception"; _lastDecodeTab = 6;
                    if (SelectedTab != 6) { SelectedTab = 6; AddLog($"SSTV : en-tête VIS reçu ({_sstvModeName}{size}), image en cours → onglet SSTV"); }
                }
                else DrawSstvLine(sl);
                SstvUpdated?.Invoke();
                break;
            case SstvInfoEvent si:
                SstvInfoText = $"{(si.Mode > 0 ? SstvModes.Name(si.Mode) : _sstvModeName)} : {si.Lines} lignes, {si.Syncs} recalées sur leur synchro" + (si.Valid ? $", horloge {si.Ppm:+#;-#;0} ppm" : "") + (si.Final ? " — image complète" : "");
                break;
            case PocsagEvent pg:
                OnPocsag(pg);
                break;
            case IsmEvent im:
                Ism.Add(im);
                break;
            case GpsEvent g:
                _espGpsText = g.Valid ? $"GPS {g.Lat:F5} {g.Lon:F5}  alt {g.AltM} m  {g.Sats} sat  {g.UtcH:00}:{g.UtcM:00}:{g.UtcS:00} UTC"
                        : g.Seen ? $"GPS : pas de fix ({g.Sats} sat, {g.UtcH:00}:{g.UtcM:00}:{g.UtcS:00} UTC)" : "GPS : aucun module";
                if (g.Valid != _espGpsValid && PhoneGps && _phonePos != null)
                    AddLog(g.Valid ? "GPS : fix du module ESP, il reprend la main sur le téléphone" : "GPS : module ESP sans fix, position du téléphone utilisée");
                _espGpsValid = g.Valid;
                if (g.Valid) MapController.SetMyPosition(g.Lat, g.Lon);
                else if (PhoneGps && _phonePos is { } pp) UsePhonePosition(pp.Lat, pp.Lon, false);
                UpdateGpsText();
                break;
        }
    }

    private void OnFrame(byte[] f)
    {
        string tnc2 = Ax25.ToTnc2(f); RememberRaw(tnc2);
        AprsMonitor.Insert(0, $"{DateTime.Now:HH:mm:ss} [ESP]   {tnc2}");
        while (AprsMonitor.Count > 300) AprsMonitor.RemoveAt(AprsMonitor.Count - 1);
    }

    // ---- décodeur APRS de l'appli (Aprs/AprsRx.cs sur l'audio BLE) ----
    // Second décodeur, indépendant de celui de l'ESP : chacun rattrape les trames
    // que l'autre a manquées. Une trame vue par les deux n'est affichée qu'une
    // fois (dédoublonnage dans OnDecoderEvent, source « ESP+appli »).
    [ObservableProperty] private bool _aprsAppDecoder = true;
    partial void OnAprsAppDecoderChanged(bool value) { _audio.AprsEnabled = value; AddLog(value ? "décodeur APRS de l'appli actif (audio BLE)" : "décodeur APRS de l'appli coupé"); }
    [ObservableProperty] private string _aprsAppText = "";
    private void UpdateAprsAppText()
        => AprsAppText = $"décodeur appli : {_audio.Aprs.Packets} trames, {_audio.Aprs.Fixed} réparées, {_audio.Aprs.FcsBad} FCS fausses{(_audio.Aprs.Carrier ? "  •" : "")}";
    private void OnAppAprsFrame(byte[] f)
    {
        string tnc2 = Ax25.ToTnc2(f); RememberRaw(tnc2);
        AprsMonitor.Insert(0, $"{DateTime.Now:HH:mm:ss} [appli] {tnc2}");
        while (AprsMonitor.Count > 300) AprsMonitor.RemoveAt(AprsMonitor.Count - 1);
        double? lat = null, lon = null;
        if (double.TryParse(MyLat.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var la) &&
            double.TryParse(MyLon.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var lo)) { lat = la; lon = lo; }
        var e = Aprs.AprsParse.ToEvent(f, lat, lon, "appli");
        if (e != null) OnDecoderEvent(e);
    }
    private readonly List<(string Key, DateTime At, AprsRow Row)> _recentAprs = new();
    private int _aprsTextTick;

    // ---- POCSAG (onglet POCSAG) : messages de l'ESP (EVENT 0x09, fragmentés au-delà de 170 caractères)
    //      et du décodeur de l'appli (Pocsag/PocsagRx.cs sur l'audio BLE). Comme pour l'APRS, un message
    //      vu par les deux dans les 10 s n'est affiché qu'une fois (source « ESP+appli »). Le texte arrive
    //      en ASCII 7 bits tel que transmis : les caractères nationaux ($ @ [ \ ] ` { | } ~) sont
    //      traduits à l'affichage selon le jeu choisi (même table que le « Charset » de Portsdown 4). ----
    public string[] PocsagCharsetNames { get; } = { "US (ASCII)", "FR", "DE", "SE", "SI" };
    [ObservableProperty] private int _pocsagCharset = 1;
    [ObservableProperty] private bool _pocsagHideTone;          // masquer les appels sans message (bip seul)
    [ObservableProperty] private bool _pocsagAppDecoder = true;
    [ObservableProperty] private string _pocsagAppText = "";
    partial void OnPocsagCharsetChanged(int value) => Settings.Set("pocsag_charset", value.ToString());
    partial void OnPocsagHideToneChanged(bool value) => Settings.Set("pocsag_hide_tone", value);
    partial void OnPocsagAppDecoderChanged(bool value) { _audio.PocsagEnabled = value; Settings.Set("pocsag_app", value); AddLog(value ? "décodeur POCSAG de l'appli actif (audio BLE, 512/1200 Bd, 2400 limite)" : "décodeur POCSAG de l'appli coupé"); }
    // Onglet POCSAG : bascule directe du mode (le mode se règle sinon dans RÉGLAGES → DÉCODEUR)
    public bool PocsagDecoder
    {
        get => SelectedMode == 3;
        set { if (value != (SelectedMode == 3)) SelectedMode = value ? 3 : 0; }
    }
    private void UpdatePocsagAppText()
        => PocsagAppText = $"décodeur appli : {_audio.Pocsag.Messages} messages, {_audio.Pocsag.Syncs} synchros, {_audio.Pocsag.BadWords} mots incorrigibles{(_audio.Pocsag.Synced ? "  •" : "")}";
    private readonly Dictionary<string, (int Frag, PocsagEvent Head, System.Text.StringBuilder Text)> _pocsagFrags = new();
    private readonly List<(string Key, DateTime At, PocsagRow Row)> _recentPocsag = new();
    private void OnPocsag(PocsagEvent e)
    {
        // recollage des fragments de l'ESP (clé : source + adresse + débit)
        string fk = $"{e.Source}|{e.Baud}|{e.Address}";
        if (!(e.Frag == 0 && e.Last))
        {
            if (e.Frag == 0) _pocsagFrags[fk] = (0, e, new System.Text.StringBuilder(e.Text));
            else if (_pocsagFrags.TryGetValue(fk, out var f) && f.Frag + 1 == e.Frag) { f.Text.Append(e.Text); _pocsagFrags[fk] = (e.Frag, f.Head, f.Text); }
            else { _pocsagFrags.Remove(fk); AddLog($"POCSAG : fragment {e.Frag} perdu (RIC {e.Address})"); return; }
            if (!e.Last) return;
            var done = _pocsagFrags[fk]; _pocsagFrags.Remove(fk);
            e = done.Head with { Frag = 0, Last = true, Text = done.Text.ToString() };
        }
        if (e.ToneOnly && PocsagHideTone) return;
        string text = PocsagText(e.Text, PocsagCharset);
        var now = DateTime.Now;
        string key = $"{e.Baud}|{e.Address}|{e.Function}|{e.Text}";
        _recentPocsag.RemoveAll(r => (now - r.At).TotalSeconds > 10);
        int dup = _recentPocsag.FindIndex(r => r.Key == key && r.Row.Source != e.Source);
        if (dup >= 0)
        {
            var old = _recentPocsag[dup].Row;
            int idx = PocsagMessages.IndexOf(old);
            var merged = new PocsagRow(e, text, "ESP+appli");
            if (idx >= 0) PocsagMessages[idx] = merged;
            _recentPocsag[dup] = (key, _recentPocsag[dup].At, merged);
            return;
        }
        var row = new PocsagRow(e, text);
        _recentPocsag.Add((key, now, row));
        PocsagMessages.Insert(0, row);
        while (PocsagMessages.Count > 500) PocsagMessages.RemoveAt(PocsagMessages.Count - 1);
        LastDecodeText = $"{now:HH:mm:ss}  POCSAG RIC {e.Address:D7}  {(e.ToneOnly ? "(bip seul)" : text)}"; _lastDecodeTab = 7;
        AddLog($"POCSAG [{e.Source}] {row.Head} : {row.Body}");
    }
    // Jeux de caractères nationaux (CCIR 584 / Portsdown « Charset ») : positions $ @ [ \ ] ^ ` { | } ~
    private static readonly (char from, string to)[][] PocsagCharsets =
    {
        Array.Empty<(char, string)>(),
        new[] { ('$', "£"), ('@', "à"), ('[', "°"), ('\\', "ç"), (']', "§"), ('`', "µ"), ('{', "é"), ('|', "ù"), ('}', "è"), ('~', "¨") },
        new[] { ('[', "Ä"), ('\\', "Ö"), (']', "Ü"), ('{', "ä"), ('|', "ö"), ('}', "ü"), ('~', "ß") },
        new[] { ('[', "Ä"), ('\\', "Ö"), (']', "Å"), ('{', "ä"), ('|', "ö"), ('}', "å") },
        new[] { ('@', "Ž"), ('[', "Š"), ('\\', "Đ"), (']', "Ć"), ('^', "Č"), ('`', "ž"), ('{', "š"), ('|', "đ"), ('}', "ć"), ('~', "č") },
    };
    public static string PocsagText(string ascii, int charset)
    {
        if (charset <= 0 || charset >= PocsagCharsets.Length) return ascii;
        var sb = new System.Text.StringBuilder(ascii.Length);
        foreach (char c in ascii)
        {
            string? r = null;
            foreach (var (from, to) in PocsagCharsets[charset]) if (c == from) { r = to; break; }
            if (r != null) sb.Append(r); else sb.Append(c);
        }
        return sb.ToString();
    }
    [RelayCommand] private void ClearPocsag() { PocsagMessages.Clear(); _recentPocsag.Clear(); _pocsagFrags.Clear(); }

    // ---- ISM 433/868 (fw ≥ 0.14) : impulsions OOK de l'ESP -> rtl_433 (Ism/Rtl433Runner.cs) ----
    public Ism.Rtl433Runner Ism { get; } = new();
    public string[] IsmModeNames { get; } = { "Capteurs (tous les décodeurs)", "Télécommandes" };
    [ObservableProperty] private int _ismMode;
    [ObservableProperty] private string _ismText = "";
    partial void OnIsmModeChanged(int value) { Ism.Remotes = value == 1; Settings.Set("ism_mode", value.ToString()); }
    public bool IsmDecoder
    {
        get => SelectedMode == 4;
        set { if (value != (SelectedMode == 4)) SelectedMode = value ? 4 : 0; }
    }
    private void UpdateIsmText()
        => IsmText = $"{Ism.Packets} rafale{(Ism.Packets > 1 ? "s" : "")}, {Ism.Pulses} impulsions   {Ism.Status}" + (Ism.LogPath.Length > 0 ? $"   journal {Ism.LogPath}" : "");
    private void OnIsmDecoded(Ism.IsmDecode d)
    {
        var row = new IsmRow(d);
        IsmRows.Insert(0, row);
        while (IsmRows.Count > 300) IsmRows.RemoveAt(IsmRows.Count - 1);
        AddLog($"ISM {d.Model} {d.Id} {d.Summary}");
        LastDecodeText = $"{d.Time:HH:mm:ss}  ISM {d.Model} {d.Summary}"; _lastDecodeTab = 8;
    }
    [RelayCommand] private void ClearIsm() { IsmRows.Clear(); Ism.Clear(); UpdateIsmText(); }

    // ---- GPS du téléphone (ILocationService, Android) : repli quand le module GPS de l'ESP n'a pas de fix.
    //      Priorité au module dès qu'il a un fix ; sinon la position du téléphone remplit « Ma position »,
    //      part en SET_POS (si déplacement > 30 m) et place le marqueur sur la carte. ----
    private readonly ILocationService? _loc;
    public bool PhoneGpsAvailable => _loc != null;
    [ObservableProperty] private bool _phoneGps;
    [ObservableProperty] private string _phoneGpsText = "";
    private string _espGpsText = "GPS : —";
    private bool _espGpsValid;
    private (double Lat, double Lon)? _phonePos;
    private double _phoneAcc = -1;
    private (double Lat, double Lon)? _sentPos;

    partial void OnPhoneGpsChanged(bool value)
    {
        Settings.Set("phone_gps", value);
        if (value) StartPhoneGps();
        else { _loc?.Stop(); _phonePos = null; _sentPos = null; PhoneGpsText = "position du téléphone utilisée quand le module GPS de l'ESP n'a pas de fix"; if (!_espGpsValid) MapController.SetMyPosition(null, null); UpdateGpsText(); }
    }
    private void StartPhoneGps()
    {
        if (_loc == null) return;
        var err = _loc.Start();
        PhoneGpsText = err ?? "téléphone : en attente d'une position…";
        AddLog("GPS téléphone : " + (err ?? "démarré"));
    }
    private void OnPhoneLocation(double lat, double lon, double acc)
    {
        bool first = _phonePos == null;
        _phonePos = (lat, lon); _phoneAcc = acc;
        PhoneGpsText = $"téléphone : {lat:F5} {lon:F5}" + (acc >= 0 ? $" (±{acc:F0} m)" : "");
        if (first) AddLog("GPS téléphone : " + PhoneGpsText + (_espGpsValid ? " (module ESP prioritaire, il a un fix)" : ""));
        if (!_espGpsValid) UsePhonePosition(lat, lon, true);
        UpdateGpsText();
    }
    private void UsePhonePosition(double lat, double lon, bool send)
    {
        MapController.SetMyPosition(lat, lon);
        if (_sentPos is { } s && DistanceM(s.Lat, s.Lon, lat, lon) < 30) return;   // pas de SET_POS pour quelques mètres
        _sentPos = (lat, lon);
        _quiet++;
        try { MyLat = lat.ToString("F5", CultureInfo.InvariantCulture); MyLon = lon.ToString("F5", CultureInfo.InvariantCulture); }
        finally { _quiet--; }
        if (send && IsConnected) _ = ApplyPosAsync();
    }
    private void UpdateGpsText()
        => GpsText = !_espGpsValid && PhoneGps && _phonePos != null ? _espGpsText + " — " + PhoneGpsText : _espGpsText;
    private static double DistanceM(double lat1, double lon1, double lat2, double lon2)
    {
        double dlat = (lat2 - lat1) * Math.PI / 180, dlon = (lon2 - lon1) * Math.PI / 180;
        double a = Math.Sin(dlat / 2) * Math.Sin(dlat / 2) + Math.Cos(lat1 * Math.PI / 180) * Math.Cos(lat2 * Math.PI / 180) * Math.Sin(dlon / 2) * Math.Sin(dlon / 2);
        return 6371000 * 2 * Math.Atan2(Math.Sqrt(a), Math.Sqrt(1 - a));
    }

    private void ClearSstv()
    {
        using var fb = SstvImage.Lock();
        var row = new byte[fb.RowBytes];
        for (int i = 3; i < row.Length; i += 4) row[i] = 255;     // noir opaque (alpha 0 = fond blanc à l'écran)
        for (int y = 0; y < fb.Size.Height; y++) Marshal.Copy(row, 0, fb.Address + y * fb.RowBytes, row.Length);
    }

    private void DrawSstvLine(SstvLineEvent sl)
    {
        int w = SstvImage.PixelSize.Width, h = SstvImage.PixelSize.Height;
        if (sl.Line >= h || sl.X0 >= w) return;
        int n = Math.Min(sl.Rgb565.Length / 2, w - sl.X0);
        var px = new byte[n * 4];
        for (int i = 0; i < n; i++)
        {
            int v = sl.Rgb565[i * 2] | (sl.Rgb565[i * 2 + 1] << 8);
            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            px[i * 4]     = (byte)((b << 3) | (b >> 2));
            px[i * 4 + 1] = (byte)((g << 2) | (g >> 4));
            px[i * 4 + 2] = (byte)((r << 3) | (r >> 2));
            px[i * 4 + 3] = 255;
        }
        using var fb = SstvImage.Lock();
        Marshal.Copy(px, 0, fb.Address + sl.Line * fb.RowBytes + sl.X0 * 4, px.Length);
    }

    [RelayCommand]
    private Task ApplyPosAsync()
    {
        if (!double.TryParse(MyLat.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var lat) ||
            !double.TryParse(MyLon.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var lon))
        { AddLog("position invalide (degrés décimaux)"); return Task.CompletedTask; }
        MapController.SetMyPosition(lat, lon);
        return RunAsync($"SET_POS {lat:F5} {lon:F5}", ct => _radio.OkAsync(UvLinkProtocol.SetPos(lat, lon), ct));
    }

    [RelayCommand]
    private void ClearDecoder()
    {
        DecoderLines.Clear(); DecodeHistory.Clear(); AprsPackets.Clear(); AprsMonitor.Clear(); ClearPocsag(); ClearIsm();
    }

    // ---- connexion ----
    [ObservableProperty] private bool _isConnected;
    [ObservableProperty] private bool _isBusy;
    [ObservableProperty] private string _connectionText = "Non connecté";
    [ObservableProperty] private string _firmwareText = "";

    // ---- état du poste (STATUS) ----
    [ObservableProperty] private bool _radioPresent;
    [ObservableProperty] private string _radioText = "Poste : —";
    [ObservableProperty] private bool _signal;
    [ObservableProperty] private bool _transmitting;
    [ObservableProperty] private int _sMeter;
    [ObservableProperty] private string _rssiText = "";
    [ObservableProperty] private string _batteryText = "";
    [ObservableProperty] private string _vfo0Text = "---.----";
    [ObservableProperty] private string _vfo1Text = "---.----";
    [ObservableProperty] private string _modeText = "";
    // pastille « décodeur » de l'accueil : mode annoncé par l'ESP, sinon celui choisi
    public string ModeChipText => "Décodeur : " + (string.IsNullOrEmpty(ModeText) ? ModeNames[Math.Clamp(SelectedMode, 0, ModeNames.Length - 1)] : ModeText);
    partial void OnModeTextChanged(string value) => OnPropertyChanged(nameof(ModeChipText));
    [ObservableProperty] private bool _monitorOn;
    [ObservableProperty] private int _adcDc;
    [ObservableProperty] private bool _adcSaturated;    // STATUS b7 : entrée audio en butée -> volume du poste trop fort
    [ObservableProperty] private bool _agcOn = true;    // AGC numérique du firmware (coupé par la calibration)
    [ObservableProperty] private string _audioPathText = "";   // « audio plat » (forcé en mode décodeur, ou case cochée)
    private byte _fwMode;
    private void UpdateAudioPathText()
        => AudioPathText = _fwMode != 0 ? "audio plat (forcé par le mode)" : FlatAudio ? "audio plat" : "audio phonie (filtres)";
    private byte _fwGain; private bool _fwAgc;          // dernier réglage vu dans le STATUS
    [ObservableProperty] private string _adcText = "";  // mesures brutes de l'ADC (dernière seconde)
    private byte _hostProto;                            // version du firmware hôte du poste (H<n>), 0 = pas de poste
    // Le gain AF du poste (SET_AF_GAIN, hôte H29) n'est plus exposé : le niveau
    // est aligné sur l'audio plat (poste au volume maximal + atténuateur 47 kΩ
    // en série devant C1), le gain BK4819 n'agit pas sur ce chemin.
    [ObservableProperty] private bool _gpsFix;

    // ---- VFO : deux cartes « affichage + réglage » (VfoCard), envoi dès modification ----
    public VfoCard[] Vfos { get; }
    [ObservableProperty] private int _selectedVfo;      // VFO d'émission (bouton « émettre ici » d'une carte)
    [ObservableProperty] private int _selectedMode;
    [ObservableProperty] private bool _dualWatch;
    private bool FlatAudio => Vfos[Vfos[1].IsRx ? 1 : 0].Flat;
    [ObservableProperty] private string _lastDecodeText = "";   // dernier décodage (toucher = onglet)
    // Batterie du poste (2S Li-ion, 1600/2200 mAh) : pourcentage d'après la tension,
    // courbe de décharge approchée par paliers (8,40 V = 100 %, 6,60 V = 0 %).
    [ObservableProperty] private int _batteryPercent;
    [ObservableProperty] private string _batteryHeaderText = "";
    [ObservableProperty] private bool _batteryLow;
    private static int BatteryPercentOf(int mv)
    {
        (int mv, int pct)[] t = { (8400, 100), (8100, 90), (7900, 75), (7700, 60), (7500, 45), (7300, 30), (7100, 18), (6900, 8), (6600, 0) };
        if (mv >= t[0].mv) return 100;
        for (int i = 1; i < t.Length; i++)
            if (mv >= t[i].mv) return t[i].pct + (mv - t[i].mv) * (t[i - 1].pct - t[i].pct) / (t[i - 1].mv - t[i].mv);
        return 0;
    }
    private int _lastDecodeTab = 1;
    [RelayCommand] private void OpenLastDecode() => SelectedTab = _lastDecodeTab;

    // ---- envoi immédiat (2026-10-30, demande de l'opérateur : plus de bouton de
    // validation). Chaque changement d'un réglage part aussitôt vers l'ESP ;
    // les champs texte (fréquences, position) sont validés par Entrée ou en
    // quittant le champ (UpdateSourceTrigger=LostFocus dans la vue), les
    // compteurs sont regroupés sur 300 ms. _quiet > 0 pendant les mises à jour
    // programmées (lecture du VFO au changement A/B, STATUS) : pas d'envoi.
    private int _quiet;
    private CancellationTokenSource? _audioDebounce;
    private bool AutoSend => IsConnected && _quiet == 0;
    private void Debounced(ref CancellationTokenSource? cts, Func<Task> op, int ms = 300)
    {
        cts?.Cancel();
        var c = cts = new CancellationTokenSource();
        var tok = c.Token;
        _ = Task.Delay(ms, tok).ContinueWith(t => { if (!t.IsCanceled) Post(() => _ = op()); }, TaskScheduler.Default);
    }
    private void QueueAudio() { if (AutoSend) Debounced(ref _audioDebounce, ApplyAudioAsync); }
    private void OnVfoCardChanged(VfoCard c) { if (AutoSend) Debounced(ref c.Debounce, () => ApplyVfoAsync(c)); UpdateAudioPathText(); }
    partial void OnAudioRxModeChanged(int value)   => QueueAudio();
    partial void OnAudioGainChanged(int value)     => QueueAudio();
    partial void OnAgcOnChanged(bool value)        => QueueAudio();
    partial void OnDualWatchChanged(bool value)    { if (AutoSend) _ = ApplyRadioAsync(); }
    partial void OnSelectedModeChanged(int value)  { OnPropertyChanged(nameof(ModeChipText)); OnPropertyChanged(nameof(SstvDecoder)); OnPropertyChanged(nameof(PocsagDecoder)); OnPropertyChanged(nameof(IsmDecoder)); if (AutoSend) _ = ApplyModeAsync(); }
    // Onglet SSTV : bascule directe du décodeur (le mode se règle sinon dans RÉGLAGES → DÉCODEUR)
    public bool SstvDecoder
    {
        get => SelectedMode == 2;
        set { if (value != (SelectedMode == 2)) SelectedMode = value ? 2 : 0; }
    }
    private DateTime _modeSentUtc;     // le STATUS reflète l'ancien mode jusqu'au tick suivant notre SET_MODE
    partial void OnMyLatChanged(string value)      { if (AutoSend && value.Length > 0 && MyLon.Length > 0) _ = ApplyPosAsync(); }
    partial void OnMyLonChanged(string value)      { if (AutoSend && value.Length > 0 && MyLat.Length > 0) _ = ApplyPosAsync(); }
    // VFO d'émission : la carte touchée (« émettre ici ») -> SET_RADIO ; les cartes affichent TX ▶
    partial void OnSelectedVfoChanged(int value)
    {
        foreach (var c in Vfos) c.IsTx = c.Index == value;
        if (AutoSend) _ = ApplyRadioAsync();
    }
    // Lecture d'un VFO dans l'ESP -> carte, SANS envoi (connexion)
    private async Task LoadVfoFormAsync(int vfo)
    {
        if (!IsConnected) return;
        var c = Vfos[vfo & 1];
        try
        {
            var p = await _radio.GetVfoAsync((byte)vfo, CancellationToken.None);
            if (p == null || p.RxHz == 0) { AddLog($"VFO {(vfo == 0 ? "A" : "B")} : inconnu de l'ESP, réglages de l'appli conservés"); return; }
            _quiet++;
            try
            {
                c.RxMhz = (p.RxHz / 1e6).ToString("F4", CultureInfo.InvariantCulture);
                c.TxMhz = p.TxHz == p.RxHz ? "" : (p.TxHz / 1e6).ToString("F4", CultureInfo.InvariantCulture);
                c.Modulation = p.Modulation; c.Squelch = p.Squelch; c.Narrow = p.Narrow; c.Flat = (p.Flags & 1) != 0;
                c.PowerIndex = p.Power >= 7 ? 0 : p.Power >= 6 ? 1 : 2;
            }
            finally { _quiet--; }
            c.Debounce?.Cancel();
        }
        catch (Exception e) { AddLog($"lecture VFO {(vfo == 0 ? "A" : "B")} : {e.Message}"); }
    }

    public string[] PowerNames { get; } = { "Haute", "Moyenne", "Basse" };
    public string[] ModeNames { get; } = UvLinkProtocol.ModeNames;
    public ObservableCollection<string> LogLines { get; } = new();

    public bool AutoConnect { get; set; }

    // Reconnexion automatique tant que l'utilisateur n'a pas demandé la
    // déconnexion (carte redémarrée, liaison BLE rompue) : nouvel essai 3 s après.
    private bool _wantConnected;
    private int _keepAliveTicks, _lifePingTicks; private bool _pinging;
    private DateTime _lastStatusUtc;
    private async Task KeepAliveAsync()
    {
        _pinging = true;
        try
        {
            AddLog($"liaison BLE morte (aucun STATUS depuis {(DateTime.UtcNow - _lastStatusUtc).TotalSeconds:F0} s) : purge et reconnexion");
            throw new TimeoutException("aucun STATUS");
        }
        catch (Exception)
        {
            // jamais d'attente non bornée sur le thread de l'interface
            try { await Task.Run(() => _radio.DisconnectAsync()).WaitAsync(TimeSpan.FromSeconds(5)); } catch { }
            IsConnected = false;
            OnLinkLost();
            _connectFailures = 2;          // le prochain échec déclenche la réinitialisation de l'adaptateur
            ScheduleReconnect();
        }
        finally { _pinging = false; }
    }
    private void ScheduleReconnect()
    {
        if (!_wantConnected) return;
        _ = Task.Delay(3000).ContinueWith(_ => Post(() =>
        {
            if (_wantConnected && !IsConnected && !IsBusy) { AddLog("reconnexion…"); _ = ConnectAsync(); }
        }));
    }

    [RelayCommand]
    private async Task ConnectAsync()
    {
        if (IsConnected) { await DisconnectAsync(); return; }
        _wantConnected = true;
        IsBusy = true;
        _cts = new CancellationTokenSource();
        try
        {
            ConnectionText = "Recherche d'un UVLink…";
            AddLog("connexion demandée");
            if (App.PermissionProblem() is { } perm) throw new InvalidOperationException(perm);
            var who = await _radio.ConnectAsync(_cts.Token);
            IsConnected = true;
            _lastStatusUtc = DateTime.UtcNow;   // la surveillance passive compte à partir d'ici
            _connectFailures = 0;
            ConnectionText = who;
            var (proto, maj, min) = await _radio.PingAsync(_cts.Token);
            var (present, hostProto, fw) = await _radio.GetInfoAsync(_cts.Token);
            _hostProto = present ? hostProto : (byte)0;
            FirmwareText = $"UVLink fw {maj}.{min} (proto {proto})" + (present ? $" — poste {fw} H{hostProto}" : " — pas de poste");
            AddLog(FirmwareText);
            _audio.Volume = (float)(Volume / 100.0);
            if (AudioOn) _audio.StartPlayback();
            if (present) { await LoadVfoFormAsync(0); await LoadVfoFormAsync(1); }   // cartes = état réel avant tout envoi automatique
            await LoadRssiCorrAsync();
            if (PhoneGps && _phonePos != null && !_espGpsValid) { _sentPos = null; UsePhonePosition(_phonePos.Value.Lat, _phonePos.Value.Lon, true); }   // ESP peut-être redémarré : SET_POS
        }
        catch (Exception e)
        {
            ConnectionText = "Échec : " + e.Message;
            AddLog(ConnectionText + (e is TimeoutException or InvalidOperationException ? "" : $"  [{e.GetType().Name}{(e.InnerException != null ? " / " + e.InnerException.GetType().Name + " : " + e.InnerException.Message : "")}]"));
            await _radio.DisconnectAsync();
            IsConnected = false;
            // Pile Bluetooth du PC coincée (échecs en boucle après une liaison
            // rompue) : on réinitialise l'adaptateur avant de réessayer.
            if (++_connectFailures >= 2 && _wantConnected)
            {
                _connectFailures = 0;
                AddLog("Bluetooth coincé : réinitialisation de l'adaptateur…");
                if (!await _radio.RecoverAsync()) AddLog("réinitialisation impossible sur cette plateforme");
            }
            IsBusy = false;
            ScheduleReconnect();
        }
        finally { IsBusy = false; }
    }
    private int _connectFailures;

    private async Task DisconnectAsync()
    {
        _wantConnected = false;
        _cts?.Cancel();
        OnLinkLost();
        await _radio.DisconnectAsync();
        IsConnected = false;
        ConnectionText = "Déconnecté";
    }

    private async Task ApplyVfoAsync(VfoCard c)
    {
        if (!TryParseMhz(c.RxMhz, out var rx)) { AddLog($"{c.Name} : fréquence RX invalide"); return; }
        if (!TryParseMhz(string.IsNullOrWhiteSpace(c.TxMhz) ? c.RxMhz : c.TxMhz, out var tx)) { AddLog($"{c.Name} : fréquence TX invalide"); return; }
        if (c.Modulation >= 3 && _hostProto > 0 && _hostProto < 30) AddLog($"{c.Name} : {c.ModulationText} demande le firmware hôte H30 (poste en H{_hostProto}), le poste restera en FM");
        byte power = c.PowerIndex switch { 0 => 7, 1 => 6, _ => 3 };
        var p = new VfoParams(rx, tx, (byte)c.Modulation, c.Narrow, 0, 0, power, (byte)c.Squelch, (byte)(c.Flat ? 1 : 0));
        await RunAsync($"SET_VFO {(c.Index == 0 ? "A" : "B")} {rx / 1e6:F4}/{tx / 1e6:F4} {c.ModulationText} SQ{c.Squelch} {c.BandwidthText}{(c.Flat ? " plat" : "")}",
                       ct => _radio.OkAsync(UvLinkProtocol.SetVfo((byte)c.Index, p), ct));
    }

    private DateTime _radioSentUtc;
    [RelayCommand]
    private Task ApplyRadioAsync()
    {
        _radioSentUtc = DateTime.UtcNow;
        return RunAsync($"SET_RADIO txVfo={(SelectedVfo == 0 ? "A" : "B")} dw={DualWatch}",
                    ct => _radio.OkAsync(UvLinkProtocol.SetRadio((byte)SelectedVfo, DualWatch), ct));
    }

    // PTT : commande au poste puis micro -> AUDIO_TX ; au relâcher, dernier paquet
    // (drapeau fin) puis PTT off. Le firmware relâche aussi de lui-même 300 ms
    // après le dernier paquet audio.
    [ObservableProperty] private bool _pttOn;      // PTT demandé par l'appli (bouton), indépendant du flag TX du poste
    [RelayCommand]
    private async Task PttDownAsync()
    {
        if (PttOn) return;
        PttOn = true;
        await RunAsync("PTT on", ct => _radio.OkAsync(UvLinkProtocol.Ptt(true), ct));
        if (IsConnected) _audio.StartCapture(_radio.SendAudioAsync);
    }

    [RelayCommand]
    private async Task PttUpAsync()
    {
        if (!PttOn) return;
        PttOn = false;
        // Fermeture du micro bornée, puis PTT off envoyé coûte que coûte (jusqu'à 3
        // essais) : un PTT off perdu laisse le poste en émission.
        try { await _audio.StopCaptureAsync().WaitAsync(TimeSpan.FromSeconds(2)); }
        catch (Exception e) { AddLog("micro : fermeture : " + e.Message); }
        for (int attempt = 1; attempt <= 3 && IsConnected; attempt++)
        {
            try
            {
                bool ok = await _radio.OkAsync(UvLinkProtocol.Ptt(false), CancellationToken.None);
                AddLog($"PTT off : {(ok ? "ok" : "refusé")}" + (attempt > 1 ? $" (essai {attempt})" : ""));
                if (ok) return;
            }
            catch (Exception e) { AddLog($"PTT off : {e.Message} (essai {attempt})"); }
            await Task.Delay(500);
        }
    }

    [RelayCommand]
    private Task ToggleMonitorAsync()
        => RunAsync(MonitorOn ? "MONITOR off" : "MONITOR on", ct => _radio.OkAsync(UvLinkProtocol.Monitor(!MonitorOn), ct));

    [RelayCommand]
    private async Task ApplyModeAsync()
    {
        byte id = UvLinkProtocol.ModeIds[Math.Clamp(SelectedMode, 0, UvLinkProtocol.ModeIds.Length - 1)];
        _modeSentUtc = DateTime.UtcNow;
        await RunAsync($"SET_MODE {ModeNames[SelectedMode]}", ct => _radio.OkAsync(UvLinkProtocol.SetMode(id), ct));
        DecoderLines.Clear(); LevelText = ""; LevelVerdictText = ""; LevelRms = 0;
    }

    [RelayCommand]
    private async Task PingAsync()
    {
        try { var (p, a, b) = await _radio.PingAsync(CancellationToken.None); AddLog($"PING : proto {p}, fw {a}.{b}"); }
        catch (Exception e) { AddLog("PING : " + e.Message); }
    }

    private async Task RunAsync(string label, Func<CancellationToken, Task<bool>> op)
    {
        if (!IsConnected) { AddLog($"{label} : non connecté"); return; }
        try { AddLog($"{label} : {(await op(CancellationToken.None) ? "ok" : "refusé")}"); }
        catch (Exception e) { AddLog($"{label} : {e.Message}"); }
    }

    // Le poste ne renvoie pas ses VFO : l'ESP n'affiche que le dernier SET_VFO
    // (mémorisé en NVS). Poste vu sans aucun VFO connu -> on pousse le réglage
    // de l'appli une fois, pour que l'affichage et le poste soient d'accord.
    private bool _vfoPushed;

    private void ApplyStatus(RadioStatus s)
    {
        _lastStatusUtc = DateTime.UtcNow;
        if (s.RadioPresent && s.Rx0 == 0 && s.Rx1 == 0 && !_vfoPushed && IsConnected)
        {
            _vfoPushed = true;
            AddLog("poste sans VFO connu : envoi du réglage de l'appli");
            _ = ApplyVfoAsync(Vfos[0]).ContinueWith(_ => ApplyRadioAsync());
        }
        RadioPresent = s.RadioPresent;
        RadioText = s.RadioPresent ? "Poste : connecté" : "Poste : absent";
        Signal = s.Signal;
        Transmitting = s.Tx;
        SMeter = s.SMeter;
        RssiText = s.RadioPresent ? $"{s.RssiDbm} dBm  S{s.SMeter}" : "";
        BatteryText = s.RadioPresent ? $"{s.BatteryMv / 1000.0:F2} V" : "";
        BatteryPercent = s.RadioPresent ? BatteryPercentOf(s.BatteryMv) : 0;
        BatteryHeaderText = s.RadioPresent ? $"{BatteryPercent} %  {s.BatteryMv / 1000.0:F2} V" : "";
        BatteryLow = s.RadioPresent && BatteryPercent <= 15;
        for (int i = 0; i < 2; i++)
        {
            var c = Vfos[i];
            uint rx = i == 0 ? s.Rx0 : s.Rx1, tx = i == 0 ? s.Tx0 : s.Tx1;
            c.FreqText = Freq(rx);
            c.TxText = tx != rx && tx != 0 ? "↑ " + Freq(tx) : "";
            c.IsRx = s.RadioPresent && s.RxVfo == i;
            c.Signal = s.Signal && s.RxVfo == i;
            c.SetMeter(c.IsRx ? s.SMeter : 0, c.IsRx ? $"{s.RssiDbm} dBm  S{s.SMeter}" : "");
        }
        RxFreqText = s.RadioPresent ? $"{(s.RxVfo == 0 ? 'A' : 'B')} {Freq(s.RxVfo == 0 ? s.Rx0 : s.Rx1)}" : "---.----";
        // VFO d'émission = état réel du poste, sauf dans les 2 s qui suivent notre SET_RADIO
        if (s.RadioPresent && s.TxVfo != SelectedVfo && (DateTime.UtcNow - _radioSentUtc).TotalSeconds > 2) { _quiet++; try { SelectedVfo = s.TxVfo; } finally { _quiet--; } }
        ModeText = UvLinkProtocol.ModeNameOf(s.Mode);
        _fwMode = s.Mode; UpdateAudioPathText();
        // Liste des modes = mode réel de l'ESP (redémarrage, autre appli…), sauf dans les 2 s suivant notre SET_MODE
        int modeIdx = Math.Max(0, Array.IndexOf(UvLinkProtocol.ModeIds, s.Mode));
        if (modeIdx != SelectedMode && (DateTime.UtcNow - _modeSentUtc).TotalSeconds > 2) { _quiet++; try { SelectedMode = modeIdx; } finally { _quiet--; } }
        MonitorOn = s.Monitor;
        // Case « Double veille » = état réel du poste, sauf dans les 2 s qui suivent
        // notre propre SET_RADIO (le STATUS reflète l'ancien état jusqu'au tick suivant).
        if (s.RadioPresent && s.DualWatch != DualWatch && (DateTime.UtcNow - _radioSentUtc).TotalSeconds > 2) { _quiet++; try { DualWatch = s.DualWatch; } finally { _quiet--; } }
        AdcDc = s.AdcDc;
        if (s.AdcSat && !AdcSaturated) AddLog("entrée audio saturée : baisser le volume du poste");
        AdcSaturated = s.AdcSat;
        // Réglage courant du firmware (fw ≥ 0.6) : on ne touche aux champs que
        // s'il a changé côté ESP (calibration, console), pas à chaque STATUS.
        if (s.RxGain > 0 && (s.RxGain != _fwGain || s.Agc != _fwAgc)) { _fwGain = s.RxGain; _fwAgc = s.Agc; _quiet++; try { AudioGain = s.RxGain; AgcOn = s.Agc; } finally { _quiet--; } }
        AdcText = s.RadioPresent
            ? $"ADC : dc {s.AdcDc}  crête {s.AdcPeak}  RMS {s.AdcRms}  (12 bits, ±2048)  saturation {s.SatPct} %"
            : "";
        GpsFix = s.GpsFix;
    }

    private static string Freq(uint hz) => hz == 0 ? "---.----" : (hz / 1e6).ToString("000.0000", CultureInfo.InvariantCulture);

    private static bool TryParseMhz(string s, out uint hz)
    {
        hz = 0;
        if (!double.TryParse(s.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var mhz)) return false;
        if (mhz < 18 || mhz > 1300) return false;
        hz = (uint)Math.Round(mhz * 1e6);
        return true;
    }

    private void AddLog(string m)
    {
        var line = $"{DateTime.Now:HH:mm:ss} {m}";
        LogLines.Insert(0, line);
        while (LogLines.Count > 200) LogLines.RemoveAt(LogLines.Count - 1);
        Console.WriteLine(line);
        Services.Journal.Write(line);
    }

    private static void Post(Action a)
    {
        if (Dispatcher.UIThread.CheckAccess()) a(); else Dispatcher.UIThread.Post(a);
    }
}

// Une bande du BK4819 et sa correction RSSI (dB) ; Changed déclenche l'envoi.
public sealed partial class RssiBand : ObservableObject
{
    public RssiBand(int index, string name) { Index = index; Name = name; }
    public int Index { get; }
    public string Name { get; }
    [ObservableProperty] private int _value;
    public event Action? Changed;
    partial void OnValueChanged(int value) => Changed?.Invoke();
}

// Une ligne de la liste APRS (paquet parsé par le firmware).
public sealed class AprsRow
{
    public AprsRow(AprsEvent a, string? source = null)
    {
        Time = DateTime.Now.ToString("HH:mm:ss");
        Source = source ?? a.Source;
        Src = a.Src;
        Kind = a.KindText + (a.Adrasec ? " ADRASEC" : "");
        Symbol = a.SymCode == '\0' ? "" : $"{a.SymTable}{a.SymCode}";
        Position = a.HasPos || a.Adrasec ? $"{a.Lat:F4} {a.Lon:F4}" : "";
        Extra = (a.HasCourse ? $"{a.CourseDeg}° {a.SpeedKmh} km/h " : "") + (a.HasAlt ? $"{a.AltM} m " : "") + (a.HasRange ? $"{a.DistKm:F1} km {a.BearingDeg}°" : "");
        Via = a.Via.Length == 0 ? "direct" : "via " + a.Via;
        Text = (a.Name.Length > 0 ? $"[{a.Name}] " : "") + a.Text;
    }
    public string Time { get; }
    public string Source { get; }       // ESP, appli, ESP+appli
    public string Src { get; }
    public string Kind { get; }
    public string Symbol { get; }
    public string Position { get; }
    public string Extra { get; }
    public string Via { get; }
    public string Text { get; }
    public string Summary => $"{Time} {Source,-9} {Src,-9} {Kind,-9} {Position,-18} {Extra} {Via}  {Text}";
}

// Une ligne de la liste ISM : « 21:40:12  Nexus-TH  id 90 » puis le résumé des champs rtl_433.
public sealed class IsmRow
{
    public IsmRow(Ism.IsmDecode d)
    {
        Head = $"{d.Time:HH:mm:ss}  {d.Model}" + (d.Id.Length > 0 ? $"   id {d.Id}" : "");
        Body = d.Summary; Json = d.Json;
    }
    public string Head { get; }
    public string Body { get; }
    public string Json { get; }
}

// Une ligne de la liste POCSAG, présentée comme dans Portsdown 4 : en-tête
// « POCSAG1200   RIC 0028328   F0   Alpha » puis le message.
public sealed class PocsagRow
{
    public PocsagRow(PocsagEvent e, string text, string? source = null)
    {
        Time = DateTime.Now.ToString("HH:mm:ss");
        Source = source ?? e.Source;
        Address = e.Address;
        string type = e.ToneOnly ? "Bip" : e.Numeric ? "Numérique" : "Alpha";
        string warn = e.Bad > 0 ? $"   {e.Bad} mot{(e.Bad > 1 ? "s" : "")} incorrigible{(e.Bad > 1 ? "s" : "")}" : "";
        if (e.Corrected > 0) warn += $"   BCH +{e.Corrected}";
        if (e.Partial) warn += "   douteux";
        Head = $"{Time}  {Source,-9} POCSAG{e.Baud}   RIC {e.Address:D7}   F{e.Function}   {type}{warn}";
        Body = e.ToneOnly ? "(bip seul)" : text;
        IsTone = e.ToneOnly;
    }
    public string Time { get; }
    public string Source { get; }       // ESP, appli, ESP+appli
    public uint Address { get; }
    public string Head { get; }
    public string Body { get; }
    public bool IsTone { get; }
}
