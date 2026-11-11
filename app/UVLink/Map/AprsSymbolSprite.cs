using Mapsui.Styles;

namespace UVLink.Map;

/// Symbole APRS (table + code) -> cellule 24x24 des planches hessu/aprs-symbols
/// incorporées (planche 0 = table '/', 1 = table '\', 2 = jeu à surcharge pour
/// tout autre caractère de table). 16 colonnes x 6 lignes, index = code - 0x21.
/// Repris de PCT_Report (même convention).
public static class AprsSymbolSprite
{
    private const int Cell = 24, Cols = 16;
    private const string Sheet0 = "embedded://UVLink.Assets.aprs-symbols-24-0.png";
    private const string Sheet1 = "embedded://UVLink.Assets.aprs-symbols-24-1.png";
    private const string Sheet2 = "embedded://UVLink.Assets.aprs-symbols-24-2.png";
    private static readonly Dictionary<(char, char), Image> _cache = new();   // une Image par symbole : bitmap décodé gardé en cache par Mapsui

    public static Image CellImage(char table, char code)
    {
        var key = (table, code);
        if (_cache.TryGetValue(key, out var img)) return img;
        string sheet = table switch { '/' => Sheet0, '\\' => Sheet1, _ => Sheet2 };
        int c = code < '!' || code > '~' ? '/' : code;
        int idx = c - '!';
        img = new Image { Source = sheet, BitmapRegion = new BitmapRegion(idx % Cols * Cell, idx / Cols * Cell, Cell, Cell) };
        _cache[key] = img;
        return img;
    }
}
