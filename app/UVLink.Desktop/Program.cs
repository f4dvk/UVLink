using System;
using Avalonia;

namespace UVLink.Desktop;

sealed class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
#if WINDOWS
        App.AudioOutputFactory = () => new NAudioOutput();
        App.AudioInputFactory = () => new NAudioInput();
#else
        if (OperatingSystem.IsLinux())
        {
            App.TransportFactory = () => new BlueZTransport();
            if (Alsa.Available())
            {
                App.AudioOutputFactory = () => new AlsaOutput();
                App.AudioInputFactory = () => new AlsaInput();
            }
        }
#endif
        // Toute exception non gérée (threads BLE/audio compris) finit dans le journal
        // sur disque, avec sa trace : sans ça un « plantage » ne laisse rien.
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            UVLink.Services.Journal.Write($"{DateTime.Now:HH:mm:ss} EXCEPTION NON GÉRÉE : {e.ExceptionObject}");
        System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            UVLink.Services.Journal.Write($"{DateTime.Now:HH:mm:ss} exception de tâche non observée : {e.Exception}");
            e.SetObserved();
        };
        UVLink.Services.Journal.Write($"{DateTime.Now:HH:mm:ss} UVLink.Desktop démarré, journal : {UVLink.Services.Journal.Path}");
        try { BuildAvaloniaApp().StartWithClassicDesktopLifetime(args); }
        catch (Exception e)
        {
            UVLink.Services.Journal.Write($"{DateTime.Now:HH:mm:ss} EXCEPTION FATALE : {e}");
            throw;
        }
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
