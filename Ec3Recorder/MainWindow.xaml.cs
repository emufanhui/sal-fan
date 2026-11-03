using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace Ec3Recorder;

public partial class MainWindow : Window
{
    // One E-AC3 syncframe carries 1536 samples at 48 kHz.
    private const double SecondsPerFrame = 1536.0 / 48000.0;

    private static readonly Brush IdleBrush = new SolidColorBrush(Color.FromRgb(0x5A, 0x62, 0x72));
    private static readonly Brush LiveBrush = new SolidColorBrush(Color.FromRgb(0x3D, 0xD6, 0x8C));
    private static readonly Brush OtherBrush = new SolidColorBrush(Color.FromRgb(0xE8, 0xB1, 0x3A));
    private static readonly Brush RecordingBrush = new SolidColorBrush(Color.FromRgb(0xE5, 0x4D, 0x5A));

    private readonly Ec3RecorderService _recorder = new();
    private readonly TrackWatcher _trackWatcher = new();
    private readonly DispatcherTimer _statusTimer;
    private string? _cliPath;
    private string? _lastStatusError;
    private TrackInfo? _currentTrack;
    private string _splitDirectory = string.Empty;
    private int _trackSequence;
    private long _frames;
    private long _bytes;
    private long _fileFramesBase;
    private long _fileBytesBase;

