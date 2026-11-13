using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Papyrus.Services;
using Windows.System;
using Windows.UI.Core;

namespace Papyrus.Pages;

/// <summary>
/// Recorder settings. Every change is written to settings.ini at once and the tray program is told to reload,
/// so there is no Save button. The switch and status line follow what the tray program reports.
/// </summary>
public sealed partial class SettingsPage : Page
{
    private readonly DispatcherTimer _poll = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private RecorderSettings _settings = new();
    private bool _loading = true;
    private bool _capturingHotkey;
    private DateTime _ignoreStateUntil = DateTime.MinValue;

    public SettingsPage()
    {
        InitializeComponent();
        _poll.Tick += (_, _) => RefreshState();
        Loaded += (_, _) => { LoadSettings(); RefreshState(); _poll.Start(); };
        Unloaded += (_, _) => _poll.Stop();
    }

    private static void SelectByTag(ComboBox box, int value)
    {
        foreach (object o in box.Items)
            if (o is ComboBoxItem item && item.Tag is string tag && tag == value.ToString(CultureInfo.InvariantCulture)) { box.SelectedItem = item; return; }
        box.SelectedIndex = 0;
    }

    private static int TagOf(ComboBox box, int fallback) =>
        box.SelectedItem is ComboBoxItem item && item.Tag is string tag && int.TryParse(tag, out int v) ? v : fallback;

    private void LoadSettings()
    {
        _loading = true;
        _settings = Engine.LoadSettings();
        ModeBox.SelectedIndex = _settings.CaptureMode == 1 ? 1 : 0;
        SelectByTag(LengthBox, _settings.ClipSeconds);
        SelectByTag(FpsBox, _settings.Fps);
        SelectByTag(QualityBox, _settings.QualityHeight);
        MicSwitch.IsOn = _settings.Microphone;
        AlwaysOnSwitch.IsOn = _settings.AlwaysOn;
        HotkeyButton.Content = Engine.HotkeyName(_settings.HotkeyVk, _settings.HotkeyMods);
        EstimateText.Text = Engine.Estimate(_settings);
        ClipsLocationText.Text = Engine.ClipsDir;
        var v = Assembly.GetExecutingAssembly().GetName().Version;
        VersionText.Text = v == null ? "Papyrus" : $"Papyrus {v.Major}.{v.Minor}.{v.Build}  ·  Opifex Studios";
        _loading = false;
    }

    /// <summary>Reads the controls back into the settings object, saves, and tells the recorder.</summary>
    private void Apply()
    {
        if (_loading) return;
        _settings.CaptureMode = ModeBox.SelectedIndex == 1 ? 1 : 0;
        _settings.ClipSeconds = TagOf(LengthBox, 30);
        _settings.Fps = TagOf(FpsBox, 120);
        _settings.QualityHeight = TagOf(QualityBox, 2160);
        _settings.Microphone = MicSwitch.IsOn;
        _settings.AlwaysOn = AlwaysOnSwitch.IsOn;
        Engine.SaveSettings(_settings);
        EstimateText.Text = Engine.Estimate(_settings);
    }

    private void Setting_SelectionChanged(object sender, SelectionChangedEventArgs e) => Apply();
    private void Setting_Toggled(object sender, RoutedEventArgs e) => Apply();

    // --- Replay buffer on/off and status -----------------------------------------------------------------------------
    private void RefreshState()
    {
        var s = Engine.ReadState();
        StatusText.Text = s.Status;
        SaveNowButton.IsEnabled = s.State != 0 && s.Running;
        if (DateTime.Now >= _ignoreStateUntil && BufferSwitch.IsOn != s.Armed)
        {
            _loading = true;
            BufferSwitch.IsOn = s.Armed;
            _loading = false;
        }
    }

    private void BufferSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        Engine.EnsureController();
        Engine.SetBuffer(BufferSwitch.IsOn);
        _ignoreStateUntil = DateTime.Now.AddSeconds(1.5);      // give the recorder a moment before the poll corrects the switch
    }

    private void SaveNow_Click(object sender, RoutedEventArgs e) => Engine.SaveClipNow();

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(Engine.ClipsDir);
            Process.Start(new ProcessStartInfo(Engine.ClipsDir) { UseShellExecute = true });
        }
        catch (Exception ex) { App.Window?.ShowStatus(ex.Message, true); }
    }

    // --- Hotkey picker -----------------------------------------------------------------------------------------------
    private void HotkeyButton_Click(object sender, RoutedEventArgs e)
    {
        _capturingHotkey = true;
        HotkeyButton.Content = "Press the new keys...";
    }

    private void HotkeyButton_LostFocus(object sender, RoutedEventArgs e)
    {
        if (!_capturingHotkey) return;
        _capturingHotkey = false;
        HotkeyButton.Content = Engine.HotkeyName(_settings.HotkeyVk, _settings.HotkeyMods);
    }

    private static bool IsDown(VirtualKey key) => InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private void HotkeyButton_KeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (!_capturingHotkey) return;
        VirtualKey key = e.Key;
        if (key is VirtualKey.Control or VirtualKey.LeftControl or VirtualKey.RightControl or VirtualKey.Shift or VirtualKey.LeftShift or VirtualKey.RightShift
            or VirtualKey.Menu or VirtualKey.LeftMenu or VirtualKey.RightMenu or VirtualKey.LeftWindows or VirtualKey.RightWindows) return;   // wait for the real key
        e.Handled = true;
        _capturingHotkey = false;
        if (key != VirtualKey.Escape)
        {
            int mods = (IsDown(VirtualKey.Control) ? Engine.ModControl : 0) | (IsDown(VirtualKey.Shift) ? Engine.ModShift : 0) | (IsDown(VirtualKey.Menu) ? Engine.ModAlt : 0);
            _settings.HotkeyVk = (int)key;
            _settings.HotkeyMods = mods;
            Engine.SaveSettings(_settings);
        }
        HotkeyButton.Content = Engine.HotkeyName(_settings.HotkeyVk, _settings.HotkeyMods);
    }
}
