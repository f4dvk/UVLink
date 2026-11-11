using Mapsui;
using Mapsui.Extensions;
using Mapsui.Layers;
using Mapsui.Nts;
using Mapsui.Projections;
using Mapsui.Styles;
using Mapsui.Tiling;
using NetTopologySuite.Geometries;
using UVLink.Protocol;
using Color = Mapsui.Styles.Color;
using Font = Mapsui.Styles.Font;

namespace UVLink.Map;

// Onglet CARTE : fond OpenStreetMap (Mapsui, même pile que PCT_Report) et trois
// couches de marqueurs alimentées par les EVENT du firmware : stations APRS
// (symbole APRS réel), radiosondes (ballon + trace du vol), balises SARSAT
// (avion en détresse, halo rouge ou ambre pour une balise de test), plus la
// position de l'opérateur (GPS de l'ESP ou position manuelle). Chaque marqueur
// porte l'heure de sa dernière position ; Purge() efface ceux qui n'ont rien
// donné depuis Expiry (réglable, 1 h par défaut, « jamais » possible).
public sealed class MapController
{
    public sealed class Marker
    {
        public string Key = "";
        public double Lat, Lon;
        public DateTime LastSeen;
        public string Label = "";
        public string Detail = "";
        public char SymTable = '/', SymCode = '-';
        public bool IsTest;
        public List<(double Lat, double Lon)> Trail = new();   // sondes : positions successives (vol)
        // fiche station (clic sur l'épingle) : dernière trame APRS, première vue, nombre, dernier statut
        public AprsEvent? Aprs;
        public DateTime FirstSeen;
        public int Count;
        public string Status = "";
        public string Layer = "";
    }

    private readonly WritableLayer _aprs   = new() { Name = "APRS",   Style = null };
    private readonly WritableLayer _sondes = new() { Name = "Sondes", Style = null };
    private readonly WritableLayer _sarsat = new() { Name = "SARSAT", Style = null };
    private readonly WritableLayer _me     = new() { Name = "Moi",    Style = null };
    private readonly Dictionary<string, Marker> _aprsM = new(), _sondeM = new(), _sarsatM = new();
    private (double Lat, double Lon)? _mePos;
    private static readonly Image BalloonImage = new() { Source = "embedded://UVLink.Assets.balloon.png" };
    private static readonly Image CrashPlaneImage = new() { Source = "embedded://UVLink.Assets.crash-plane.png" };

    public MapController()
    {
        Map = new Mapsui.Map { CRS = "EPSG:3857" };
        Map.Layers.Add(OpenStreetMap.CreateTileLayer("UVLink"));
        Map.Layers.Add(_aprs);
        Map.Layers.Add(_sondes);
        Map.Layers.Add(_sarsat);     // une détresse ne doit jamais être cachée
        Map.Layers.Add(_me);
    }
    // Vue initiale (appelée par la vue dès que le contrôle a une taille) :
    // France, en attendant une position (opérateur ou premier marqueur).
    public void Home()
    {
        if (_aprsM.Count + _sondeM.Count + _sarsatM.Count > 0 || _mePos != null) { _centered = true; FitAll(); }
        else if (!_centered) Try(() => Map.Navigator.CenterOnAndZoomTo(ToWorld(46.8, 2.4), 2500, 0, null));
    }

    public Mapsui.Map Map { get; }
    public TimeSpan? Expiry { get; set; } = TimeSpan.FromHours(1);   // null = jamais
    public bool ShowAprs { get => _aprs.Enabled; set { _aprs.Enabled = value; Redraw(); } }
    public bool ShowSondes { get => _sondes.Enabled; set { _sondes.Enabled = value; Redraw(); } }
    public bool ShowSarsat { get => _sarsat.Enabled; set { _sarsat.Enabled = value; Redraw(); } }
    /// <summary>Marqueur désigné par une entité de la carte (clic) : couche + clé.</summary>
    public Marker? Find(string? layer, string? key)
    {
        if (key == null) return null;
        var d = layer == "APRS" ? _aprsM : layer == "Sondes" ? _sondeM : layer == "SARSAT" ? _sarsatM : null;
        return d != null && d.TryGetValue(key, out var m) ? m : null;
    }
    public string[] InfoLayerNames { get; } = { "APRS", "Sondes", "SARSAT" };
    public int AprsCount => _aprsM.Count;
    public int SondeCount => _sondeM.Count;
    public int SarsatCount => _sarsatM.Count;
    public event Action? Changed;