    public MainWindow()
    {
        InitializeComponent();

        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1000) };
        _statusTimer.Tick += async (_, _) => await RefreshStatusAsync();

        _recorder.Progress += OnProgress;
        _recorder.FormatDetected += name => Log($"格式：{name}");
        _recorder.Line += OnRecorderLine;
        _recorder.RecordingExited += OnRecordingExited;
        _trackWatcher.TrackChanged += OnTrackChanged;

        Loaded += (_, _) => OnLoaded();
        Closed += (_, _) =>
        {
            _recorder.Stop();
            _trackWatcher.Dispose();
        };
    }

    private void OnLoaded()
    {
        _trackWatcher.Start();

        _cliPath = CliLocator.Find();
        if (_cliPath is null)
        {
            CliPathText.Text = "未找到 SpatialAudioLab.CLI.exe。请先运行 tools\\Build-DolbyProbe.ps1。";
            StartButton.IsEnabled = false;
            return;
        }

        CliPathText.Text = _cliPath;
        OutputPathBox.Text = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            "Music",
            "capture.ec3");
        _statusTimer.Start();
    }

    private async Task RefreshStatusAsync()
    {
        if (_cliPath is null) return;

        StreamStatus? status;
        try
        {
            status = await Ec3RecorderService.QueryStatus(_cliPath, CancellationToken.None);
            _lastStatusError = null;
        }
        catch (Exception ex)
        {
            ReportStatusError(ex.Message);
            return;
        }

        if (status is null)
        {
            ReportStatusError("ec3-status 输出无法解析（编码或格式不符）");
            return;
        }

        if (_recorder.IsRecording)
        {
            // Recording owns the lamp; the probe would only repeat what the
            // recorder is already reporting.
            return;
        }

        if (!status.Known)
        {
            SetSignal(IdleBrush, "未检测到流", "驱动尚无 IEC 61937 格式。先启动 Apple Music 播放一首 Atmos 曲目。");
            return;
        }

        if (status.IsEac3)
        {
            SetSignal(status.IsLive ? LiveBrush : OtherBrush,
                status.IsLive ? "已捕获 E-AC3 流" : "E-AC3 已协商，暂无数据",
                $"{status.Subtype} · 探测窗口写入 {status.BytesPerProbe} 字节");
            return;
        }

        SetSignal(OtherBrush, "当前不是 E-AC3", $"驱动报告 {status.Subtype}，录出来不会是 .ec3。");
    }

    /// Surfaces a status-probe failure once per distinct message. Without this
    /// a broken probe looks exactly like "nothing is playing", which sends you
    /// hunting for missing audio instead of a parsing bug.
    private void ReportStatusError(string message)
    {
        if (string.Equals(_lastStatusError, message, StringComparison.Ordinal)) return;
        _lastStatusError = message;
        Log($"状态探测失败：{message}");
        SetSignal(IdleBrush, "状态探测失败", message);
    }

    private void SetSignal(Brush brush, string title, string detail)
    {
        SignalLamp.Fill = brush;
        SignalText.Text = title;
        SignalDetail.Text = detail;
    }

    private void OnProgress(RecordProgress progress)
    {
        // Raised from the recorder's output pump, which is a thread-pool
        // thread. Touching WPF controls here corrupts the dispatcher state and
        // fails later inside the layout pass, so marshal first.
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(() => OnProgress(progress));
            return;
        }

        _frames = progress.Frames;
        _bytes = progress.Bytes;

        // The recorder reports session totals; the UI shows the current file,
        // so splits subtract the baseline captured when the file rotated.
        long fileFrames = Math.Max(0, progress.Frames - _fileFramesBase);
        long fileBytes = Math.Max(0, progress.Bytes - _fileBytesBase);

        SignalLamp.Fill = _frames > 0 ? RecordingBrush : OtherBrush;
        SignalText.Text = _frames > 0 ? "正在录制 E-AC3" : "已开始，等待 E-AC3 数据";
        SignalDetail.Text = $"carrier {FormatBytes(progress.CarrierBytes)}";

        DurationText.Text = FormatDuration(fileFrames * SecondsPerFrame);
        FramesText.Text = fileFrames.ToString();
        SizeText.Text = FormatBytes(fileBytes);
    }

    private void OnRecorderLine(string text)
    {
        // Same thread-pool origin as OnProgress; Log touches the ListBox.
        if (!Dispatcher.CheckAccess())
        {
            if (text.StartsWith("STATUS ", StringComparison.Ordinal)) return;
            Dispatcher.BeginInvoke(() => OnRecorderLine(text));
            return;
        }

        if (text.StartsWith("STATUS ", StringComparison.Ordinal)) return;
        Log(text);
    }

    private void OnTrackChanged(TrackInfo? track)
    {
        // The watcher runs on the thread pool and can still deliver a change
        // while the window is tearing down.
        if (Dispatcher.HasShutdownStarted) return;
        try
        {
            Dispatcher.Invoke(() => ApplyTrack(track));
        }
        catch (TaskCanceledException)
        {
        }
    }

    private void ApplyTrack(TrackInfo? track)
    {
        _currentTrack = track;
        TrackText.Text = track?.Display ?? "—（Apple Music 未在播放）";

        if (track is null) return;

        // Logged even when idle so it is obvious whether SMTC is tracking at all.
        Log($"曲目：{track.Display}");

        if (!_recorder.IsRecording) return;
        if (AutoSplitCheck.IsChecked != true) return;

        _trackSequence++;
        string path = UniquePath(Path.Combine(_splitDirectory,
            TrackWatcher.BuildFileName(track, _trackSequence)));

        if (!_recorder.Rotate(path))
        {
            Log("切分失败：录制进程已退出。");
            return;
        }

        OutputPathBox.Text = path;
        Log($"切分 → {Path.GetFileName(path)}");

        // Per-file counters restart; the recorder keeps session totals.
        _fileFramesBase = _frames;
        _fileBytesBase = _bytes;
        DurationText.Text = "00:00";
        FramesText.Text = "0";
        SizeText.Text = "0 MB";
    }

    /// Avoids clobbering an earlier take of the same track.
    private static string UniquePath(string path)
    {
        if (!File.Exists(path)) return path;

        string directory = Path.GetDirectoryName(path) ?? ".";
        string stem = Path.GetFileNameWithoutExtension(path);
        string extension = Path.GetExtension(path);
        for (int i = 2; i < 1000; i++)
        {
            string candidate = Path.Combine(directory, $"{stem} ({i}){extension}");
            if (!File.Exists(candidate)) return candidate;
        }
        return path;
    }

    private void OnRecordingExited()
    {
        Dispatcher.Invoke(() =>
        {
            StartButton.IsEnabled = true;
            StopButton.IsEnabled = false;
            Log("录制进程已结束。");
            _statusTimer.Start();
        });
    }

    private void OnStartClick(object sender, RoutedEventArgs e)
    {
        if (_cliPath is null) return;

        string outputPath = OutputPathBox.Text.Trim();
        if (outputPath.Length == 0)
        {
            Log("请先填写输出文件路径。");
            return;
        }

        string? directory = Path.GetDirectoryName(outputPath);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

        _frames = 0;
        _bytes = 0;
        _fileFramesBase = 0;
        _fileBytesBase = 0;
        _trackSequence = 0;
        _splitDirectory = string.IsNullOrEmpty(directory) ? "." : directory;

        // With splitting on and a track already playing, the first file is
        // named after that track instead of the generic name in the box.
        if (AutoSplitCheck.IsChecked == true && _currentTrack is not null)
        {
            _trackSequence = 1;
            outputPath = UniquePath(Path.Combine(_splitDirectory,
                TrackWatcher.BuildFileName(_currentTrack, _trackSequence)));
            OutputPathBox.Text = outputPath;
            Log($"按曲目命名：{Path.GetFileName(outputPath)}");
        }

        DurationText.Text = "00:00";
        FramesText.Text = "0";
        SizeText.Text = "0 MB";

        if (!_recorder.Start(_cliPath, outputPath))
        {
            Log("启动录制进程失败。");
            return;
        }

        // The recorder and a status probe would both open the capture device,
        // so probing stops for the duration of the recording.
        _statusTimer.Stop();
        StartButton.IsEnabled = false;
        StopButton.IsEnabled = true;
        Log($"开始录制 → {outputPath}");
    }

    private void OnStopClick(object sender, RoutedEventArgs e)
    {
        _recorder.Stop();
        StopButton.IsEnabled = false;
        Log("已发送停止指令，正在收尾写入文件...");
    }

    private void OnBrowseClick(object sender, RoutedEventArgs e)
    {
        var dialog = new SaveFileDialog
        {
            Filter = "E-AC3 裸流 (*.ec3)|*.ec3",
            DefaultExt = ".ec3",
            FileName = "capture.ec3"
        };

        if (dialog.ShowDialog(this) == true) OutputPathBox.Text = dialog.FileName;
    }

    private void Log(string text)
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(() => Log(text));
            return;
        }

        LogList.Items.Add($"[{DateTime.Now:HH:mm:ss}] {text}");
        if (LogList.Items.Count > 200) LogList.Items.RemoveAt(0);
        LogList.SelectedIndex = LogList.Items.Count - 1;
        LogList.ScrollIntoView(LogList.SelectedItem);
    }

    private static string FormatDuration(double seconds)
    {
        var span = TimeSpan.FromSeconds(seconds);
        return span.TotalHours >= 1
            ? $"{(int)span.TotalHours}:{span.Minutes:00}:{span.Seconds:00}"
            : $"{span.Minutes:00}:{span.Seconds:00}";
    }

    private static string FormatBytes(long bytes)
    {
        return bytes >= 1024 * 1024
            ? $"{bytes / (1024.0 * 1024.0):F1} MB"
            : $"{bytes / 1024.0:F0} KB";
    }
}
