using System.Buffers.Binary;
using System.Text;

namespace UVLink.Protocol;

// Notifications EVENT du firmware (docs/protocol.md, section EVENT). Premier
// octet = type, puis une charge utile little-endian propre au décodeur.
public abstract record DecoderEvent;

public sealed record LevelEvent(int Peak, int Rms, int Dc, int ClipPct, int Verdict, int AdcMin, int AdcMax, int FloorRms) : DecoderEvent
{
    public static readonly string[] VerdictNames = { "OK", "écrêtage", "polarisation ADC", "trop fort", "trop faible" };
    public string VerdictText => Verdict < VerdictNames.Length ? VerdictNames[Verdict] : $"? {Verdict}";
}

public sealed record SarsatEvent(int FrameBits, int Protocol, bool IsTest, bool HasPosition, int Country,
                                 double Lat, double Lon, string HexId, bool Repeat, int BchFixed, string RawHex) : DecoderEvent
{
    public static readonly string[] ProtocolNames =
    {
        "Inconnu", "Standard Location", "National Location", "User-Location", "Test", "ELT-DT Location",
        "Emergency EPIRB", "Emergency PLB", "RLS Location", "Ship Security",
    };
    public string ProtocolText => Protocol < ProtocolNames.Length ? ProtocolNames[Protocol] : $"proto {Protocol}";
}

public sealed record AprsEvent(int Kind, bool HasPos, bool HasCourse, bool HasAlt, bool HasRange, bool Adrasec,
                               char SymTable, char SymCode, double Lat, double Lon, int CourseDeg, int SpeedKmh, int AltM,
                               double DistKm, int BearingDeg, string Src, string Name, string Via, string Text) : DecoderEvent
{
    public static readonly string[] KindNames = { "autre", "position", "objet", "statut", "message", "télémétrie" };
    public string KindText => Kind < KindNames.Length ? KindNames[Kind] : $"? {Kind}";
    public string Source { get; init; } = "ESP";     // "ESP" (EVENT du firmware) ou "appli" (décodeur sur l'audio BLE)
    public string Raw { get; init; } = "";           // trame TNC2 complète quand on l'a (décodeur de l'appli)
}

// ISM 433/868 (fw ≥ 0.14, type 10) : impulsions OOK (porteuse / silence, µs) lues sur le souffle du
// discriminateur ; un paquet rtl_433 = une rafale, fragmentée par 44 paires (Frag 0.., Last sur la dernière).
public sealed record IsmEvent(int Frag, bool Last, int Id, (ushort Pulse, ushort Gap)[] Pairs) : DecoderEvent;

// Télémétrie (fw ≥ 0.8, 40 o) : Type 0x9F/0xAF M10, 0x20 M20 ; valeurs null = non disponibles.
public sealed record SondeEvent(int Kind, bool HasPos, double Lat, double Lon, int AltM,
                                int Type = 0, double? TempC = null, int? RhPct = null, double? SpeedH = null, double? SpeedV = null,
                                int? Heading = null, int? Sats = null, string? GpsTime = null, string? Serial = null) : DecoderEvent
{
    public string KindText => Kind switch { 1 => "RS41", 2 => "M10/M20 (en-tête)", 3 => "M10/M20", _ => $"? {Kind}" };
}

// Line = 0xFFFF : nouvelle image (effacer) ; fw ≥ 0.12 : Width x Height = taille transmise (0 = 320x256 d'un
// firmware plus ancien), Mode = SSTV_MODE_*, ModeWidth x ModeHeight = taille réelle du mode (les modes > 320 px
// sont transmis réduits de moitié). Sinon : pixels RGB565 de X0 à X0+Rgb565.Length/2-1.
public sealed record SstvLineEvent(int Line, int X0, byte[] Rgb565, int Mode = 0, int Width = 0, int Height = 0,
                                   int ModeWidth = 0, int ModeHeight = 0) : DecoderEvent;

public sealed record SstvInfoEvent(int Mode, bool Valid, bool Final, int Lines, int Syncs, int Ppm) : DecoderEvent;

public static class SstvModes
{
    public static string Name(int mode) => mode switch
    {
        1 => "Scottie 1", 2 => "Scottie 2", 3 => "Scottie DX", 4 => "Martin 1", 5 => "Martin 2",
        6 => "PD 50", 7 => "PD 90", 8 => "PD 120", 9 => "PD 160", 10 => "PD 180", 11 => "PD 240", 12 => "PD 290",
        _ => "SSTV",
    };
}