    // ---- alimentation ----
    public void Update(AprsEvent a)
    {
        if (!(a.HasPos || a.Adrasec) || !Plausible(a.Lat, a.Lon))
        {
            // statut (ou message) d'une station déjà sur la carte : on garde le texte pour la fiche
            if (a.Kind == 3 && _aprsM.TryGetValue(a.Src, out var st)) { st.Status = a.Text; st.Count++; Changed?.Invoke(); }
            return;
        }
        string key = a.Kind == 2 && a.Name.Length > 0 ? a.Name : a.Src;   // un objet est tracé sous son nom
        if (!_aprsM.TryGetValue(key, out var m)) _aprsM[key] = m = new Marker { Key = key, Layer = "APRS", FirstSeen = DateTime.UtcNow };
        m.Lat = a.Lat; m.Lon = a.Lon; m.LastSeen = DateTime.UtcNow; m.Aprs = a; m.Count++;
        m.SymTable = a.SymCode == '\0' ? '/' : a.SymTable; m.SymCode = a.SymCode == '\0' ? '-' : a.SymCode;
        m.Label = key;
        m.Detail = (a.HasCourse ? $"{a.CourseDeg}° {a.SpeedKmh} km/h " : "") + (a.HasAlt ? $"{a.AltM} m " : "") + a.Text;
        RebuildAprs(); FirstFix(m);
    }

    public void Update(SondeEvent s)
    {
        if (!s.HasPos || !Plausible(s.Lat, s.Lon)) return;
        string type = s.Type == 0x20 ? "M20" : s.Type != 0 ? "M10" : s.Kind == 1 ? "RS41" : "sonde";
        string key = s.Serial ?? type;
        if (!_sondeM.TryGetValue(key, out var m)) _sondeM[key] = m = new Marker { Key = key, Layer = "Sondes", FirstSeen = DateTime.UtcNow };
        m.Count++;
        if (m.Trail.Count == 0 || Math.Abs(m.Trail[^1].Lat - s.Lat) > 1e-5 || Math.Abs(m.Trail[^1].Lon - s.Lon) > 1e-5)
        {
            m.Trail.Add((s.Lat, s.Lon));
            if (m.Trail.Count > 2000) m.Trail.RemoveAt(0);
        }
        m.Lat = s.Lat; m.Lon = s.Lon; m.LastSeen = DateTime.UtcNow;
        m.Label = $"{type} {s.Serial ?? ""}".Trim() + $"  {s.AltM} m";
        m.Detail = (s.TempC is double t ? $"{t:F1} °C " : "") + (s.SpeedV is double vv ? $"{vv:+0.0;-0.0} m/s" : "");
        RebuildSondes(); FirstFix(m);
    }

    public void Update(SarsatEvent e)
    {
        if (!e.HasPosition || !Plausible(e.Lat, e.Lon)) return;
        if (!_sarsatM.TryGetValue(e.HexId, out var m)) _sarsatM[e.HexId] = m = new Marker { Key = e.HexId, Layer = "SARSAT", FirstSeen = DateTime.UtcNow };
        m.Count++;
        m.Lat = e.Lat; m.Lon = e.Lon; m.LastSeen = DateTime.UtcNow; m.IsTest = e.IsTest;
        m.Label = e.HexId; m.Detail = e.ProtocolText;
        RebuildSarsat(); FirstFix(m);
    }

    public void SetMyPosition(double? lat, double? lon)
    {
        _mePos = lat is double la && lon is double lo && Plausible(la, lo) ? (la, lo) : null;
        _me.Clear();
        if (_mePos is { } p)
        {
            var f = new GeometryFeature { Geometry = ToPoint(p.Lat, p.Lon) };
            f.Styles.Add(new SymbolStyle { SymbolType = SymbolType.Ellipse, SymbolScale = 0.55,
                                           Fill = new Brush(new Color(47, 209, 255, 230)), Outline = new Pen(Color.White, 2) });
            f.Styles.Add(Label("moi", 12));
            _me.Add(f);
            if (!_centered) { _centered = true; Try(() => Map.Navigator.CenterOnAndZoomTo(ToWorld(p.Lat, p.Lon), 40, 0, null)); }
        }
        _me.DataHasChanged(); Redraw();
    }

    // ---- expiration ----
    public void Purge()
    {
        if (Expiry is not TimeSpan ttl) return;
        var limit = DateTime.UtcNow - ttl;
        bool a = RemoveOld(_aprsM, limit), s = RemoveOld(_sondeM, limit), r = RemoveOld(_sarsatM, limit);
        if (a) RebuildAprs();
        if (s) RebuildSondes();
        if (r) RebuildSarsat();
    }
    private static bool RemoveOld(Dictionary<string, Marker> d, DateTime limit)
    {
        var old = d.Where(kv => kv.Value.LastSeen < limit).Select(kv => kv.Key).ToList();
        foreach (var k in old) d.Remove(k);
        return old.Count > 0;
    }
    public void Clear()
    {
        _aprsM.Clear(); _sondeM.Clear(); _sarsatM.Clear();
        RebuildAprs(); RebuildSondes(); RebuildSarsat();
    }

