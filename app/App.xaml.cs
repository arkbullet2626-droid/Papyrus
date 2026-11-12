using Microsoft.UI.Xaml;
using Papyrus.Services;

namespace Papyrus;

public partial class App : Application
{
    public static MainWindow? Window { get; private set; }
    private static Mutex? _mutex;

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            // Keep the app alive on a stray error and show it instead of crashing.
            e.Handled = true;
            Window?.ShowStatus(e.Exception?.Message ?? "Something went wrong.", true);
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // One Papyrus window at a time: a second launch just brings the first one forward.
        _mutex = new Mutex(true, "PapyrusAppMutex", out bool first);
        if (!first)
        {
            Engine.BringExistingWindowForward();
            Exit();
            return;
        }

        Engine.EnsureController();      // the small tray program that owns the recorder
        Window = new MainWindow();
        Window.Activate();
    }
}
