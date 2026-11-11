using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using UVLink.Audio;

namespace UVLink.Views;

// Spectre (haut) + chute d'eau (bas) de l'audio reçu, 0..4 kHz. Dessin direct
// (DrawingContext), rafraîchi 20 fois par seconde tant que l'audio arrive.
public sealed class SpectrumView : Control
{
    public static readonly StyledProperty<AudioSpectrum?> SourceProperty = AvaloniaProperty.Register<SpectrumView, AudioSpectrum?>(nameof(Source));
    public AudioSpectrum? Source { get => GetValue(SourceProperty); set => SetValue(SourceProperty, value); }

    private const int W = AudioSpectrum.Bins, Rows = 160;
    private const float DbMin = -90f, DbMax = 0f;
    private readonly float[] _db = new float[W];
    private readonly WriteableBitmap _fall = new(new PixelSize(W, Rows), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Opaque);
    private readonly uint[] _row = new uint[W];
    private readonly DispatcherTimer _timer;
    private bool _have;
    private int _peakBin;
    private DateTime _lastData;

    public SpectrumView()
    {
        ClipToBounds = true;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _timer.Tick += (_, _) => Tick();
        AttachedToVisualTree += (_, _) => _timer.Start();
        DetachedFromVisualTree += (_, _) => _timer.Stop();
    }

    private void Tick()
    {
        var src = Source;
        if (src == null || !src.Compute(_db)) { if (_have && (DateTime.UtcNow - _lastData).TotalSeconds > 1) InvalidateVisual(); return; }
        _have = true; _lastData = DateTime.UtcNow;
        float best = float.MinValue;
        for (int k = 1; k < W; k++) if (_db[k] > best) { best = _db[k]; _peakBin = k; }
        // chute d'eau : décale d'une ligne vers le bas, nouvelle ligne en haut
        using (var fb = _fall.Lock())
        {
            unsafe
            {
                var p = (byte*)fb.Address;
                int stride = fb.RowBytes;
                Buffer.MemoryCopy(p, p + stride, (long)stride * (Rows - 1), (long)stride * (Rows - 1));
                for (int k = 0; k < W; k++) _row[k] = Color(_db[k]);
                fixed (uint* r = _row) Buffer.MemoryCopy(r, p, stride, W * 4);
            }
        }
        InvalidateVisual();
    }

    private static uint Color(float db)
    {
        float t = Math.Clamp((db - DbMin) / (DbMax - DbMin), 0f, 1f);   // 0 noir .. 1 blanc
        byte r, g, b;
        if (t < 0.35f) { float u = t / 0.35f; r = 0; g = (byte)(40 * u); b = (byte)(60 + 180 * u); }
        else if (t < 0.7f) { float u = (t - 0.35f) / 0.35f; r = (byte)(255 * u); g = (byte)(40 + 215 * u); b = (byte)(240 - 240 * u); }
        else { float u = (t - 0.7f) / 0.3f; r = 255; g = 255; b = (byte)(255 * u); }
        return 0xFF000000u | ((uint)r << 16) | ((uint)g << 8) | b;
    }

    public override void Render(DrawingContext ctx)
    {
        var bounds = Bounds;
        double w = bounds.Width, h = bounds.Height;
        if (w < 10 || h < 10) return;
        double specH = Math.Max(60, h * 0.42), fallH = h - specH - 4;
        var bg = new SolidColorBrush(Avalonia.Media.Color.Parse("#0B1016"));
        ctx.FillRectangle(bg, new Rect(0, 0, w, h));
        var grid = new Pen(new SolidColorBrush(Avalonia.Media.Color.Parse("#1F2E3C")), 1);
        var dim = new SolidColorBrush(Avalonia.Media.Color.Parse("#6F8294"));
        var line = new Pen(new SolidColorBrush(Avalonia.Media.Color.Parse("#2FD1FF")), 1.2);
        int sr = Source?.SampleRate ?? 8000;
        double fmax = sr / 2.0;
        // grille : 500 Hz et -20 dB
        for (double f = 0; f <= fmax; f += 500)
        {
            double x = f / fmax * w;
            ctx.DrawLine(grid, new Point(x, 0), new Point(x, specH));
            if (f > 0 && f < fmax) Text(ctx, $"{f / 1000:0.#} k", x + 2, 2, dim, 10);
        }
        for (double d = -20; d > DbMin; d -= 20)
        {
            double y = (DbMax - d) / (DbMax - DbMin) * specH;
            ctx.DrawLine(grid, new Point(0, y), new Point(w, y));
            Text(ctx, $"{d:0} dB", 2, y - 12, dim, 9);
        }
        if (_have)
        {
            var geo = new StreamGeometry();
            using (var g = geo.Open())
            {
                for (int k = 0; k < W; k++)
                {
                    double x = (double)k / W * w, y = (DbMax - Math.Clamp(_db[k], DbMin, DbMax)) / (DbMax - DbMin) * specH;
                    if (k == 0) g.BeginFigure(new Point(x, y), false); else g.LineTo(new Point(x, y));
                }
                g.EndFigure(false);
            }
            ctx.DrawGeometry(null, line, geo);
            double pf = _peakBin * fmax / W;
            bool stale = (DateTime.UtcNow - _lastData).TotalSeconds > 1;
            Text(ctx, stale ? "pas d'audio" : $"crête {pf:0} Hz  {_db[_peakBin]:0} dB", w - 150, 14, new SolidColorBrush(Avalonia.Media.Color.Parse("#FFB02E")), 11);
            ctx.DrawImage(_fall, new Rect(0, 0, W, Rows), new Rect(0, specH + 4, w, fallH));
        }
        else Text(ctx, "en attente d'audio (flux RX)", 8, specH + 12, dim, 11);
    }

    private static void Text(DrawingContext ctx, string s, double x, double y, IBrush brush, double size)
    {
        var ft = new FormattedText(s, System.Globalization.CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Typeface.Default, size, brush);
        ctx.DrawText(ft, new Point(x, y));
    }
}
