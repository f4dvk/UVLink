namespace UVLink.Services;

// Position de l'appareil qui fait tourner l'appli (GPS du téléphone sur Android,
// rien sur le bureau). Utilisée en repli quand le module GPS de l'ESP n'a pas de
// fix : la position part en SET_POS (distance / azimut APRS et sondes) et place
// « ma position » sur la carte. Implémentation : UVLink.Android/AndroidLocation.cs.
public interface ILocationService
{
    // lat, lon (degrés), précision en mètres (-1 = inconnue)
    event Action<double, double, double>? Changed;
    // null si démarré, sinon le message d'erreur (permission refusée, localisation coupée…)
    string? Start();
    void Stop();
}
