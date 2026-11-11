using Android.App;
using Android.Content.PM;
using Android.OS;
using Avalonia;
using Avalonia.Android;

namespace UVLink.Android;

[Activity(
    Label = "UVLink",
    Theme = "@style/MyTheme.NoActionBar",
    Icon = "@drawable/icon",
    MainLauncher = true,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.UiMode)]
public class MainActivity : AvaloniaMainActivity<App>
{
    protected override AppBuilder CustomizeAppBuilder(AppBuilder builder)
    {
        return base.CustomizeAppBuilder(builder)
            .WithInterFont();
    }

    protected override void OnCreate(Bundle? savedInstanceState)
    {
        // InTheHand : BluetoothGatt.connect() reçoit ce contexte ; sans lui (nous ne
        // passons pas par le sélecteur RequestDevice qui le renseigne), la connexion
        // échoue en « operation canceled » (constaté 2026-10-30, premier essai sur téléphone).
        InTheHand.AndroidActivity.CurrentActivity = this;
        App.AudioOutputFactory = () => new AndroidOutput();     // haut-parleur / micro du téléphone (AndroidAudio.cs)
        App.AudioInputFactory = () => new AndroidInput();
        // GPS du téléphone (AndroidLocation.cs) ; la permission est demandée quand l'utilisateur coche l'option
        App.LocationFactory = () => new AndroidLocation(this, () => RunOnUiThread(() =>
            RequestPermissions(new[] { global::Android.Manifest.Permission.AccessFineLocation, global::Android.Manifest.Permission.AccessCoarseLocation }, 3)));
        // rtl_433 (onglet ISM) : exécutable statique livré dans lib/<abi>/librtl_433.so (voir UVLink.Android.csproj)
        if (ApplicationInfo?.NativeLibraryDir is string libDir)
            UVLink.Ism.Rtl433Runner.PlatformExe = System.IO.Path.Combine(libDir, "librtl_433.so");
        base.OnCreate(savedInstanceState);
        // Permissions BLE à l'exécution (Android 6+) : scan + connexion sur 12+,
        // Bluetooth + localisation fine avant (le scan BLE l'exige).
        var wanted = OperatingSystem.IsAndroidVersionAtLeast(31)
            ? new[] { global::Android.Manifest.Permission.BluetoothScan, global::Android.Manifest.Permission.BluetoothConnect, global::Android.Manifest.Permission.RecordAudio }
            : new[] { global::Android.Manifest.Permission.Bluetooth, global::Android.Manifest.Permission.BluetoothAdmin, global::Android.Manifest.Permission.AccessFineLocation, global::Android.Manifest.Permission.RecordAudio };
        var missing = wanted.Where(p => CheckSelfPermission(p) != Permission.Granted).ToArray();
        if (missing.Length > 0) RequestPermissions(missing, 1);
        // Vérification à chaque demande de connexion : message explicite dans le journal
        // de l'appli si l'utilisateur a refusé, et nouvelle demande.
        App.PermissionProblem = () =>
        {
            var m = wanted.Where(p => CheckSelfPermission(p) != Permission.Granted).ToArray();
            if (m.Length == 0) return null;
            RunOnUiThread(() => RequestPermissions(m, 2));
            return "permissions Android manquantes : " + string.Join(", ", m.Select(x => x.Split('.').Last())) + " (accepter la demande, puis reconnecter ; sinon Paramètres > Applications > UVLink > Autorisations)";
        };
    }

    public override void OnRequestPermissionsResult(int requestCode, string[] permissions, Permission[] grantResults)
    {
        base.OnRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 3 && grantResults.Any(g => g == Permission.Granted)) App.LocationPermissionGranted?.Invoke();
    }
}
