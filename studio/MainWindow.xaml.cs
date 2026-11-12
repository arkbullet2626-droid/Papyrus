using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Windows.Graphics;
using Windows.Media.Core;
using Windows.Media.Playback;

namespace PapyrusStudio;

/// <summary>
/// The whole Studio app: clip list on the left, player and editor on the right.
/// All real work on files is done by papyrus-engine.exe (--info and --edit); this window only describes the job.
/// </summary>
public sealed partial class MainWindow : Window
{
    private sealed class TrackRow
    {
        public int Number;                 // 1-based sound track number in the clip
        public string Name = "";
        public RadioButton Listen = null!;
        public CheckBox Keep = null!;
        public NumberBox Start = null!;
        public NumberBox End = null!;
        public Slider Volume = null!;
    }

    private readonly string _clipsDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "Papyrus");
    private readonly string _dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Papyrus");
    private readonly List<TrackRow> _tracks = new();
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(100) };
    private MediaPlaybackItem? _item;
    private string? _clipPath;
    private double _duration;              // seconds
    private bool _playingSelection;
    private bool _loading;

    public MainWindow()
    {
        InitializeComponent();
        try
        {
            AppWindow.Resize(new SizeInt32(1320, 900));
        }
        catch { /* cosmetic only */ }

        _timer.Tick += Timer_Tick;
        _timer.Start();
        Closed += (_, _) => { _timer.Stop(); Player.Source = null; };
        LoadClips(null);
    }

    // ---------------------------------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------------------------------
    public void ShowStatus(string message, bool error)
    {
        Status.Severity = error ? InfoBarSeverity.Error : InfoBarSeverity.Success;
        Status.Message = message;
        Status.IsOpen = true;
    }

    private static string Clock(double seconds)
    {
        if (seconds < 0) seconds = 0;
        int m = (int)(seconds / 60);
        return string.Format(CultureInfo.InvariantCulture, "{0}:{1:00.0}", m, seconds - m * 60);
    }

    private string EnginePath()
    {
        string here = AppContext.BaseDirectory;
        string parent = Path.GetFullPath(Path.Combine(here, "..", "papyrus-engine.exe"));
        return File.Exists(parent) ? parent : Path.Combine(here, "papyrus-engine.exe");
    }

    private async Task<int> RunEngineAsync(IEnumerable<string> args)
    {
        var psi = new ProcessStartInfo(EnginePath()) { UseShellExecute = false, CreateNoWindow = true };
        foreach (string a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi);
        if (p == null) return -1;
        await p.WaitForExitAsync();
        return p.ExitCode;
    }

    private string LastEngineError()
    {
        try
        {
            string f = Path.Combine(_dataDir, "last-edit-error.txt");
            return File.Exists(f) ? File.ReadAllText(f) : "unknown error";
        }
        catch { return "unknown error"; }
    }

    private TimeSpan Position
    {
        get => Player.MediaPlayer?.PlaybackSession?.Position ?? TimeSpan.Zero;
        set { if (Player.MediaPlayer?.PlaybackSession != null) Player.MediaPlayer.PlaybackSession.Position = value; }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Clip list
    // ---------------------------------------------------------------------------------------------------------
    private void LoadClips(string? select)
    {
        ClipList.Items.Clear();
        if (!Directory.Exists(_clipsDir)) return;
        var files = new DirectoryInfo(_clipsDir).GetFiles("*.mp4").OrderByDescending(f => f.LastWriteTime).ToList();
        ListViewItem? toSelect = null;
        foreach (var f in files)
        {
            var panel = new StackPanel { Spacing = 2, Padding = new Thickness(0, 6, 0, 6) };
            panel.Children.Add(new TextBlock { Text = Path.GetFileNameWithoutExtension(f.Name), TextTrimming = TextTrimming.CharacterEllipsis });
            panel.Children.Add(new TextBlock
            {
                Text = string.Format(CultureInfo.CurrentCulture, "{0:g}  ·  {1:0.0} MB", f.LastWriteTime, f.Length / 1048576.0),
                Opacity = 0.65,
                FontSize = 12,
            });
            var item = new ListViewItem { Content = panel, Tag = f.FullName };
            ClipList.Items.Add(item);
            if (select != null && string.Equals(f.FullName, select, StringComparison.OrdinalIgnoreCase)) toSelect = item;
        }
        if (toSelect != null) ClipList.SelectedItem = toSelect;
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => LoadClips(_clipPath);

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(_clipsDir);
            Process.Start(new ProcessStartInfo(_clipsDir) { UseShellExecute = true });
        }
        catch (Exception ex) { ShowStatus(ex.Message, true); }
    }

    private async void ClipList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ClipList.SelectedItem is ListViewItem item && item.Tag is string path) await OpenClipAsync(path);
    }

    // ---------------------------------------------------------------------------------------------------------
    // Opening a clip
    // ---------------------------------------------------------------------------------------------------------
    private void SetEditorEnabled(bool on)
    {
        StartSlider.IsEnabled = on; EndSlider.IsEnabled = on;
        StartHereButton.IsEnabled = on; EndHereButton.IsEnabled = on;
        PlaySelectionButton.IsEnabled = on; SaveButton.IsEnabled = on; DeleteButton.IsEnabled = on;
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
        Directory.CreateDirectory(_dataDir);
        string infoFile = Path.Combine(_dataDir, "studio-info.txt");
        try { File.Delete(infoFile); } catch { }
        int code = await RunEngineAsync(new[] { "--info", path, "--out", infoFile });
        if (_clipPath != path) return;                     // the user already picked another clip
        if (code != 0 || !File.Exists(infoFile))
        {
            Player.Source = null;
            ShowStatus("This clip cannot be opened: " + LastEngineError(), true);
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

        StartSlider.Maximum = Math.Max(0, Math.Floor(_duration) - 1);
        StartSlider.Value = 0;
        EndSlider.Maximum = _duration;
        EndSlider.Value = _duration;
        UpdateTrimLabels();

        _item = new MediaPlaybackItem(MediaSource.CreateFromUri(new Uri(path)));
        Player.Source = _item;
        SetEditorEnabled(true);
        _loading = false;
    }

    private void AddTrackRow(int number, string name)
    {
        var row = new TrackRow { Number = number, Name = name };
        var grid = new Grid { ColumnSpacing = 12 };
        foreach (double w in new double[] { 130, 90, 80, 130, 130 }) grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(w) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var title = new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        row.Listen = new RadioButton { Content = "Listen", GroupName = "ListenTrack", IsChecked = _tracks.Count == 0, MinWidth = 0, Tag = _tracks.Count };
        row.Listen.Checked += Listen_Checked;
        row.Keep = new CheckBox { Content = "Keep", IsChecked = true, MinWidth = 0 };
        row.Start = new NumberBox { Header = "Start (s)", Minimum = 0, Maximum = _duration, Value = 0, SmallChange = 1, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
        row.End = new NumberBox { Header = "End (s)", Minimum = 0, Maximum = Math.Ceiling(_duration), Value = Math.Round(_duration, 1), SmallChange = 1, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact };
        row.Volume = new Slider { Header = "Volume 100%", Minimum = 0, Maximum = 200, Value = 100, StepFrequency = 5 };
        row.Volume.ValueChanged += (s, _) => { if (s is Slider sl) sl.Header = string.Format(CultureInfo.InvariantCulture, "Volume {0:0}%", sl.Value); };

        Grid.SetColumn(title, 0); Grid.SetColumn(row.Listen, 1); Grid.SetColumn(row.Keep, 2);
        Grid.SetColumn(row.Start, 3); Grid.SetColumn(row.End, 4); Grid.SetColumn(row.Volume, 5);
        grid.Children.Add(title); grid.Children.Add(row.Listen); grid.Children.Add(row.Keep);
        grid.Children.Add(row.Start); grid.Children.Add(row.End); grid.Children.Add(row.Volume);
        TracksPanel.Children.Add(grid);
        _tracks.Add(row);
    }

    private void Listen_Checked(object sender, RoutedEventArgs e)
    {
        try
        {
            if (sender is RadioButton rb && rb.Tag is int index && _item != null && index < _item.AudioTracks.Count)
                _item.AudioTracks.SelectedIndex = index;
        }
        catch (Exception ex) { ShowStatus("Could not switch the sound track: " + ex.Message, true); }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Trimming
    // ---------------------------------------------------------------------------------------------------------
    private void UpdateTrimLabels()
    {
        StartLabel.Text = Clock(StartSlider.Value);
        EndLabel.Text = Clock(EndSlider.Value);
        LengthLabel.Text = "New length: " + Clock(Math.Max(0, EndSlider.Value - StartSlider.Value));
    }

    private void Trim_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading || StartLabel == null || EndLabel == null) return;
        if (EndSlider.Value < StartSlider.Value + 1)
        {
            if (ReferenceEquals(sender, StartSlider)) EndSlider.Value = Math.Min(_duration, StartSlider.Value + 1);
            else StartSlider.Value = Math.Max(0, Math.Floor(EndSlider.Value - 1));
        }
        UpdateTrimLabels();
    }

    private void StartHere_Click(object sender, RoutedEventArgs e) => StartSlider.Value = Math.Floor(Position.TotalSeconds);

    private void EndHere_Click(object sender, RoutedEventArgs e) => EndSlider.Value = Math.Round(Position.TotalSeconds, 1);

    private void PlaySelection_Click(object sender, RoutedEventArgs e)
    {
        if (Player.MediaPlayer == null) return;
        Position = TimeSpan.FromSeconds(StartSlider.Value);
        _playingSelection = true;
        Player.MediaPlayer.Play();
    }

    private void Timer_Tick(object? sender, object e)
    {
        if (!_playingSelection || Player.MediaPlayer == null) return;
        if (Position.TotalSeconds >= EndSlider.Value)
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
        string output = Path.Combine(_clipsDir, string.Format(CultureInfo.InvariantCulture, "{0}-edit-{1:HHmmss}.mp4", name, DateTime.Now));

        var args = new List<string>
        {
            "--edit", source, "--out", output,
            "--start", ((long)(StartSlider.Value * 1000)).ToString(CultureInfo.InvariantCulture),
            "--end", ((long)(EndSlider.Value * 1000)).ToString(CultureInfo.InvariantCulture),
        };
        foreach (var t in _tracks)
        {
            double start = double.IsNaN(t.Start.Value) ? 0 : t.Start.Value;
            double end = double.IsNaN(t.End.Value) ? _duration : t.End.Value;
            args.Add("--atrack");
            args.Add(string.Format(CultureInfo.InvariantCulture, "{0}:{1}:{2}:{3}:{4}", t.Number, t.Keep.IsChecked == true ? 1 : 0,
                                   (long)(start * 1000), (long)(end * 1000), (int)t.Volume.Value));
        }

        SaveButton.IsEnabled = false;
        Busy.IsActive = true;
        int code = await RunEngineAsync(args);
        Busy.IsActive = false;
        SaveButton.IsEnabled = true;
        if (code == 0 && File.Exists(output))
        {
            ShowStatus("Saved " + Path.GetFileName(output), false);
            LoadClips(output);
        }
        else ShowStatus("The clip could not be saved: " + LastEngineError(), true);
    }

    private async void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (_clipPath == null) return;
        string path = _clipPath;
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
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
        if (RecycleBin.Send(path)) { ShowStatus("Moved to the Recycle Bin.", false); ClipTitle.Text = "Pick a clip on the left."; }
        else ShowStatus("The clip could not be deleted. Is it open in another program?", true);
        _tracks.Clear();
        TracksPanel.Children.Clear();
        LoadClips(null);
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
