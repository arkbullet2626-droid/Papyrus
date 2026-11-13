using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Papyrus.Controls;

/// <summary>
/// A timeline strip with a draggable bar on each side. The part between the bars is kept; the rest is dimmed.
/// It can draw a sound track's loudness outline behind the bars, and a line for the current playback position.
/// Clicking anywhere on the strip asks the player to jump there.
/// </summary>
public sealed class RangeBar : Grid
{
    private const double HandleWidth = 12;
    private readonly Canvas _canvas = new() { Background = new SolidColorBrush(Colors.Transparent) };
    private readonly Polygon _wave = new();
    private readonly Rectangle _dimLeft = new(), _dimRight = new(), _playhead = new() { Width = 2 };
    private readonly Border _selection = new() { BorderThickness = new Thickness(0, 2, 0, 2), IsHitTestVisible = false };
    private readonly Border _left, _right;
    private byte[]? _peaks;
    private int _drag;                     // 0 none, 1 left bar, 2 right bar
    private double _playSeconds = -1;

    public double Duration { get; private set; } = 1;
    public double Start { get; private set; }
    public double End { get; private set; } = 1;
    /// <summary>The bars move in steps of this many seconds (0 = freely). The editor uses one video frame.</summary>
    public double StartStep { get; set; }
    public double MinLength { get; set; } = 0.5;
    /// <summary>The bars cannot be dragged outside this part of the timeline (used to keep a sound track inside the clip).</summary>
    public double BoundStart { get; set; }
    public double BoundEnd { get; set; } = double.MaxValue;
    /// <summary>Times a dragged bar jumps to when it comes within a few pixels of them. It can still be dragged away again.</summary>
    public double[] SnapPoints { get; set; } = Array.Empty<double>();
    private const double SnapPixels = 10;

    public event Action? RangeChanged;
    public event Action<double>? SeekRequested;

    public RangeBar(Color accent, bool tall)
    {
        Height = tall ? 64 : 56;
        CornerRadius = new CornerRadius(6);
        Background = new SolidColorBrush(Color.FromArgb(40, 128, 128, 128));

        var accentBrush = new SolidColorBrush(accent);
        _wave.Fill = new SolidColorBrush(Color.FromArgb(200, accent.R, accent.G, accent.B));
        _wave.IsHitTestVisible = false;
        _dimLeft.Fill = _dimRight.Fill = new SolidColorBrush(Color.FromArgb(150, 0, 0, 0));
        _dimLeft.IsHitTestVisible = _dimRight.IsHitTestVisible = false;
        _selection.BorderBrush = accentBrush;
        _playhead.Fill = new SolidColorBrush(Colors.White);
        _playhead.IsHitTestVisible = false;
        _left = MakeHandle(accentBrush, new CornerRadius(6, 0, 0, 6));
        _right = MakeHandle(accentBrush, new CornerRadius(0, 6, 6, 0));

        _canvas.Children.Add(_wave);
        _canvas.Children.Add(_dimLeft);
        _canvas.Children.Add(_dimRight);
        _canvas.Children.Add(_selection);
        _canvas.Children.Add(_playhead);
        _canvas.Children.Add(_left);
        _canvas.Children.Add(_right);
        Children.Add(_canvas);

        _canvas.SizeChanged += (_, _) => Arrange();
        _canvas.PointerPressed += Canvas_PointerPressed;
        HookHandle(_left, 1);
        HookHandle(_right, 2);
    }

