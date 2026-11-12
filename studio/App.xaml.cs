using Microsoft.UI.Xaml;

namespace PapyrusStudio;

public partial class App : Application
{
    public static MainWindow? Window { get; private set; }

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
        Window = new MainWindow();
        Window.Activate();
    }
}
