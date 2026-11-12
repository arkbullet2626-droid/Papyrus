using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Papyrus.Pages;
using Papyrus.Services;
using Windows.Graphics;

namespace Papyrus;

/// <summary>The shell: title bar, navigation on the left, one page on the right.</summary>
public sealed partial class MainWindow : Window
{
    private readonly DispatcherTimer _stateTimer = new() { Interval = TimeSpan.FromMilliseconds(700) };

    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        try
        {
            AppWindow.Resize(new SizeInt32(1360, 900));
            var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
            var size = AppWindow.Size;
            AppWindow.Move(new PointInt32(area.X + (area.Width - size.Width) / 2, area.Y + (area.Height - size.Height) / 2));
        }
        catch { /* cosmetic only */ }

        // The dot in the title bar mirrors the tray icon: red recording, amber waiting, grey off.
        _stateTimer.Tick += (_, _) => UpdateStateDot();
        _stateTimer.Start();
        Closed += (_, _) => _stateTimer.Stop();
        UpdateStateDot();

        Nav.SelectedItem = ClipsItem;
    }

    private void UpdateStateDot()
    {
        var s = Engine.ReadState();
        var color = s.State == 2 ? Colors.Red : s.State == 1 ? Colors.Orange : Colors.Gray;
        StateDot.Fill = new SolidColorBrush(color);
    }

    public void ShowStatus(string message, bool error)
    {
        Status.Severity = error ? InfoBarSeverity.Error : InfoBarSeverity.Success;
        Status.Message = message;
        Status.IsOpen = true;
    }

    public void HideStatus() => Status.IsOpen = false;

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.IsSettingsSelected) ContentFrame.Navigate(typeof(SettingsPage));
        else if (args.SelectedItem is NavigationViewItem item && (item.Tag as string) == "clips") ContentFrame.Navigate(typeof(ClipsPage));
    }

    /// <summary>Opens the editor for one clip (reached by clicking a clip on the Clips page).</summary>
    public void OpenEditor(string clipPath) => ContentFrame.Navigate(typeof(EditorPage), clipPath);

    /// <summary>Back to the clip list, optionally scrolling to a clip.</summary>
    public void ShowClips(string? select = null) => ContentFrame.Navigate(typeof(ClipsPage), select);
}
