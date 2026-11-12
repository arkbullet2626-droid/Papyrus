using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Navigation;
using Papyrus.Controls;
using Papyrus.Services;
using Windows.Media.Core;
using Windows.Media.Playback;

namespace Papyrus.Pages;

/// <summary>
/// The editor for one clip: player, clip length, and a row per sound track. Opened by clicking a clip on the
/// Clips page. All real work on files is done by papyrus-engine.exe (--info and --edit).
/// </summary>
public sealed partial class EditorPage : Page
{
    public EditorPage()
    {
        InitializeComponent();
        _videoBar = new RangeBar(Windows.UI.Color.FromArgb(255, 230, 60, 60), true) { StartStep = 1, MinLength = 1 };
        _videoBar.RangeChanged += UpdateTrimLabels;
        _videoBar.SeekRequested += Seek;
        VideoBarHost.Children.Add(_videoBar);
        _timer.Tick += Timer_Tick;
        Unloaded += (_, _) => { _timer.Stop(); _playingSelection = false; try { Player.MediaPlayer?.Pause(); } catch { } Player.Source = null; };
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        _timer.Start();
        if (e.Parameter is string path) await OpenClipAsync(path);
    }

    private void Back_Click(object sender, RoutedEventArgs e) => App.Window?.ShowClips(_clipPath);

    private sealed class TrackRow
    {
        public int Number;                 // 1-based sound track number in the clip
        public string Name = "";
        public RadioButton Listen = null!;
        public CheckBox Keep = null!;
        public RangeBar Bar = null!;
        public Slider Volume = null!;
    }

