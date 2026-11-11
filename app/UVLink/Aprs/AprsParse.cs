using System.Globalization;
using System.Text;
using UVLink.Protocol;

namespace UVLink.Aprs;

// Analyse du champ info APRS : portage C# de firmware/lib/decoders/aprs_parse.c
// (mêmes règles que l'ESP : position non compressée, compressée, Mic-E, objet,
// item, statut, message + numéro, télémétrie, requête ADRASEC de PCT_Report),
// plus l'indicatif source, les digipeaters réels (bit H, alias WIDEn/TRACEn
// ignorés) et distance / azimut. Produit le même AprsEvent que l'EVENT de l'ESP.
public sealed class AprsInfo
{
    public string Src = "";
    public int Kind;                 // AprsEvent.KindNames : 0 autre, 1 position, 2 objet, 3 statut, 4 message, 5 télémétrie
    public char SymTable, SymCode;
    public bool HasPos, HasCourse, HasAlt, IsAdrasec;
    public int LatE5, LonE5, CourseDeg, SpeedKmh, AltM;
    public string Name = "", Text = "", MsgNo = "";
}

public static class AprsParse
{
    const int KindOther = 0, KindPosition = 1, KindObject = 2, KindStatus = 3, KindMessage = 4, KindTelemetry = 5;

    static bool IsDig(byte c) => c >= (byte)'0' && c <= (byte)'9';
    static int Digit2(ReadOnlySpan<byte> p) => (p[0] - '0') * 10 + (p[1] - '0');
    static int B91(byte c) => c >= (byte)'!' && c <= (byte)'{' ? c - '!' : 0;

    static string CopyStr(ReadOnlySpan<byte> src, int cap)
    {
        var sb = new StringBuilder();
        foreach (byte c in src)
        {
            if (c == '\r' || c == '\n' || sb.Length >= cap - 1) break;
            sb.Append(c >= 0x20 && c < 0x7F ? (char)c : '.');
        }
        return sb.ToString();
    }

    static int ParseDecE5(string s)
    {
        int i = 0; while (i < s.Length && s[i] == ' ') i++;
        int sign = 1;
        if (i < s.Length && s[i] == '-') { sign = -1; i++; } else if (i < s.Length && s[i] == '+') i++;
        long ip = 0; while (i < s.Length && char.IsAsciiDigit(s[i])) ip = ip * 10 + (s[i++] - '0');
        long frac = 0, scale = 10000;
        if (i < s.Length && s[i] == '.')
        {
            i++;
            for (int k = 0; k < 5 && i < s.Length && char.IsAsciiDigit(s[i]); k++) { frac += (s[i++] - '0') * scale; scale /= 10; }
        }
        return (int)(sign * (ip * 100000 + frac));
    }

    static void TakeCourseSpeed(AprsInfo o, ReadOnlySpan<byte> p)
    {
        if (p.Length >= 7 && IsDig(p[0]) && IsDig(p[1]) && IsDig(p[2]) && p[3] == '/' && IsDig(p[4]) && IsDig(p[5]) && IsDig(p[6]))
        {
            int crs = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
            int kt = (p[4] - '0') * 100 + (p[5] - '0') * 10 + (p[6] - '0');
            o.HasCourse = true; o.CourseDeg = crs % 360; o.SpeedKmh = (kt * 1852 + 500) / 1000;
        }
    }

    static void TakeAltitude(AprsInfo o, ReadOnlySpan<byte> p)
    {
        for (int i = 0; i + 9 <= p.Length; i++)
        {
            if (p[i] != '/' || p[i + 1] != 'A' || p[i + 2] != '=') continue;
            long ft = 0; bool ok = true;
            for (int k = 0; k < 6; k++) { if (!IsDig(p[i + 3 + k])) { ok = false; break; } ft = ft * 10 + (p[i + 3 + k] - '0'); }
            if (ok) { o.HasAlt = true; o.AltM = (int)(ft * 3048 / 10000); }
            return;
        }
    }

