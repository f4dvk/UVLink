using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using UVLink.ViewModels;
using UVLink.Views;

namespace UVLink;

public partial class App : Application
{
    // Transport BLE par plateforme : InTheHand par défaut (Windows, Android),
    // BlueZ direct sur Linux (fixé par UVLink.Desktop/Program.cs).
    public static Func<Ble.IUvLinkTransport> TransportFactory { get; set; } = () => new Ble.InTheHandTransport();
    // Périphériques audio par plateforme (null = pas d'audio ; fixés par Program.cs / MainActivity).
    public static Func<Audio.IAudioOutput?> AudioOutputFactory { get; set; } = () => null;
    // Android : renvoie un message si des permissions (Bluetooth, micro) manquent, et les redemande ; null ailleurs
    public static Func<string?> PermissionProblem { get; set; } = () => null;
    public static Func<Audio.IAudioInput?> AudioInputFactory { get; set; } = () => null;
    // Position de l'appareil (GPS du téléphone sur Android ; null = pas de localisation, bureau)
    public static Func<Services.ILocationService?> LocationFactory { get; set; } = () => null;
    // Android : la permission de localisation vient d'être accordée (le modèle de vue relance le GPS)
    public static Action? LocationPermissionGranted { get; set; }

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var vm = new MainViewModel { AutoConnect = desktop.Args?.Contains("--autoconnect") == true };
            if (desktop.Args?.Contains("--demo-spectre") == true) vm.DemoSpectrum();   // onglet SPECTRE avec un signal synthétique
            if (desktop.Args?.Contains("--demo-saisie") == true) { vm.Vfos[0].BeginEditCommand.Execute(null); vm.Vfos[0].KeyCommand.Execute("4"); vm.Vfos[0].KeyCommand.Execute("3"); vm.Vfos[0].KeyCommand.Execute("3"); vm.Vfos[0].KeyCommand.Execute("."); vm.Vfos[0].KeyCommand.Execute("9"); vm.Vfos[0].KeyCommand.Execute("2"); }   // saisie VFO A ouverte (pavé) pour la capture
            if (desktop.Args?.Contains("--demo-ism") == true) vm.DemoIsm();   // impulsions Nexus-TH synthétiques -> rtl_433 -> onglet ISM
            if (desktop.Args?.Contains("--demo-carte") == true) { Console.Error.WriteLine("demo : debut"); vm.DemoMap(); Console.Error.WriteLine("demo : fin"); }   // marqueurs fictifs pour vérifier l'onglet CARTE sans poste
            var win = new MainWindow { DataContext = vm };
            // --window=LARGEURxHAUTEUR : taille forcée (essai de la mise en page téléphone sur le bureau)
            var winArg = desktop.Args?.FirstOrDefault(a => a.StartsWith("--window="));
            if (winArg != null && winArg["--window=".Length..].Split('x') is { Length: 2 } wh
                && int.TryParse(wh[0], out var ww) && int.TryParse(wh[1], out var hh))
            { win.MinWidth = 300; win.MinHeight = 400; win.Width = ww; win.Height = hh; }
            else if (Environment.GetEnvironmentVariable("UVLINK_SNAPSHOT") is not { Length: > 0 })
                win.WindowState = WindowState.Maximized;        // bureau : fenêtre maximisée par défaut (demande du 2026-11-10)
            desktop.MainWindow = win;
            // --ptt=SECONDES : appuie le PTT 5 s après l'ouverture, pendant N s (test du micro sans clic)
            var pttArg = desktop.Args?.FirstOrDefault(a => a.StartsWith("--ptt="));
            if (pttArg != null && double.TryParse(pttArg["--ptt=".Length..], out var pttSec))
            {
                var t = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
                t.Tick += async (_, _) =>
                {
                    t.Stop();
                    await vm.PttDownCommand.ExecuteAsync(null);
                    await Task.Delay(TimeSpan.FromSeconds(pttSec));
                    await vm.PttUpCommand.ExecuteAsync(null);
                };
                t.Start();
            }
            // --tab=N : onglet du bas affiché au départ (0 journal, 1 décodeur, 2 APRS, 3 carte, 4 réglages, 5 spectre, 6 SSTV)
            var tabArg = desktop.Args?.FirstOrDefault(a => a.StartsWith("--tab="));
            if (tabArg != null && int.TryParse(tabArg["--tab=".Length..], out var tab)) vm.SelectedTab = tab;
            // --screenshot=chemin.png : capture de la fenêtre après --shot-delay=S secondes (10 par défaut ; tests)
            var shot = desktop.Args?.FirstOrDefault(a => a.StartsWith("--screenshot="));
            if (shot != null)
            {
                var path = shot["--screenshot=".Length..];
                double delay = 10;
                var d = desktop.Args?.FirstOrDefault(a => a.StartsWith("--shot-delay="));
                if (d != null) double.TryParse(d["--shot-delay=".Length..], out delay);
                var timer = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(delay) };
                timer.Tick += (_, _) =>
                {
                    timer.Stop();
                    var size = new PixelSize((int)win.ClientSize.Width, (int)win.ClientSize.Height);
                    using var rtb = new Avalonia.Media.Imaging.RenderTargetBitmap(size, new Vector(96, 96));
                    rtb.Render(win);
                    rtb.Save(path);
                    Console.WriteLine($"capture : {path}");
                };
                timer.Start();
            }
        }
        else if (ApplicationLifetime is ISingleViewApplicationLifetime singleView)
        {
            singleView.MainView = new MainView { DataContext = new MainViewModel() };
        }

        base.OnFrameworkInitializationCompleted();
    }
}