    private readonly List<TrackRow> _tracks = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(100) };
    private MediaPlaybackItem? _item;
    private string? _clipPath;
    private double _duration;              // seconds
    private bool _playingSelection;
    private bool _loading;
    private RangeBar _videoBar = null!;
    private static readonly Windows.UI.Color[] TrackColors =
    {
        Windows.UI.Color.FromArgb(255, 76, 175, 80), Windows.UI.Color.FromArgb(255, 30, 215, 96),
        Windows.UI.Color.FromArgb(255, 114, 137, 218), Windows.UI.Color.FromArgb(255, 255, 167, 38),
    };

    // ---------------------------------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------------------------------
    private static string Clock(double seconds)
    {
        if (seconds < 0) seconds = 0;
        int m = (int)(seconds / 60);
        return string.Format(CultureInfo.InvariantCulture, "{0}:{1:00.0}", m, seconds - m * 60);
    }

    private TimeSpan Position
    {
        get => Player.MediaPlayer?.PlaybackSession?.Position ?? TimeSpan.Zero;
        set { if (Player.MediaPlayer?.PlaybackSession != null) Player.MediaPlayer.PlaybackSession.Position = value; }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Opening a clip
    // ---------------------------------------------------------------------------------------------------------
    private void SetEditorEnabled(bool on)
    {
        _videoBar.IsHitTestVisible = on;
        PlaySelectionButton.IsEnabled = on; ResetButton.IsEnabled = on; SaveButton.IsEnabled = on; DeleteButton.IsEnabled = on;
    }

    private void Seek(double seconds)
    {
        _playingSelection = false;
        Position = TimeSpan.FromSeconds(seconds);
    }

    private async Task OpenClipAsync(string path)
    {
        _loading = true;
        _playingSelection = false;
        _clipPath = path;
        _tracks.Clear();
        TracksPanel.Children.Clear();
        SetEditorEnabled(false);
        ClipTitle.Text = Path.GetFileName(path);

        // Ask the engine what is in the file.
        Directory.CreateDirectory(Engine.DataDir);
        string infoFile = Path.Combine(Engine.DataDir, "studio-info.txt");
        try { File.Delete(infoFile); } catch { }
        int code = await Engine.RunAsync(new[] { "--info", path, "--out", infoFile });
        if (_clipPath != path) return;                     // the user already picked another clip
        if (code != 0 || !File.Exists(infoFile))
        {
            Player.Source = null;
            App.Window?.ShowStatus("This clip cannot be opened: " + Engine.LastError(), true);
            _loading = false;
            return;
        }

        _duration = 0;
        int width = 0, height = 0, fps = 0;
        foreach (string line in File.ReadAllLines(infoFile))
        {
            int eq = line.IndexOf('=');
            if (eq < 0) continue;
            string key = line[..eq], val = line[(eq + 1)..];
            if (key == "durationMs") _duration = double.Parse(val, CultureInfo.InvariantCulture) / 1000.0;
            else if (key == "width") width = int.Parse(val, CultureInfo.InvariantCulture);
            else if (key == "height") height = int.Parse(val, CultureInfo.InvariantCulture);
            else if (key == "fps") fps = int.Parse(val, CultureInfo.InvariantCulture);
            else if (key == "audio")
            {
                string[] parts = val.Split('|');
                if (parts.Length >= 2) AddTrackRow(int.Parse(parts[0], CultureInfo.InvariantCulture), parts[1]);
            }
        }
        NoTracksLabel.Visibility = _tracks.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ClipTitle.Text = string.Format(CultureInfo.InvariantCulture, "{0}   ·   {1}x{2}, {3} fps, {4}", Path.GetFileName(path), width, height, fps, Clock(_duration));

        _videoBar.Reset(_duration);
        foreach (var t in _tracks) t.Bar.Reset(_duration);
        UpdateTrimLabels();

        _item = new MediaPlaybackItem(MediaSource.CreateFromUri(new Uri(path)));
        Player.Source = _item;
        SetEditorEnabled(true);
        _loading = false;

        // Waveforms arrive a moment later; the editor is usable meanwhile.
        var peaks = await Engine.PeaksAsync(path);
        if (_clipPath != path) return;
        foreach (var t in _tracks)
            if (peaks.TryGetValue(t.Number, out byte[]? data)) t.Bar.SetPeaks(data);
    }

    private void AddTrackRow(int number, string name)
    {
        var row = new TrackRow { Number = number, Name = name };
        var color = TrackColors[_tracks.Count % TrackColors.Length];

        var header = new Grid { ColumnSpacing = 16 };
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(140) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(110) });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var title = new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        row.Listen = new RadioButton { Content = "Listen", GroupName = "ListenTrack", IsChecked = _tracks.Count == 0, MinWidth = 0, Tag = _tracks.Count };
        row.Listen.Checked += Listen_Checked;
        row.Keep = new CheckBox { Content = "Keep", IsChecked = true, MinWidth = 0 };
        var volumeLabel = new TextBlock { Text = "Volume 100%", VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };
        row.Volume = new Slider { Minimum = 0, Maximum = 200, Value = 100, StepFrequency = 5, VerticalAlignment = VerticalAlignment.Center, MaxWidth = 320, HorizontalAlignment = HorizontalAlignment.Stretch };
        row.Volume.ValueChanged += (s, _) => { if (s is Slider sl) volumeLabel.Text = string.Format(CultureInfo.InvariantCulture, "Volume {0:0}%", sl.Value); };

        Grid.SetColumn(title, 0); Grid.SetColumn(row.Listen, 1); Grid.SetColumn(row.Keep, 2); Grid.SetColumn(volumeLabel, 3); Grid.SetColumn(row.Volume, 4);
        header.Children.Add(title); header.Children.Add(row.Listen); header.Children.Add(row.Keep); header.Children.Add(volumeLabel); header.Children.Add(row.Volume);

        row.Bar = new RangeBar(color, false) { MinLength = 0.2 };
        row.Bar.Reset(_duration > 0 ? _duration : 1);
        row.Bar.SeekRequested += Seek;
        // A track that is left out is shown faded.
        row.Keep.Checked += (_, _) => row.Bar.Opacity = 1;
        row.Keep.Unchecked += (_, _) => row.Bar.Opacity = 0.35;

        var block = new StackPanel { Spacing = 6 };
        block.Children.Add(header);
        block.Children.Add(row.Bar);
        TracksPanel.Children.Add(block);
        _tracks.Add(row);
    }

    private void Listen_Checked(object sender, RoutedEventArgs e)
    {
        try
        {
            if (sender is RadioButton rb && rb.Tag is int index && _item != null && index < _item.AudioTracks.Count)
                _item.AudioTracks.SelectedIndex = index;
        }
        catch (Exception ex) { App.Window?.ShowStatus("Could not switch the sound track: " + ex.Message, true); }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Trimming
    // ---------------------------------------------------------------------------------------------------------
    private void UpdateTrimLabels()
    {
        LengthLabel.Text = string.Format(CultureInfo.InvariantCulture, "{0}  to  {1}     new length {2}",
                                         Clock(_videoBar.Start), Clock(_videoBar.End), Clock(Math.Max(0, _videoBar.End - _videoBar.Start)));
    }

    private void Reset_Click(object sender, RoutedEventArgs e)
    {
        _videoBar.Reset(_duration);
        foreach (var t in _tracks) t.Bar.Reset(_duration);
        UpdateTrimLabels();
    }

    private void PlaySelection_Click(object sender, RoutedEventArgs e)
    {
        if (Player.MediaPlayer == null) return;
        Position = TimeSpan.FromSeconds(_videoBar.Start);
        _playingSelection = true;
        Player.MediaPlayer.Play();
    }

    private void Timer_Tick(object? sender, object e)
    {
        if (Player.MediaPlayer == null || _clipPath == null) return;
        double now = Position.TotalSeconds;
        _videoBar.SetPlayhead(now);
        foreach (var t in _tracks) t.Bar.SetPlayhead(now);
        if (_playingSelection && now >= _videoBar.End)
        {
            Player.MediaPlayer.Pause();
            _playingSelection = false;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Saving and deleting
    // ---------------------------------------------------------------------------------------------------------
    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        if (_clipPath == null) return;
        string source = _clipPath;
        string name = Path.GetFileNameWithoutExtension(source);
        string output = Path.Combine(Engine.ClipsDir, string.Format(CultureInfo.InvariantCulture, "{0}-edit-{1:HHmmss}.mp4", name, DateTime.Now));

        var args = new List<string>
        {
            "--edit", source, "--out", output,
            "--start", ((long)(_videoBar.Start * 1000)).ToString(CultureInfo.InvariantCulture),
            "--end", ((long)(_videoBar.End * 1000)).ToString(CultureInfo.InvariantCulture),
        };
        foreach (var t in _tracks)
        {
            double start = t.Bar.Start;
            double end = t.Bar.End;
            args.Add("--atrack");
            args.Add(string.Format(CultureInfo.InvariantCulture, "{0}:{1}:{2}:{3}:{4}", t.Number, t.Keep.IsChecked == true ? 1 : 0,
                                   (long)(start * 1000), (long)(end * 1000), (int)t.Volume.Value));
        }

        SaveButton.IsEnabled = false;
        Busy.IsActive = true;
        int code = await Engine.RunAsync(args);
        Busy.IsActive = false;
        SaveButton.IsEnabled = true;
        if (code == 0 && File.Exists(output))
        {
            App.Window?.ShowStatus("Saved " + Path.GetFileName(output) + ". It is open in the editor now.", false);
            await OpenClipAsync(output);
        }
        else App.Window?.ShowStatus("The clip could not be saved: " + Engine.LastError(), true);
    }

    private async void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (_clipPath == null) return;
        string path = _clipPath;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Delete this clip?",
            Content = Path.GetFileName(path) + " will be moved to the Recycle Bin.",
            PrimaryButtonText = "Delete",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;

        Player.Source = null;
        _item = null;
        _clipPath = null;
        SetEditorEnabled(false);
        await Task.Delay(300);                              // let the player let go of the file
        if (RecycleBin.Send(path)) App.Window?.ShowStatus("Moved to the Recycle Bin.", false);
        else App.Window?.ShowStatus("The clip could not be deleted. Is it open in another program?", true);
        App.Window?.ShowClips();
    }
}

/// <summary>Moves a file to the Recycle Bin instead of deleting it for good.</summary>
internal static class RecycleBin
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SHFILEOPSTRUCT
    {
        public IntPtr hwnd;
        public uint wFunc;
        public string pFrom;
        public string? pTo;
        public ushort fFlags;
        [MarshalAs(UnmanagedType.Bool)] public bool fAnyOperationsAborted;
        public IntPtr hNameMappings;
        public string? lpszProgressTitle;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHFileOperation(ref SHFILEOPSTRUCT op);

    public static bool Send(string path)
    {
        const uint FO_DELETE = 3;
        const ushort FOF_SILENT = 0x0004, FOF_NOCONFIRMATION = 0x0010, FOF_ALLOWUNDO = 0x0040, FOF_NOERRORUI = 0x0400;
        var op = new SHFILEOPSTRUCT
        {
            wFunc = FO_DELETE,
            pFrom = path + "\0\0",
            fFlags = (ushort)(FOF_SILENT | FOF_NOCONFIRMATION | FOF_ALLOWUNDO | FOF_NOERRORUI),
        };
        return SHFileOperation(ref op) == 0 && !op.fAnyOperationsAborted;
    }
}