    static bool ParseUncompressed(ReadOnlySpan<byte> d, AprsInfo o)
    {
        if (d.Length < 19 || !(d[7] == 'N' || d[7] == 'S') || !(d[17] == 'E' || d[17] == 'W')) return false;
        if (!IsDig(d[0]) || !IsDig(d[1]) || !IsDig(d[9]) || !IsDig(d[10]) || !IsDig(d[11])) return false;
        int lat = Digit2(d) * 100000 + (Digit2(d[2..]) * 100 + Digit2(d[5..])) * 1000 / 60;
        int lonDeg = (d[9] - '0') * 100 + (d[10] - '0') * 10 + (d[11] - '0');
        int lon = lonDeg * 100000 + (Digit2(d[12..]) * 100 + Digit2(d[15..])) * 1000 / 60;
        o.LatE5 = d[7] == 'S' ? -lat : lat; o.LonE5 = d[17] == 'W' ? -lon : lon;
        o.SymTable = (char)d[8]; o.SymCode = (char)d[18]; o.HasPos = true;
        var rest = d[19..];
        TakeCourseSpeed(o, rest);
        if (o.HasCourse) rest = rest[7..];
        TakeAltitude(o, rest);
        if (rest.Length >= 9 && rest[0] == '/' && rest[1] == 'A' && rest[2] == '=') rest = rest[9..];
        o.Text = CopyStr(rest, 64);
        return true;
    }

    static bool ParseCompressed(ReadOnlySpan<byte> d, AprsInfo o)
    {
        if (d.Length < 13) return false;
        long y = (long)B91(d[1]) * 753571 + B91(d[2]) * 8281 + B91(d[3]) * 91 + B91(d[4]);
        long x = (long)B91(d[5]) * 753571 + B91(d[6]) * 8281 + B91(d[7]) * 91 + B91(d[8]);
        o.LatE5 = (int)(9000000 - y * 90 * 100 / 380926);
        o.LonE5 = (int)(-18000000 + x * 360 * 10 / 190463);
        o.SymTable = (char)d[0]; o.SymCode = (char)d[9]; o.HasPos = true;
        o.Text = CopyStr(d[13..], 64);
        TakeAltitude(o, d[13..]);
        return true;
    }

    static int MicEDigit(char c, ref int nsWe)
    {
        if (c >= '0' && c <= '9') { nsWe = 0; return c - '0'; }
        if (c >= 'A' && c <= 'J') { nsWe = 1; return c - 'A'; }
        if (c >= 'P' && c <= 'Y') { nsWe = 1; return c - 'P'; }
        if (c == 'K' || c == 'L' || c == 'Z') { nsWe = c != 'L' ? 1 : 0; return 0; }
        return 0;
    }

    static bool ParseMicE(ReadOnlySpan<byte> ax25, ReadOnlySpan<byte> info, AprsInfo o)
    {
        if (info.Length < 9) return false;
        var de = new char[6]; for (int i = 0; i < 6; i++) de[i] = (char)(ax25[i] >> 1);
        int ns = 0, ofs = 0, we = 0, dummy = 0;
        int d0 = MicEDigit(de[0], ref dummy), d1 = MicEDigit(de[1], ref dummy), d2 = MicEDigit(de[2], ref dummy);
        int d3 = MicEDigit(de[3], ref ns), d4 = MicEDigit(de[4], ref ofs), d5 = MicEDigit(de[5], ref we);
        long lat = (d0 * 10 + d1) * 100000L + ((d2 * 10 + d3) * 100000L + (d4 * 10 + d5) * 1000L) / 60;
        o.LatE5 = ns != 0 ? (int)lat : -(int)lat;
        int lonDeg = info[1] - 28; if (ofs != 0) lonDeg += 100;
        if (lonDeg >= 180 && lonDeg <= 189) lonDeg -= 80; else if (lonDeg >= 190 && lonDeg <= 199) lonDeg -= 190;
        int lonMin = info[2] - 28; if (lonMin >= 60) lonMin -= 60;
        int lonHun = info[3] - 28;
        long lon = lonDeg * 100000L + (lonMin * 100000L + lonHun * 1000L) / 60;
        o.LonE5 = we != 0 ? -(int)lon : (int)lon; o.HasPos = true;
        int sp = info[4] - 28, dc = info[5] - 28, se = info[6] - 28;
        int kt = sp * 10 + dc / 10; if (kt >= 800) kt -= 800;
        int course = (dc % 10) * 100 + se; if (course >= 400) course -= 400;
        o.HasCourse = true; o.CourseDeg = course % 360; o.SpeedKmh = (kt * 1852 + 500) / 1000;
        o.SymCode = (char)info[7]; o.SymTable = (char)info[8];
        var cm = info[9..];
        if (cm.Length >= 4 && cm[3] == '}')
        {
            long a = (long)B91(cm[0]) * 8281 + B91(cm[1]) * 91 + B91(cm[2]) - 10000;
            o.HasAlt = true; o.AltM = (int)a; cm = cm[4..];
        }
        TakeAltitude(o, cm);
        o.Text = CopyStr(cm, 64);
        return true;
    }