public sealed record GpsEvent(bool Valid, bool Seen, double Lat, double Lon, int SpeedKmh, int CourseDeg, int AltM,
                              int Sats, int UtcH, int UtcM, int UtcS) : DecoderEvent;

// Lignes de texte "écran du poste" (Source 2 SARSAT, 3 APRS, 4 sonde) ; Line = 255 : effacer.
public sealed record TextEvent(int Source, int Line, bool Invert, string Text) : DecoderEvent;

// POCSAG (fw ≥ 0.13, type 9) : un message = 1 à 3 notifications (Frag 0.., Last sur la dernière), le
// texte (ASCII 7 bits tel que transmis) est recollé par l'appli. Numeric : rendu en chiffres ; ToneOnly :
// appel sans message ; Partial : mots incorrigibles et texte illisible (caché par défaut côté ESP).
public sealed record PocsagEvent(int Frag, bool Last, int Baud, uint Address, int Function, bool Numeric, bool ToneOnly,
                                 bool Partial, int Corrected, int Bad, string Text) : DecoderEvent
{
    public string Source { get; init; } = "ESP";     // "ESP" (EVENT du firmware) ou "appli" (décodeur sur l'audio BLE)
}

public static class DecoderEventParser
{
    public static DecoderEvent? Parse(ReadOnlySpan<byte> d)
    {
        if (d.Length < 1) return null;
        var p = d[1..];
        try
        {
            switch (d[0])
            {
                case 1:
                    if (p.Length < 8) return null;
                    return new LevelEvent(U16(p, 0), U16(p, 2), U16(p, 4), p[6], p[7],
                                          p.Length >= 10 ? U16(p, 8) : 0, p.Length >= 12 ? U16(p, 10) : 0, p.Length >= 14 ? U16(p, 12) : 0);
                case 2:
                {
                    if (p.Length < 29) return null;
                    string id = Encoding.ASCII.GetString(p.Slice(14, 15)).TrimEnd('\0');
                    bool repeat = p.Length > 29 && p[29] != 0;
                    int bch = p.Length > 30 ? p[30] : 0;
                    string raw = p.Length > 31 ? Str(p, 31, out _) : "";
                    return new SarsatEvent(p[0], p[1], p[2] != 0, p[3] != 0, U16(p, 4), I32(p, 6) / 1e5, I32(p, 10) / 1e5, id, repeat, bch, raw);
                }
                case 3:
                {
                    if (p.Length < 22) return null;
                    byte fl = p[1];
                    int o = 22;
                    string src = Str(p, o, out o), name = Str(p, o, out o), via = Str(p, o, out o), text = Str(p, o, out _);
                    return new AprsEvent(p[0], (fl & 1) != 0, (fl & 2) != 0, (fl & 4) != 0, (fl & 8) != 0, (fl & 16) != 0,
                                         (char)p[2], (char)p[3], I32(p, 4) / 1e5, I32(p, 8) / 1e5, U16(p, 12), U16(p, 14),
                                         (short)U16(p, 16), U16(p, 18) / 10.0, U16(p, 20), src, name, via, text);
                }
                case 4:
                    if (p.Length < 14) return null;
                    if (p.Length < 40) return new SondeEvent(p[0], p[1] != 0, I32(p, 2) / 1e5, I32(p, 6) / 1e5, I32(p, 10));
                    {
                        short t10 = (short)U16(p, 15); int vh = U16(p, 18); short vv = (short)U16(p, 20); int hdg = U16(p, 22);
                        string sn = System.Text.Encoding.ASCII.GetString(p.Slice(28, 12)).TrimEnd('\0', ' ');
                        return new SondeEvent(p[0], p[1] != 0, I32(p, 2) / 1e5, I32(p, 6) / 1e5, I32(p, 10),
                            Type: p[14],
                            TempC: t10 == short.MinValue ? null : t10 / 10.0,
                            RhPct: p[17] == 255 ? null : p[17],
                            SpeedH: vh == 0xFFFF ? null : vh / 10.0,
                            SpeedV: vh == 0xFFFF ? null : vv / 10.0,
                            Heading: hdg == 0xFFFF ? null : hdg,
                            Sats: p[24] == 255 ? null : p[24],
                            GpsTime: p[25] == 255 ? null : $"{p[25]:D2}:{p[26]:D2}:{p[27]:D2}",
                            Serial: sn.Length > 0 ? sn : null);
                    }
                case 5:
                {
                    if (p.Length < 5) return null;
                    int line = U16(p, 0), x0 = U16(p, 2), n = p[4];
                    if (line == 0xFFFF && p.Length >= 12)
                        return new SstvLineEvent(line, 0, Array.Empty<byte>(), p[5], x0, U16(p, 6), U16(p, 8), U16(p, 10));
                    if (p.Length < 5 + n * 2) n = (p.Length - 5) / 2;
                    return new SstvLineEvent(line, x0, p.Slice(5, n * 2).ToArray());
                }
                case 6:
                    if (p.Length < 20) return null;
                    return new GpsEvent((p[0] & 1) != 0, (p[0] & 2) != 0, I32(p, 1) / 1e5, I32(p, 5) / 1e5, U16(p, 9), U16(p, 11),
                                        (short)U16(p, 13), p[15], p[16], p[17], p[18]);
                case 7:
                    if (p.Length < 3) return null;
                    return new TextEvent(p[0], p[1], p[2] != 0, Encoding.Latin1.GetString(p[3..]));
                case 8:
                    if (p.Length < 10) return null;
                    return new SstvInfoEvent(p[0], (p[1] & 1) != 0, (p[1] & 2) != 0, U16(p, 2), U16(p, 4), I32(p, 6));
                case 9:
                {
                    if (p.Length < 11) return null;
                    byte fl = p[8];
                    return new PocsagEvent(p[0] & 0x7F, (p[0] & 0x80) != 0, U16(p, 1), (uint)I32(p, 3), p[7],
                                           (fl & 1) != 0, (fl & 2) != 0, (fl & 4) != 0, p[9], p[10], p.Length > 11 ? Str(p, 11, out _) : "");
                }
                case 10:
                {
                    if (p.Length < 3) return null;
                    int n = (p.Length - 3) / 4;
                    var pairs = new (ushort, ushort)[n];
                    for (int i = 0; i < n; i++) pairs[i] = ((ushort)U16(p, 3 + 4 * i), (ushort)U16(p, 5 + 4 * i));
                    return new IsmEvent(p[0] & 0x7F, (p[0] & 0x80) != 0, p[1], pairs);
                }
                default:
                    return null;
            }
        }
        catch (ArgumentOutOfRangeException) { return null; }
    }

