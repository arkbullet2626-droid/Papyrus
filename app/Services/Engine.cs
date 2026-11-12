using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Papyrus.Services;

/// <summary>What the tray program reports about the replay buffer.</summary>
public sealed record EngineState(bool Running, bool Armed, int State, string Status);

/// <summary>The recorder settings, exactly as stored in settings.ini (shared with the tray program).</summary>
public sealed class RecorderSettings
{
    public bool AlwaysOn;
    public int HotkeyVk = 0x78;          // F9
    public int HotkeyMods = 2 | 4;       // Ctrl + Shift
    public int ClipSeconds = 30;
    public int Fps = 120;
    public int QualityHeight = 2160;
    public int CaptureMode;              // 0 = fullscreen apps and games only, 1 = whole screen
    public bool Microphone;
}

/// <summary>
/// Everything this app needs from papyrus-engine.exe: where things live, running edit jobs, reading and
/// changing the recorder's settings, and talking to the tray program that owns the recorder.
/// </summary>
public static class Engine
{
    public const int ModAlt = 1, ModControl = 2, ModShift = 4;
    private const uint WM_APP = 0x8000;
    private const uint MsgReload = WM_APP + 10, MsgArm = WM_APP + 11, MsgSave = WM_APP + 12;

    public static string ClipsDir { get; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "Papyrus");
    public static string DataDir { get; } = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Papyrus");
    private static string IniPath => Path.Combine(DataDir, "settings.ini");

    public static string EnginePath
    {
        get
        {
            string here = AppContext.BaseDirectory;
            string parent = Path.GetFullPath(Path.Combine(here, "..", "papyrus-engine.exe"));
            return File.Exists(parent) ? parent : Path.Combine(here, "papyrus-engine.exe");
        }
    }

    // --- Edit jobs -------------------------------------------------------------------------------------------
    public static async Task<int> RunAsync(IEnumerable<string> args)
    {
        var psi = new ProcessStartInfo(EnginePath) { UseShellExecute = false, CreateNoWindow = true };
        foreach (string a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi);
        if (p == null) return -1;
        await p.WaitForExitAsync();
        return p.ExitCode;
    }

    public static string LastError()
    {
        try
        {
            string f = Path.Combine(DataDir, "last-edit-error.txt");
            return File.Exists(f) ? File.ReadAllText(f) : "unknown error";
        }
        catch { return "unknown error"; }
    }

