using System.Text;

namespace UVLink.Services;

// Journal persistant de l'appli : ~/.local/state/uvlink/journal.log (Linux) ou
// %LOCALAPPDATA%/uvlink/journal.log. Même contenu que le panneau de l'appli,
// plus les exceptions non gérées (Program.cs). Tronqué à 2 Mo au démarrage.
public static class Journal
{
    public static readonly string Path = Init();
    private static readonly object Lock = new();

    private static string Init()
    {
        try
        {
            string dir = OperatingSystem.IsWindows()
                ? System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "uvlink")
                : System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "state", "uvlink");
            Directory.CreateDirectory(dir);
            string p = System.IO.Path.Combine(dir, "journal.log");
            if (File.Exists(p) && new FileInfo(p).Length > 2_000_000) File.Delete(p);
            File.AppendAllText(p, $"\n===== {DateTime.Now:yyyy-MM-dd HH:mm:ss} démarrage =====\n");
            return p;
        }
        catch { return ""; }
    }

    public static void Write(string line)
    {
        if (Path.Length == 0) return;
        try { lock (Lock) File.AppendAllText(Path, line + "\n", Encoding.UTF8); } catch { }
    }
}
