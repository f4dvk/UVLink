using CommunityToolkit.Mvvm.ComponentModel;

namespace UVLink.ViewModels;

// Un segment du S-mètre (1..15) : vert 1-9, ambre 10-12, rouge 13-15.
public partial class Segment : ObservableObject
{
    public int Index { get; }
    public bool IsGreen => Index <= 9;
    public bool IsAmber => Index is >= 10 and <= 12;
    public bool IsRed => Index >= 13;

    [ObservableProperty] private bool _lit;

    public Segment(int index) => Index = index;
}