    // ---- navigation ----
    public void ZoomIn()  => Try(() => Map.Navigator.ZoomIn(200, null));
    public void ZoomOut() => Try(() => Map.Navigator.ZoomOut(200, null));
    public void FitAll()
    {
        var pts = _aprsM.Values.Concat(_sondeM.Values).Concat(_sarsatM.Values).Select(m => (m.Lat, m.Lon)).ToList();
        if (_mePos is { } p) pts.Add(p);
        if (pts.Count == 0) { Try(() => Map.Navigator.CenterOnAndZoomTo(ToWorld(46.8, 2.4), 2500, 0, null)); return; }
        if (pts.Count == 1) { Try(() => Map.Navigator.CenterOnAndZoomTo(ToWorld(pts[0].Lat, pts[0].Lon), 40, 0, null)); return; }
        var box = new MRect(pts.Min(q => ToWorld(q.Lat, q.Lon).X), pts.Min(q => ToWorld(q.Lat, q.Lon).Y),
                            pts.Max(q => ToWorld(q.Lat, q.Lon).X), pts.Max(q => ToWorld(q.Lat, q.Lon).Y)).Grow(2000);
        Try(() => Map.Navigator.ZoomToBox(box, MBoxFit.Fit, 0, null));
    }
    private bool _centered;
    private void FirstFix(Marker m)
    {
        if (_centered) return;
        _centered = true;
        Try(() => Map.Navigator.CenterOnAndZoomTo(ToWorld(m.Lat, m.Lon), 40, 0, null));
    }

    // ---- rendu ----
    private void RebuildAprs()
    {
        _aprs.Clear();
        foreach (var m in _aprsM.Values)
        {
            var f = new GeometryFeature { Geometry = ToPoint(m.Lat, m.Lon) };
            f["key"] = m.Key; f["layer"] = "APRS";
            f.Styles.Add(new ImageStyle { Image = AprsSymbolSprite.CellImage(m.SymTable, m.SymCode), SymbolScale = 0.95 });
            f.Styles.Add(Label(m.Label, 18));
            _aprs.Add(f);
        }
        _aprs.DataHasChanged(); Redraw();
    }
    private void RebuildSondes()
    {
        _sondes.Clear();
        foreach (var m in _sondeM.Values)
        {
            if (m.Trail.Count >= 2)
            {
                var line = new GeometryFeature { Geometry = new LineString(m.Trail.Select(p => { var w = ToWorld(p.Lat, p.Lon); return new Coordinate(w.X, w.Y); }).ToArray()) };
                line.Styles.Add(new VectorStyle { Line = new Pen(new Color(255, 120, 30, 200), 2), Fill = null });
                _sondes.Add(line);
            }
            var f = new GeometryFeature { Geometry = ToPoint(m.Lat, m.Lon) };
            f["key"] = m.Key; f["layer"] = "Sondes";
            f.Styles.Add(new ImageStyle { Image = BalloonImage, SymbolScale = 0.75, Offset = new Offset(0, 18) });
            f.Styles.Add(Label(m.Label + (m.Detail.Length > 0 ? "\n" + m.Detail : ""), 10));
            _sondes.Add(f);
        }
        _sondes.DataHasChanged(); Redraw();
    }
    private void RebuildSarsat()
    {
        _sarsat.Clear();
        foreach (var m in _sarsatM.Values)
        {
            var core = m.IsTest ? new Color(255, 179, 0) : new Color(220, 32, 32);   // détresse rouge, test/exercice ambre
            var halo = new GeometryFeature { Geometry = ToPoint(m.Lat, m.Lon) };
            halo.Styles.Add(new SymbolStyle { SymbolType = SymbolType.Ellipse, SymbolScale = 2.4,
                                              Fill = new Brush(new Color(core.R, core.G, core.B, 55)), Outline = new Pen(new Color(core.R, core.G, core.B, 130), 1) });
            _sarsat.Add(halo);
            var pt = new GeometryFeature { Geometry = ToPoint(m.Lat, m.Lon) };
            pt["key"] = m.Key; pt["layer"] = "SARSAT";
            pt.Styles.Add(new ImageStyle { Image = CrashPlaneImage, SymbolScale = 0.35 });
            pt.Styles.Add(Label($"406 {m.Label}" + (m.IsTest ? " (test)" : "") + "\n" + m.Detail, 18));
            _sarsat.Add(pt);
        }
        _sarsat.DataHasChanged(); Redraw();
    }
    private static LabelStyle Label(string text, int dy) => new()
    {
        Text = text, ForeColor = Color.Black, BackColor = new Brush(new Color(255, 255, 255, 205)),
        Offset = new Offset(0, dy), Font = new Font { Size = 10 },
        HorizontalAlignment = LabelStyle.HorizontalAlignmentEnum.Center, VerticalAlignment = LabelStyle.VerticalAlignmentEnum.Top,
        CollisionDetection = false,
    };
    private void Redraw() { Try(() => Map.RefreshGraphics()); Changed?.Invoke(); }

    private static bool Plausible(double lat, double lon) => !(lat == 0 && lon == 0) && Math.Abs(lat) <= 90 && Math.Abs(lon) <= 180;
    private static MPoint ToWorld(double lat, double lon) { var (x, y) = SphericalMercator.FromLonLat(lon, lat); return new MPoint(x, y); }
    private static Point ToPoint(double lat, double lon) { var (x, y) = SphericalMercator.FromLonLat(lon, lat); return new Point(x, y); }
    private static void Try(Action a) { try { a(); } catch { /* viewport pas encore prêt */ } }
}
