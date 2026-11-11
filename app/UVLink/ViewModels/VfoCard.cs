using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace UVLink.ViewModels;

// Une « carte » VFO (A ou B) : affiche l'état du poste (fréquence, RX/TX,
// S-mètre) ET porte les réglages, envoyés dès modification (MainViewModel).
// Les pastilles (mode, squelch, excursion, BYP, puissance) basculent au toucher.
public sealed partial class VfoCard : ObservableObject
{
    public static readonly string[] ModulationNames = { "FM", "AM", "USB", "BYP", "RAW" };   // codes SET_VFO 0..4 (BYP/RAW : hôte H30)
    public static readonly string[] PowerNames = { "Haute", "Moyenne", "Basse" };
    private readonly Action<VfoCard> _changed;
    public Action<VfoCard>? SelectTx;                 // posé par MainViewModel
    public CancellationTokenSource? Debounce;         // regroupement des envois (MainViewModel.Debounced)

    public VfoCard(int index, Action<VfoCard> changed)
    {
        Index = index; _changed = changed;
        Segments = new(Enumerable.Range(1, 15).Select(i => new Segment(i)));
    }
    public int Index { get; }
    public string Name => Index == 0 ? "VFO A" : "VFO B";

    // ---- réglages ----
    [ObservableProperty] private string _rxMhz = "145.5000";
    [ObservableProperty] private string _txMhz = "";          // vide = RX
    [ObservableProperty] private int _modulation;             // 0 FM, 1 AM, 2 USB, 3 BYP, 4 RAW
    [ObservableProperty] private int _squelch = 1;
    [ObservableProperty] private bool _narrow;                // 12,5 kHz
    [ObservableProperty] private bool _flat;                  // audio plat (drapeau hôte)
    [ObservableProperty] private int _powerIndex;             // 0 haute, 1 moyenne, 2 basse
    [ObservableProperty] private bool _editing;               // saisie de fréquence ouverte
    [ObservableProperty] private bool _editTx;                // pavé : le champ TX est celui en cours de saisie (sinon RX)
    private bool _fresh, _dirty, _typing;                     // pavé : 1re touche remplace le champ ; saisie à envoyer ; touche en cours (pas d'envoi)

    // ---- état du poste (STATUS) ----
    [ObservableProperty] private string _freqText = "---.----";
    [ObservableProperty] private string _txText = "";         // « ↑ 434.8000 » si décalé
    [ObservableProperty] private bool _isRx;                  // VFO écouté
    [ObservableProperty] private bool _isTx;                  // VFO d'émission
    [ObservableProperty] private bool _signal;
    [ObservableProperty] private string _rssiText = "";
    public ObservableCollection<Segment> Segments { get; }
    public void SetMeter(int sm, string rssi) { foreach (var seg in Segments) seg.Lit = seg.Index <= sm; RssiText = rssi; }

    public string ModulationText => ModulationNames[Math.Clamp(Modulation, 0, ModulationNames.Length - 1)];
    public string SquelchText => Squelch == 0 ? "SQ ouvert" : $"SQ {Squelch}";
    public string BandwidthText => Narrow ? "12,5 kHz" : "25 kHz";
    public string PowerText => PowerNames[Math.Clamp(PowerIndex, 0, 2)];

    partial void OnRxMhzChanged(string value) { if (!_typing) _changed(this); }
    partial void OnTxMhzChanged(string value) { if (!_typing) _changed(this); }
    partial void OnModulationChanged(int value) { OnPropertyChanged(nameof(ModulationText)); _changed(this); }
    partial void OnSquelchChanged(int value) { OnPropertyChanged(nameof(SquelchText)); _changed(this); }
    partial void OnNarrowChanged(bool value) { OnPropertyChanged(nameof(BandwidthText)); _changed(this); }
    partial void OnFlatChanged(bool value) => _changed(this);
    partial void OnPowerIndexChanged(int value) { OnPropertyChanged(nameof(PowerText)); _changed(this); }

    [RelayCommand] private void SetModulation(string? s) { if (int.TryParse(s, out var i)) Modulation = i; }
    [RelayCommand] private void SetSquelch(string? s) { if (int.TryParse(s, out var i)) Squelch = Math.Clamp(i, 0, 9); }
    [RelayCommand] private void SetPower(string? s) { if (int.TryParse(s, out var i)) PowerIndex = Math.Clamp(i, 0, 2); }
    [RelayCommand] private void ToggleNarrow() => Narrow = !Narrow;
    [RelayCommand] private void ToggleFlat() => Flat = !Flat;
    [RelayCommand] private void BeginEdit() { EditTx = false; _fresh = true; _dirty = false; Editing = true; }
    [RelayCommand] private void EndEdit() { Editing = false; if (_dirty) { _dirty = false; _changed(this); } }
    partial void OnEditTxChanged(bool value) => _fresh = true;

    // Pavé numérique de l'appli (téléphone : le clavier Android d'Avalonia n'a pas de point) :
    // chiffres et point s'ajoutent au champ en cours (RX, ou TX après la touche TX), la
    // première touche remplace l'ancienne valeur, ⌫ efface, C vide ; l'envoi au poste ne
    // se fait qu'à OK (EndEdit), pas à chaque touche.
    [RelayCommand]
    private void Key(string? k)
    {
        if (string.IsNullOrEmpty(k)) return;
        string cur = EditTx ? TxMhz : RxMhz;
        string next;
        if (k == "⌫") next = cur.Length > 0 ? cur[..^1] : cur;
        else if (k == "C") next = "";
        else
        {
            if (_fresh) cur = "";
            if (k == "." && (cur.Contains('.') || cur.Length == 0)) return;
            if (cur.Length >= 10) return;
            next = cur + k;
        }
        _fresh = false;
        if (next == (EditTx ? TxMhz : RxMhz)) return;
        _dirty = true;
        _typing = true;                                   // pas d'envoi au poste par touche, seulement à OK
        try { if (EditTx) TxMhz = next; else RxMhz = next; } finally { _typing = false; }
    }
    [RelayCommand] private void SelectAsTx() => SelectTx?.Invoke(this);
}