    static string AddrStr(ReadOnlySpan<byte> a)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < 6; i++) { char c = (char)(a[i] >> 1); if (c > ' ' && c < (char)0x7F) sb.Append(c); }
        int ssid = (a[6] >> 1) & 0x0F;
        if (ssid != 0) sb.Append('-').Append(ssid);
        return sb.ToString();
    }

    static bool GenericAlias(string c)
        => c.StartsWith("WIDE") || c.StartsWith("TRACE") || c.StartsWith("TEMP") || c == "RELAY" || c == "ECHO" || c == "GATE";

    // Digipeaters qui ont réellement répété la trame (bit H), alias génériques ignorés.
    public static string Via(ReadOnlySpan<byte> ax25)
    {
        if (ax25.Length < 14) return "";
        var parts = new List<string>();
        int pos = 14; bool ext = (ax25[13] & 1) != 0;
        while (!ext && pos + 7 <= ax25.Length)
        {
            ext = (ax25[pos + 6] & 1) != 0;
            if ((ax25[pos + 6] & 0x80) != 0)
            {
                var sb = new StringBuilder();
                for (int i = 0; i < 6; i++) { char c = (char)(ax25[pos + i] >> 1); if (c > ' ' && c < (char)0x7F) sb.Append(c); }
                string call = sb.ToString();
                if (!GenericAlias(call))
                {
                    int ssid = (ax25[pos + 6] >> 1) & 0x0F;
                    parts.Add(ssid != 0 ? $"{call}-{ssid}" : call);
                }
            }
            pos += 7;
        }
        return string.Join(",", parts);
    }

    // Trame AX.25 sans FCS -> champs ; null si trop courte.
    public static AprsInfo? Parse(ReadOnlySpan<byte> ax25)
    {
        if (ax25.Length < 16) return null;
        var o = new AprsInfo { Src = AddrStr(ax25[7..]) };
        int pos = 14; bool ext = (ax25[13] & 1) != 0;
        while (!ext && pos + 7 <= ax25.Length - 2) { ext = (ax25[pos + 6] & 1) != 0; pos += 7; }
        if (pos + 2 >= ax25.Length) return o;
        var info = ax25[(pos + 2)..];
        int n = info.Length;
        byte t = info[0];

        if (t == 0x1C || t == 0x1D || t == '`' || t == '\'') { o.Kind = KindPosition; ParseMicE(ax25, info, o); return o; }
        if (t == '!' || t == '=' || t == '@' || t == '/')
        {
            o.Kind = KindPosition;
            var d = info[1..];
            if (t == '@' || t == '/') { if (d.Length < 7) return o; d = d[7..]; }
            if (d.Length > 0 && (d[0] == '/' || d[0] == '\\' || (d[0] >= 'A' && d[0] <= 'Z') || (d[0] >= 'a' && d[0] <= 'j'))) ParseCompressed(d, o);
            else ParseUncompressed(d, o);
            return o;
        }
        if (t == ';' && n >= 30)
        {
            o.Kind = KindObject; o.Name = CopyStr(info.Slice(1, 9), 10).TrimEnd();
            var d = info[18..];
            if (d.Length > 0 && (d[0] == '/' || d[0] == '\\')) ParseCompressed(d, o); else ParseUncompressed(d, o);
            return o;
        }
        if (t == ')' && n >= 18)
        {
            o.Kind = KindObject;
            int i = 1; while (i < n && i < 10 && info[i] != '!' && info[i] != '_') i++;
            o.Name = CopyStr(info.Slice(1, i - 1), 10);
            if (i + 1 < n) ParseUncompressed(info[(i + 1)..], o);
            return o;
        }
        if (t == '>') { o.Kind = KindStatus; o.Text = CopyStr(info[1..], 64); return o; }
        if (t == ':' && n >= 11)
        {
            o.Kind = KindMessage; o.Name = CopyStr(info.Slice(1, 9), 10).TrimEnd();
            int c = 10; while (c < n && info[c] != ':') c++;
            o.Text = c + 1 < n ? CopyStr(info[(c + 1)..], 64) : "";
            for (int i = n - 1; i > c && i >= n - 7; i--)
            {
                if (info[i] != '{') continue;
                var sb = new StringBuilder();
                for (int j = i + 1; j < n && info[j] != '}' && sb.Length < 7; j++) sb.Append((char)info[j]);
                o.MsgNo = sb.ToString();
                int br = o.Text.LastIndexOf('{'); if (br >= 0 && br >= o.Text.Length - 7) o.Text = o.Text[..br];
                break;
            }
            if (o.Text.StartsWith("ADRASEC"))
            {
                int la = o.Text.IndexOf("Lat:"), lo = o.Text.IndexOf("Lon:");
                if (la >= 0 && lo >= 0) { o.LatE5 = ParseDecE5(o.Text[(la + 4)..]); o.LonE5 = ParseDecE5(o.Text[(lo + 4)..]); o.IsAdrasec = true; }
            }
            return o;
        }
        if (t == 'T') { o.Kind = KindTelemetry; o.Text = CopyStr(info, 64); return o; }
        o.Kind = KindOther; o.Text = CopyStr(info, 64);
        return o;
    }

    // Distance (m) et azimut (°) de (lat1,lon1) vers (lat2,lon2), approximation équirectangulaire comme l'ESP.
    public static bool Geo(double lat1, double lon1, double lat2, double lon2, out double distM, out int bearingDeg)
    {
        distM = 0; bearingDeg = 0;
        if (Math.Abs(lat1) > 90 || Math.Abs(lat2) > 90) return false;
        double dlat = (lat2 - lat1) * 111320.0;
        double deast = (lon2 - lon1) * 111320.0 * Math.Cos((lat1 + lat2) / 2 * Math.PI / 180);
        distM = Math.Sqrt(dlat * dlat + deast * deast);
        bearingDeg = ((int)Math.Round(Math.Atan2(deast, dlat) * 180 / Math.PI) + 360) % 360;
        return true;
    }

    // Même sélection que l'ESP (onAprsPacket) : position, ADRASEC, statut ou message -> AprsEvent ; sinon null.
    public static AprsEvent? ToEvent(byte[] ax25, double? myLat, double? myLon, string source)
    {
        var ai = Parse(ax25);
        if (ai == null) return null;
        if (!(ai.HasPos || ai.IsAdrasec || ai.Kind == KindStatus || ai.Kind == KindMessage)) return null;
        bool hasDist = false; double distM = 0; int brg = 0;
        if ((ai.HasPos || ai.IsAdrasec) && myLat is double la && myLon is double lo)
            hasDist = Geo(la, lo, ai.LatE5 / 1e5, ai.LonE5 / 1e5, out distM, out brg);
        return new AprsEvent(ai.Kind, ai.HasPos, ai.HasCourse, ai.HasAlt, hasDist, ai.IsAdrasec,
                             ai.SymTable, ai.SymCode, ai.LatE5 / 1e5, ai.LonE5 / 1e5, ai.CourseDeg, ai.SpeedKmh, ai.AltM,
                             Math.Round(distM / 100) / 10.0, brg, ai.Src, ai.Name, Via(ax25), ai.Text) { Source = source, Raw = Ax25.ToTnc2(ax25) };
    }
}
