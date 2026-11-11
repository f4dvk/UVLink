using System;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using UVLink.ViewModels;

namespace UVLink.Views;

public partial class MainView : UserControl
{
    private MainViewModel? _bound;
    private bool _mapHomed;

    public MainView()
    {
        InitializeComponent();
        // PTT : appui = émission, relâché (ou pointeur perdu) = fin d'émission.
        // Souris : maintien. Tactile : bascule (toucher = émettre, retoucher = arrêter) :
        // sur téléphone la page défile et le geste de défilement vole la capture du
        // pointeur au bouton (PointerCaptureLost immédiat), un maintien était impossible.
        MapView.Info += OnMapInfo;
        PttButton.AddHandler(PointerPressedEvent, (_, e) =>
        {
            if (e.Pointer.Type == PointerType.Touch)
            {
                if (Vm is { } vm) { if (vm.PttOn) vm.PttUpCommand.Execute(null); else vm.PttDownCommand.Execute(null); }
                e.Handled = true;
            }
            else Vm?.PttDownCommand.Execute(null);
        }, RoutingStrategies.Tunnel);
        PttButton.AddHandler(PointerReleasedEvent, (_, e) => { if (e.Pointer.Type != PointerType.Touch) Vm?.PttUpCommand.Execute(null); }, RoutingStrategies.Tunnel);
        PttButton.PointerCaptureLost += (_, e) => { if (e.Pointer.Type != PointerType.Touch) Vm?.PttUpCommand.Execute(null); };
        Loaded += (_, _) => { if (Vm is { AutoConnect: true } vm) vm.ConnectCommand.Execute(null); };
        // UVLINK_SNAPSHOT=/chemin.png : capture de la vue 8 s après l'ouverture puis
        // fermeture (vérification visuelle hors écran, ex. onglet CARTE avec --demo-carte).
        if (Environment.GetEnvironmentVariable("UVLINK_SNAPSHOT") is { Length: > 0 } snap)
            Loaded += (_, _) => { Console.Error.WriteLine("snapshot : vue chargee, capture dans 8 s"); DispatcherTimer.RunOnce(() =>
            {
                Console.Error.WriteLine("snapshot : rendu");
                try
                {
                    double w = Bounds.Width, h = Bounds.Height;
                    // UVLINK_SNAPSHOT_SIZE=LxH : rendu de la vue à cette taille (mise en page téléphone), hors fenêtre
                    if (Environment.GetEnvironmentVariable("UVLINK_SNAPSHOT_SIZE") is { Length: > 0 } sz && sz.Split('x') is { Length: 2 } wh
                        && double.TryParse(wh[0], out var sw) && double.TryParse(wh[1], out var sh))
                    {
                        w = sw; h = sh;
                        ApplyLayout(w < 700);
                        Measure(new Size(w, h)); Arrange(new Rect(0, 0, w, h));
                    }
                    // UVLINK_SNAPSHOT_TAP=lat,lon : simule un clic sur la carte à cette position (test de la fiche station)
                    if (Environment.GetEnvironmentVariable("UVLINK_SNAPSHOT_TAP") is { Length: > 0 } tap && tap.Split(',') is { Length: 2 } ll
                        && double.TryParse(ll[0], System.Globalization.CultureInfo.InvariantCulture, out var tlat) && double.TryParse(ll[1], System.Globalization.CultureInfo.InvariantCulture, out var tlon))
                    {
                        Vm?.MapInfoCloseCommand.Execute(null);
                        var (wx, wy) = Mapsui.Projections.SphericalMercator.FromLonLat(tlon, tlat);
                        var sp = Mapsui.Extensions.ViewportExtensions.WorldToScreen(MapView.Map.Navigator.Viewport, wx, wy);
                        Console.Error.WriteLine($"snapshot : clic simule a l'ecran {sp.X:F0},{sp.Y:F0}");
                        MapSelectAt(sp);
                        Console.Error.WriteLine("snapshot : fiche = " + (Vm?.MapInfoVisible == true ? Vm.MapInfoTitle : "(aucune)"));
                        Dispatcher.UIThread.RunJobs();                             // la fiche a changé : passes de mise en page en attente
                        InvalidateMeasure(); Measure(new Size(w, h)); Arrange(new Rect(0, 0, w, h));
                    }
                    var size = new Avalonia.PixelSize((int)w, (int)h);
                    using var rtb = new Avalonia.Media.Imaging.RenderTargetBitmap(size, new Avalonia.Vector(96, 96));
                    rtb.Render(this);
                    rtb.Save(snap);
                }
                catch (Exception e) { Console.Error.WriteLine("snapshot : " + e); }
                (Avalonia.Application.Current?.ApplicationLifetime as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)?.Shutdown();
            }, TimeSpan.FromSeconds(8)); };
        // Champs texte à envoi immédiat (UpdateSourceTrigger=LostFocus) : Entrée
        // valide en retirant le focus, ce qui pousse la valeur vers le modèle.
        AddHandler(KeyDownEvent, (_, e) =>
        {
            if (e.Key == Key.Enter && e.Source is TextBox { AcceptsReturn: false })
            {
                TopLevel.GetTopLevel(this)?.FocusManager?.ClearFocus();
                e.Handled = true;
            }
        }, RoutingStrategies.Tunnel);
        // l'image SSTV est modifiée en place (WriteableBitmap) : il faut redessiner le contrôle
        DataContextChanged += (_, _) =>
        {
            if (_bound != null) { _bound.SstvUpdated -= OnSstvUpdated; _bound.PropertyChanged -= OnVmPropertyChanged; }
            _bound = Vm;
            if (_bound != null) { _bound.SstvUpdated += OnSstvUpdated; _bound.PropertyChanged += OnVmPropertyChanged; }
            if (_bound != null) MapView.Map = _bound.MapController.Map;   // onglet CARTE
            UpdateMapPage();
        };
        SizeChanged += (_, e) => ApplyLayout(e.NewSize.Width < 700);
        MapView.SizeChanged += (_, e) =>
        {
            if (e.NewSize.Width > 0 && !_mapHomed) { _mapHomed = true; Vm?.MapController.Home(); }
        };
    }