    // --- The tray program ------------------------------------------------------------------------------------
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowW(string? cls, string? title);
    [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr hwnd);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern uint GetPrivateProfileIntW(string app, string key, int def, string file);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool WritePrivateProfileStringW(string app, string key, string value, string file);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);

    private static IntPtr Controller => FindWindowW("PapyrusControl", null);

    /// <summary>Starts the tray program if it is not running yet.</summary>
    public static void EnsureController()
    {
        try
        {
            if (Controller != IntPtr.Zero || !File.Exists(EnginePath)) return;
            Process.Start(new ProcessStartInfo(EnginePath, "--tray") { UseShellExecute = false, CreateNoWindow = true });
        }
        catch { /* the Settings page shows that the recorder is not running */ }
    }

    private static bool Post(uint msg, int wparam)
    {
        IntPtr h = Controller;
        if (h == IntPtr.Zero) { EnsureController(); return false; }
        return PostMessageW(h, msg, wparam, IntPtr.Zero);
    }

    public static bool SetBuffer(bool on) => Post(MsgArm, on ? 1 : 0);
    public static bool SaveClipNow() => Post(MsgSave, 0);
    public static bool ReloadSettings() => Post(MsgReload, 0);

    public static void BringExistingWindowForward()
    {
        IntPtr h = FindWindowW("WinUIDesktopWin32WindowClass", "Papyrus");
        if (h == IntPtr.Zero) return;
        if (IsIconic(h)) ShowWindow(h, 9);       // SW_RESTORE
        SetForegroundWindow(h);
    }

    public static EngineState ReadState()
    {
        bool running = Controller != IntPtr.Zero;
        bool armed = false;
        int state = 0;
        string status = running ? "" : "The Papyrus recorder is not running.";
        if (!running) return new EngineState(false, false, 0, status);
        try
        {
            string f = Path.Combine(DataDir, "state.txt");
            if (File.Exists(f))
            {
                using var fs = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var sr = new StreamReader(fs);
                string? line;
                while ((line = sr.ReadLine()) != null)
                {
                    int eq = line.IndexOf('=');
                    if (eq < 0) continue;
                    string k = line[..eq], v = line[(eq + 1)..];
                    if (k == "armed") armed = v == "1";
                    else if (k == "state") int.TryParse(v, out state);
                    else if (k == "status") status = v;
                }
            }
        }
        catch { /* being rewritten right now: next poll gets it */ }
        return new EngineState(true, armed, state, status);
    }

    // --- Settings --------------------------------------------------------------------------------------------
    public static RecorderSettings LoadSettings()
    {
        Directory.CreateDirectory(DataDir);
        string f = IniPath;
        var s = new RecorderSettings
        {
            AlwaysOn = GetPrivateProfileIntW("papyrus", "always_on", 0, f) != 0,
            HotkeyVk = (int)GetPrivateProfileIntW("papyrus", "hotkey_vk", 0x78, f),
            HotkeyMods = (int)GetPrivateProfileIntW("papyrus", "hotkey_mods", ModControl | ModShift, f),
            ClipSeconds = (int)GetPrivateProfileIntW("papyrus", "clip_seconds", 30, f),
            Fps = (int)GetPrivateProfileIntW("papyrus", "fps", 120, f),
            QualityHeight = (int)GetPrivateProfileIntW("papyrus", "quality_height", 2160, f),
            CaptureMode = GetPrivateProfileIntW("papyrus", "capture_mode", 0, f) == 1 ? 1 : 0,
            Microphone = GetPrivateProfileIntW("papyrus", "microphone", 0, f) != 0,
        };
        if (s.HotkeyVk == 0) { s.HotkeyVk = 0x78; s.HotkeyMods = ModControl | ModShift; }
        return s;
    }

    /// <summary>Writes the settings and tells the tray program to pick them up.</summary>
    public static void SaveSettings(RecorderSettings s)
    {
        Directory.CreateDirectory(DataDir);
        string f = IniPath;
        void W(string key, int v) => WritePrivateProfileStringW("papyrus", key, v.ToString(System.Globalization.CultureInfo.InvariantCulture), f);
        W("always_on", s.AlwaysOn ? 1 : 0);
        W("hotkey_vk", s.HotkeyVk);
        W("hotkey_mods", s.HotkeyMods);
        W("clip_seconds", s.ClipSeconds);
        W("fps", s.Fps);
        W("quality_height", s.QualityHeight);
        W("capture_mode", s.CaptureMode);
        W("microphone", s.Microphone ? 1 : 0);
        ReloadSettings();
    }

    public static string HotkeyName(int vk, int mods)
    {
        string text = "";
        if ((mods & ModControl) != 0) text += "Ctrl + ";
        if ((mods & ModAlt) != 0) text += "Alt + ";
        if ((mods & ModShift) != 0) text += "Shift + ";
        string key = vk >= 0x70 && vk <= 0x87 ? "F" + (vk - 0x70 + 1)
                   : (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ? ((char)vk).ToString()
                   : ((Windows.System.VirtualKey)vk).ToString();
        return text + key;
    }

    // --- The same quality ladder the recorder uses (engine/src/tier.h) --------------------------------------------
    private static readonly int[,,] Ladder =
    {
        { { 100, 150 }, { 60, 90 }, { 40, 60 } },
        { { 50, 75 }, { 35, 50 }, { 22, 33 } },
        { { 30, 45 }, { 20, 30 }, { 12, 18 } },
        { { 16, 24 }, { 10, 15 }, { 6, 9 } },
        { { 8, 12 }, { 5, 8 }, { 3, 5 } },
    };

    /// <summary>Resolution, bitrate and memory the current settings will use on this screen.</summary>
    public static string Estimate(RecorderSettings s)
    {
        int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
        int w = sw & ~1, h = sh & ~1;
        if (s.QualityHeight > 0 && s.QualityHeight < sh)
        {
            h = s.QualityHeight & ~1;
            w = (int)(((long)sw * s.QualityHeight + sh / 2) / sh) & ~1;
        }
        long px = (long)w * h;
        int row = px > 2560L * 1440 * 11 / 10 ? 0 : px > 1920L * 1080 * 11 / 10 ? 1 : px > 1280L * 720 * 11 / 10 ? 2 : px > 854L * 480 * 11 / 10 ? 3 : 4;
        int col = s.Fps > 60 ? 0 : s.Fps > 30 ? 1 : 2;
        int mbps = Ladder[row, col, 0];
        double slabMb = ((double)mbps * 1000000 / 8 + 80000) * (s.ClipSeconds + 2) * 1.15 / 1048576.0;
        return $"Records at {w}x{h}{(s.QualityHeight > sh ? " (your screen's own size)" : "")}, {s.Fps} fps, about {mbps} Mbps. " +
               $"Memory while the buffer is on: about {slabMb + 65:0} MB.";
    }
}
