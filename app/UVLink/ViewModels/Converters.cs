using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;

namespace UVLink.ViewModels;

public static class Converters
{
    public static readonly IValueConverter ConnectLabel =
        new FuncValueConverter<bool, string>(c => c ? "Déconnecter" : "Connecter");

    // bool -> pinceau : vrai = couleur passée en paramètre, faux = gris.
    public static readonly IValueConverter OnOffBrush = new OnOffBrushConverter();

    // int <-> bool pour les RadioButton (paramètre = valeur de l'option).
    public static readonly IValueConverter IntEquals = new IntEqualsConverter();

    private sealed class OnOffBrushConverter : IValueConverter
    {
        public object Convert(object? value, Type t, object? param, CultureInfo c)
            => value is true ? new SolidColorBrush(Color.Parse(param?.ToString() ?? "Green")) : new SolidColorBrush(Color.Parse("#40808080"));
        public object ConvertBack(object? value, Type t, object? param, CultureInfo c) => throw new NotSupportedException();
    }

    private sealed class IntEqualsConverter : IValueConverter
    {
        public object Convert(object? value, Type t, object? param, CultureInfo c)
            => value is int v && int.TryParse(param?.ToString(), out var p) && v == p;
        public object? ConvertBack(object? value, Type t, object? param, CultureInfo c)
            => value is true && int.TryParse(param?.ToString(), out var p) ? p : Avalonia.Data.BindingOperations.DoNothing;
    }
}