    private void OnSstvUpdated() => SstvImageControl.InvalidateVisual();

    // Clic / toucher sur la carte : l'entité la plus proche des couches APRS / Sondes / SARSAT -> fiche station
    private void OnMapInfo(object? sender, Mapsui.MapInfoEventArgs e) => MapSelectAt(e.ScreenPosition);
    private void MapSelectAt(Mapsui.Manipulations.ScreenPosition pos)
    {
        if (Vm is not { } vm) return;
        try
        {
            var layers = MapView.Map.Layers.Where(l => Array.IndexOf(vm.MapController.InfoLayerNames, l.Name) >= 0);
            var f = MapView.GetMapInfo(pos, layers)?.Feature;
            vm.MapSelect(f?["layer"] as string, f?["key"] as string);
        }
        catch (Exception ex) { Console.Error.WriteLine("carte : " + ex.Message); }
    }
    private void OnVmPropertyChanged(object? _, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.SelectedTab)) UpdateMapPage();
    }

    // Téléphone + onglet CARTE, RÉGLAGES, SSTV, POCSAG ou ISM : le contenu de l'onglet quitte le bandeau
    // du bas (480 px) pour une page plein écran (FullPage) ; le reste de la vue est
    // masqué. Le contenu est déplacé (un seul exemplaire, la carte Mapsui garde ses
    // marqueurs et son cadrage), pas recréé.
    private static readonly int[] FullPageTabs = { 3, 4, 6, 7, 8 };
    private int _fullTab = -1;
    private void UpdateMapPage()
    {
        int want = _narrow == true && Vm is { } vm && Array.IndexOf(FullPageTabs, vm.SelectedTab) >= 0 ? vm.SelectedTab : -1;
        if (want == _fullTab) return;
        if (_fullTab >= 0 && BottomTabs.Items[_fullTab] is TabItem old && FullHost.Children.Count > 0)
        {
            var c = FullHost.Children[0]; FullHost.Children.Clear(); old.Content = c;
        }
        _fullTab = want;
        if (want >= 0 && BottomTabs.Items[want] is TabItem tab && tab.Content is Control content)
        {
            tab.Content = null; FullHost.Children.Add(content);
        }
        bool full = want >= 0;
        FullPage.IsVisible = full;
        RootScroll.IsVisible = !full;
    }

    // Largeur < 700 px (téléphone en portrait) : blocs empilés, page qui défile,
    // en-tête sur deux lignes, bandeau du bas à hauteur fixe. Sinon la mise en
    // page bureau (deux colonnes, bandeau redimensionnable).
    private bool? _narrow;
    private void ApplyLayout(bool narrow)
    {
        if (_narrow == narrow) return;
        _narrow = narrow;
        RootScroll.VerticalScrollBarVisibility = narrow ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled;
        RootGrid.RowDefinitions = narrow ? new RowDefinitions("Auto,Auto,0,Auto") : new RowDefinitions("Auto,Auto,6,*");   // large : cartes à leur hauteur, bandeau du bas prend le reste
        RootGrid.Margin = new Thickness(narrow ? 4 : 8);
        HeaderGrid.RowDefinitions = narrow ? new RowDefinitions("Auto,Auto") : new RowDefinitions("Auto");
        Grid.SetRow(LedPanel, narrow ? 1 : 0);
        Grid.SetColumn(LedPanel, narrow ? 0 : 1);
        Grid.SetColumnSpan(LedPanel, narrow ? 3 : 1);
        LedPanel.Margin = new Thickness(0, narrow ? 8 : 0, 0, 0);
        SubTitle.IsVisible = !narrow;
        ConnectionLabel.IsVisible = !narrow;
        // large : carte A | carte B | émission ; étroit : A / B / émission empilés
        MainGrid.ColumnDefinitions = narrow ? new ColumnDefinitions("*") : new ColumnDefinitions("*,*,Auto");
        MainGrid.RowDefinitions = narrow ? new RowDefinitions("Auto,Auto,Auto") : new RowDefinitions("*");
        Grid.SetColumn(CardA, 0); Grid.SetRow(CardA, 0);
        Grid.SetColumn(CardB, narrow ? 0 : 1); Grid.SetRow(CardB, narrow ? 1 : 0);
        Grid.SetColumn(RightPanel, narrow ? 0 : 2); Grid.SetRow(RightPanel, narrow ? 2 : 0);
        RightPanel.Width = narrow ? double.NaN : 300;        // large : largeur fixe, les cartes se partagent le reste
        CardA.Keypad = CardB.Keypad = narrow || OperatingSystem.IsAndroid();   // pavé de l'appli : téléphone et tablette Android
        CardA.Margin = new Thickness(0, 0, narrow ? 0 : 8, narrow ? 8 : 0);
        CardB.Margin = new Thickness(0, 0, narrow ? 0 : 8, narrow ? 8 : 0);
        Splitter.IsVisible = !narrow;
        BottomPanel.Height = narrow ? 480 : double.NaN;
        BottomTabs.Classes.Set("menuonly", narrow);         // étroit : entêtes masquées, menu ≡ + nom de l'onglet
        TabTitle.IsVisible = narrow;
        PttHint.Text = narrow ? "Toucher = émettre, retoucher = arrêter" : "Maintenir PTT pour émettre";
        MapZoomButtons.VerticalAlignment = narrow ? Avalonia.Layout.VerticalAlignment.Bottom : Avalonia.Layout.VerticalAlignment.Top;   // étroit : le panneau des filtres couvre le haut
        MapZoomButtons.Margin = new Thickness(8, 8, 8, narrow ? 28 : 8);
        MapInfoBox.HorizontalAlignment = narrow ? Avalonia.Layout.HorizontalAlignment.Stretch : Avalonia.Layout.HorizontalAlignment.Right;
        MapInfoBox.MaxWidth = narrow ? double.PositiveInfinity : 460;
        // SSTV : large = image 320×256 à gauche, texte à droite ; étroit = image pleine largeur, texte dessous
        SstvGrid.ColumnDefinitions = narrow ? new ColumnDefinitions("*") : new ColumnDefinitions("Auto,*");
        SstvGrid.RowDefinitions = narrow ? new RowDefinitions("Auto,Auto,*") : new RowDefinitions("Auto,*");
        Grid.SetColumnSpan(SstvBar, narrow ? 1 : 2);
        SstvImageBorder.HorizontalAlignment = narrow ? Avalonia.Layout.HorizontalAlignment.Stretch : Avalonia.Layout.HorizontalAlignment.Left;
        SstvImageBox.Width = narrow ? double.NaN : 320;
        Grid.SetRow(SstvInfoBlock, narrow ? 2 : 1);
        Grid.SetColumn(SstvInfoBlock, narrow ? 0 : 1);
        UpdateMapPage();
    }

    private MainViewModel? Vm => DataContext as MainViewModel;
}
