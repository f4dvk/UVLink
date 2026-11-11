namespace UVLink.Services;

// Réglages mémorisés de l'appli : fichier texte clé=valeur à côté du journal
// (~/.local/state/uvlink/settings.txt ou %LOCALAPPDATA%/uvlink/settings.txt).
public static class Settings
{
    private static readonly Dictionary<string, string> Values = Load();
    private static readonly object Lock = new();

    private static string PathOf()
        => Journal.Path.Length == 0 ? "" : System.IO.Path.Combine(System.IO.Path.GetDirectoryName(Journal.Path) ?? ".", "settings.txt");

    private static Dictionary<string, string> Load()
    {
        var d = new Dictionary<string, string>();
        try
        {
            string p = PathOf();
            if (p.Length > 0 && File.Exists(p))
                foreach (var line in File.ReadAllLines(p))
                {
                    int i = line.IndexOf('=');
                    if (i > 0) d[line[..i]] = line[(i + 1)..];
                }
        }
        catch { }
        return d;
    }

    public static string? Get(string key) { lock (Lock) return Values.TryGetValue(key, out var v) ? v : null; }
    public static bool GetBool(string key, bool def = false) => Get(key) is { } v ? v == "1" : def;

    public static void Set(string key, string value)
    {
        lock (Lock)
        {
            Values[key] = value;
            try { string p = PathOf(); if (p.Length > 0) File.WriteAllLines(p, Values.Select(kv => kv.Key + "=" + kv.Value)); } catch { }
        }
    }
    public static void Set(string key, bool value) => Set(key, value ? "1" : "0");
}
