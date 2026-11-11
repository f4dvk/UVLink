using Android.Content;
using Android.Content.PM;
using Android.Locations;
using Android.OS;
using Android.Runtime;
using UVLink.Services;

namespace UVLink.Android;

// GPS du téléphone pour ILocationService : LocationManager, fournisseurs GPS et
// réseau (celui qui répond en premier ; le GPS affine ensuite). Mises à jour
// toutes les 5 s ou 10 m, livrées sur le thread principal. Seulement quand
// l'appli est au premier plan (pas de permission « arrière-plan »).
public sealed class AndroidLocation : Java.Lang.Object, ILocationListener, ILocationService
{
    private readonly Activity _activity;
    private readonly Action _requestPermission;
    private LocationManager? _lm;
    private bool _running;

    public AndroidLocation(Activity activity, Action requestPermission) { _activity = activity; _requestPermission = requestPermission; }

    public event Action<double, double, double>? Changed;

    public string? Start()
    {
        if (_activity.CheckSelfPermission(global::Android.Manifest.Permission.AccessFineLocation) != Permission.Granted)
        {
            _requestPermission();
            return "permission de localisation à accorder (demande affichée ; sinon Paramètres > Applications > UVLink > Autorisations)";
        }
        _lm ??= (LocationManager?)_activity.GetSystemService(Context.LocationService);
        if (_lm == null) return "pas de service de localisation";
        var providers = new[] { LocationManager.GpsProvider, LocationManager.NetworkProvider }.Where(p => _lm.IsProviderEnabled(p)).ToArray();
        if (providers.Length == 0) return "localisation désactivée sur le téléphone";
        if (_running) Stop();
        foreach (var p in providers)
        {
            _lm.RequestLocationUpdates(p, 5000, 10f, this, Looper.MainLooper);
            var last = _lm.GetLastKnownLocation(p);
            if (last != null && (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - last.Time) < 120_000) OnLocationChanged(last);   // dernière position si récente (< 2 min)
        }
        _running = true;
        return null;
    }

    public void Stop()
    {
        if (_running) _lm?.RemoveUpdates(this);
        _running = false;
    }

    public void OnLocationChanged(Location l) => Changed?.Invoke(l.Latitude, l.Longitude, l.HasAccuracy ? l.Accuracy : -1);
    public void OnProviderDisabled(string provider) { }
    public void OnProviderEnabled(string provider) { }
    public void OnStatusChanged(string? provider, [GeneratedEnum] Availability status, Bundle? extras) { }
}