    private static int U16(ReadOnlySpan<byte> p, int o) => BinaryPrimitives.ReadUInt16LittleEndian(p[o..]);
    private static int I32(ReadOnlySpan<byte> p, int o) => BinaryPrimitives.ReadInt32LittleEndian(p[o..]);
    private static string Str(ReadOnlySpan<byte> p, int o, out int next)
    {
        int e = o;
        while (e < p.Length && p[e] != 0) e++;
        next = Math.Min(e + 1, p.Length);
        return Encoding.Latin1.GetString(p[o..e]);
    }
}

// Réassemblage des fragments DATA (frag:u8, b7 = dernier, b0-6 = index) en trames AX.25 sans FCS.
public sealed class Ax25Reassembler
{
    private readonly MemoryStream _buf = new();
    private int _expect;

    public byte[]? Feed(ReadOnlySpan<byte> frag)
    {
        if (frag.Length < 1) return null;
        int idx = frag[0] & 0x7F;
        bool last = (frag[0] & 0x80) != 0;
        if (idx == 0) { _buf.SetLength(0); _expect = 0; }
        if (idx != _expect) { _buf.SetLength(0); _expect = 0; return null; }
        _buf.Write(frag[1..]);
        _expect++;
        if (!last) return null;
        var f = _buf.ToArray();
        _buf.SetLength(0); _expect = 0;
        return f;
    }
}

public static class Ax25
{
    // "SRC>DST,PATH:info" façon moniteur TNC2 (trame sans FCS).
    public static string ToTnc2(byte[] f)
    {
        if (f.Length < 16) return Convert.ToHexString(f);
        string dst = Addr(f, 0), src = Addr(f, 7);
        var sb = new StringBuilder().Append(src).Append('>').Append(dst);
        int o = 14;
        if ((f[13] & 1) == 0)
            while (o + 7 <= f.Length) { sb.Append(',').Append(Addr(f, o)); if ((f[o + 6] & 0x80) != 0) sb.Append('*'); bool end = (f[o + 6] & 1) != 0; o += 7; if (end) break; }
        o += 2;                                   // ctrl + pid
        sb.Append(':');
        if (o < f.Length) sb.Append(Encoding.Latin1.GetString(f, o, f.Length - o).Replace('\r', ' '));
        return sb.ToString();
    }

    private static string Addr(byte[] f, int o)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < 6; i++) { char c = (char)(f[o + i] >> 1); if (c != ' ') sb.Append(c); }
        int ssid = (f[o + 6] >> 1) & 0x0F;
        if (ssid != 0) sb.Append('-').Append(ssid);
        return sb.ToString();
    }
}
