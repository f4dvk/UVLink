using Avalonia.Controls;
using Avalonia.Input;
using UVLink.ViewModels;

namespace UVLink.Views;

public partial class VfoCardView : UserControl
{
    // Pavé numérique de l'appli à la place du clavier système (téléphone / Android, posé par
    // MainView.ApplyLayout) : les champs ne prennent plus le focus, donc pas de clavier Android.
    private bool _keypad;
    public bool Keypad
    {
        get => _keypad;
        set { _keypad = value; KeypadPanel.IsVisible = value; FreqBox.Focusable = TxBox.Focusable = !value; OkButton.IsVisible = !value; }
    }

    public VfoCardView()
    {
        InitializeComponent();
        // saisie ouverte -> focus sur le champ ; champ quitté -> saisie fermée (la valeur est déjà poussée par LostFocus)
        DataContextChanged += (_, _) =>
        {
            if (DataContext is VfoCard c)
                c.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(VfoCard.Editing) && c.Editing && !_keypad) { FreqBox.Focus(); FreqBox.SelectAll(); } };
        };
        FreqBox.LostFocus += (_, _) => { if (!TxBox.IsFocused && DataContext is VfoCard c) c.Editing = false; };
        TxBox.LostFocus += (_, _) => { if (!FreqBox.IsFocused && DataContext is VfoCard c) c.Editing = false; };
    }
}