    private static Border MakeHandle(Brush fill, CornerRadius corners)
    {
        // The white grip line in the middle makes it read as something to drag.
        var grip = new Rectangle { Width = 2, Height = 18, Fill = new SolidColorBrush(Colors.White), RadiusX = 1, RadiusY = 1,
                                   HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        return new Border { Width = HandleWidth, Background = fill, CornerRadius = corners, Child = grip };
    }

    private void HookHandle(Border handle, int which)
    {
        handle.PointerPressed += (s, e) => { _drag = which; handle.CapturePointer(e.Pointer); e.Handled = true; };
        handle.PointerMoved += (s, e) => { if (_drag == which) DragTo(e.GetCurrentPoint(_canvas).Position.X); };
        handle.PointerReleased += (s, e) => { _drag = 0; handle.ReleasePointerCapture(e.Pointer); e.Handled = true; };
        handle.PointerCaptureLost += (s, e) => _drag = 0;
        handle.PointerCanceled += (s, e) => _drag = 0;
    }

    /// <summary>Sets the clip length and resets the bars to the whole clip.</summary>
    public void Reset(double duration)
    {
        Duration = duration > 0.1 ? duration : 0.1;
        Start = 0;
        End = Duration;
        Arrange();
    }

    public void SetRange(double start, double end)
    {
        Start = Math.Clamp(start, 0, Duration);
        End = Math.Clamp(end, Start, Duration);
        Arrange();
    }

    /// <summary>Loudness outline: one value (0-255) per equal slice of the clip.</summary>
    public void SetPeaks(byte[]? peaks)
    {
        _peaks = peaks;
        Arrange();
    }

    public void SetPlayhead(double seconds)
    {
        if (Math.Abs(seconds - _playSeconds) < 0.02) return;
        _playSeconds = seconds;
        double w = _canvas.ActualWidth;
        if (w <= 0) return;
        Canvas.SetLeft(_playhead, Math.Clamp(seconds / Duration, 0, 1) * (w - 2));
    }

    private void DragTo(double x)
    {
        double w = _canvas.ActualWidth;
        if (w <= 0) return;
        double t = Math.Clamp(x / w, 0, 1) * Duration;
        foreach (double p in SnapPoints)
            if (Math.Abs(p / Duration * w - x) <= SnapPixels) { t = p; break; }
        double lo = Math.Max(0, BoundStart), hi = Math.Min(Duration, BoundEnd);
        if (hi < lo) hi = lo;
        t = Math.Clamp(t, lo, hi);
        if (_drag == 1)
        {
            if (StartStep > 0) t = Math.Round(t / StartStep) * StartStep;
            Start = Math.Clamp(t, 0, Math.Max(0, End - MinLength));
        }
        else if (_drag == 2)
        {
            if (StartStep > 0) t = Math.Round(t / StartStep) * StartStep;
            End = Math.Clamp(t, Math.Min(Duration, Start + MinLength), Duration);
        }
        Arrange();
        RangeChanged?.Invoke();
    }

    private void Canvas_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        double w = _canvas.ActualWidth;
        if (w <= 0) return;
        SeekRequested?.Invoke(Math.Clamp(e.GetCurrentPoint(_canvas).Position.X / w, 0, 1) * Duration);
    }

    private void Arrange()
    {
        double w = _canvas.ActualWidth, h = _canvas.ActualHeight;
        if (w < HandleWidth * 2 || h <= 0) return;
        double xs = Start / Duration * w, xe = End / Duration * w;

        _dimLeft.Width = Math.Max(0, xs); _dimLeft.Height = h;
        Canvas.SetLeft(_dimLeft, 0);
        _dimRight.Width = Math.Max(0, w - xe); _dimRight.Height = h;
        Canvas.SetLeft(_dimRight, xe);

        _selection.Width = Math.Max(0, xe - xs); _selection.Height = h;
        Canvas.SetLeft(_selection, xs);

        _left.Height = _right.Height = h;
        Canvas.SetLeft(_left, Math.Clamp(xs - HandleWidth / 2, 0, w - HandleWidth));
        Canvas.SetLeft(_right, Math.Clamp(xe - HandleWidth / 2, 0, w - HandleWidth));

        _playhead.Height = h;
        Canvas.SetLeft(_playhead, Math.Clamp(Math.Max(0, _playSeconds) / Duration, 0, 1) * (w - 2));

        // Outline: the top edge left to right, then the mirrored bottom edge right to left.
        var pts = new PointCollection();
        if (_peaks != null && _peaks.Length > 1)
        {
            int n = _peaks.Length;
            double mid = h / 2, max = h / 2 - 3;
            for (int i = 0; i < n; i++) pts.Add(new Point(i * w / (n - 1), mid - Level(_peaks[i]) * max - 0.5));
            for (int i = n - 1; i >= 0; i--) pts.Add(new Point(i * w / (n - 1), mid + Level(_peaks[i]) * max + 0.5));
        }
        _wave.Points = pts;
    }

    // Square root makes quiet passages visible without loud ones filling the strip.
    private static double Level(byte v) => Math.Sqrt(v / 255.0);
}
