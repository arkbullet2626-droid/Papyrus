using System.Diagnostics;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Navigation;
using Papyrus.Services;
using Windows.Media.Core;
using Windows.Media.Playback;
using Windows.Storage;
using Windows.Storage.FileProperties;

namespace Papyrus.Pages;

/// <summary>
/// The clip library: a card per clip with its thumbnail. Hovering a card for two seconds plays the clip silently
/// inside the card; clicking a card opens it in the editor.
/// </summary>
public sealed partial class ClipsPage : Page
{
    private sealed class Card
    {
        public string Path = "";
        public Grid PreviewHost = null!;
        public Image Thumb = null!;
        public TextBlock Detail = null!;
    }

    private readonly DispatcherTimer _hoverTimer = new() { Interval = TimeSpan.FromSeconds(2) };
    private Card? _hoverCard;
    private MediaPlayer? _previewPlayer;
    private MediaPlayerElement? _previewElement;
    private int _loadGeneration;

    public ClipsPage()
    {
        InitializeComponent();
        _hoverTimer.Tick += HoverTimer_Tick;
        LocationText.Text = "Clips are saved in " + Engine.ClipsDir;
        Unloaded += (_, _) => { _hoverTimer.Stop(); StopPreview(); _loadGeneration++; };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _ = LoadAsync();
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => _ = LoadAsync();

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(Engine.ClipsDir);
            Process.Start(new ProcessStartInfo(Engine.ClipsDir) { UseShellExecute = true });
        }
        catch (Exception ex) { App.Window?.ShowStatus(ex.Message, true); }
    }

    private async Task LoadAsync()
    {
        int generation = ++_loadGeneration;
        StopPreview();
        Cards.Children.Clear();
        var files = Directory.Exists(Engine.ClipsDir)
            ? new DirectoryInfo(Engine.ClipsDir).GetFiles("*.mp4").OrderByDescending(f => f.LastWriteTime).ToList()
            : new List<FileInfo>();
        EmptyText.Visibility = files.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        var cards = new List<Card>();
        foreach (var f in files)
        {
            var card = BuildCard(f);
            cards.Add(card);
        }
        // Thumbnails and lengths come from Windows; load them one by one so the page appears straight away.
        foreach (var card in cards)
        {
            if (generation != _loadGeneration) return;
            try
            {
                StorageFile file = await StorageFile.GetFileFromPathAsync(card.Path);
                using (StorageItemThumbnail thumb = await file.GetThumbnailAsync(ThumbnailMode.VideosView, 640))
                {
                    if (thumb != null && generation == _loadGeneration)
                    {
                        var bmp = new BitmapImage();
                        await bmp.SetSourceAsync(thumb);
                        card.Thumb.Source = bmp;
                    }
                }
                var video = await file.Properties.GetVideoPropertiesAsync();
                if (generation == _loadGeneration && video.Duration > TimeSpan.Zero)
                    card.Detail.Text = string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}  ·  {2}", (int)video.Duration.TotalMinutes, video.Duration.Seconds, card.Detail.Text);
            }
            catch { /* a clip without a thumbnail still opens */ }
        }
    }

    private Card BuildCard(FileInfo f)
    {
        var card = new Card { Path = f.FullName };

        card.Thumb = new Image { Stretch = Stretch.UniformToFill, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        card.PreviewHost = new Grid { Height = 180, Background = new SolidColorBrush(Microsoft.UI.Colors.Black), CornerRadius = new CornerRadius(6) };
        card.PreviewHost.Children.Add(card.Thumb);

        var name = new TextBlock { Text = System.IO.Path.GetFileNameWithoutExtension(f.Name), TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(2, 8, 2, 0) };
        card.Detail = new TextBlock
        {
            Text = string.Format(CultureInfo.CurrentCulture, "{0:g}  ·  {1:0} MB", f.LastWriteTime, f.Length / 1048576.0),
            Opacity = 0.65,
            FontSize = 12,
            Margin = new Thickness(2, 2, 2, 0),
        };

        var stack = new StackPanel();
        stack.Children.Add(card.PreviewHost);
        stack.Children.Add(name);
        stack.Children.Add(card.Detail);

        var button = new Button
        {
            Content = stack,
            Padding = new Thickness(8),
            Margin = new Thickness(0, 0, 12, 12),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Top,
            Tag = card,
        };
        button.Click += Card_Click;
        button.PointerEntered += Card_PointerEntered;
        button.PointerExited += Card_PointerExited;
        button.PointerCanceled += Card_PointerExited;
        Cards.Children.Add(button);
        return card;
    }

    private void Card_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.Tag is Card card)
        {
            _hoverTimer.Stop();
            StopPreview();
            App.Window?.OpenEditor(card.Path);
        }
    }

    // --- Hover preview -------------------------------------------------------------------------------------------
    private void Card_PointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (sender is not Button b || b.Tag is not Card card) return;
        if (!ReferenceEquals(_hoverCard, card)) StopPreview();
        _hoverCard = card;
        _hoverTimer.Stop();
        _hoverTimer.Start();                       // two seconds from now, if the pointer is still here
    }

    private void Card_PointerExited(object sender, PointerRoutedEventArgs e)
    {
        _hoverTimer.Stop();
        _hoverCard = null;
        StopPreview();
    }

    private void HoverTimer_Tick(object? sender, object e)
    {
        _hoverTimer.Stop();
        if (_hoverCard == null || _previewPlayer != null) return;
        try
        {
            _previewPlayer = new MediaPlayer { IsMuted = true, IsLoopingEnabled = true };
            _previewPlayer.Source = MediaSource.CreateFromUri(new Uri(_hoverCard.Path));
            _previewElement = new MediaPlayerElement { AreTransportControlsEnabled = false, Stretch = Stretch.UniformToFill, IsHitTestVisible = false };
            _previewElement.SetMediaPlayer(_previewPlayer);
            _hoverCard.PreviewHost.Children.Add(_previewElement);
            _previewPlayer.Play();
        }
        catch { StopPreview(); }
    }

    private void StopPreview()
    {
        try
        {
            if (_previewElement != null)
            {
                if (_previewElement.Parent is Grid host) host.Children.Remove(_previewElement);
                _previewElement.SetMediaPlayer(null);
            }
            if (_previewPlayer != null)
            {
                _previewPlayer.Pause();
                _previewPlayer.Source = null;
                _previewPlayer.Dispose();
            }
        }
        catch { }
        _previewElement = null;
        _previewPlayer = null;
    }
}
