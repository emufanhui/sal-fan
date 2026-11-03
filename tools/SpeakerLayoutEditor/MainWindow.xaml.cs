using Microsoft.Win32;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
using SpeakerLayoutEditor.Services;
using SpatialAudioLab.Core.Audio;
using SpatialAudioLab.Core.Profiles;
using SpatialAudioLab.Core.Runtime;
using IOPath = System.IO.Path;

namespace SpeakerLayoutEditor;

public sealed record EndpointChoice(
    string Filter,
    string DisplayName,
    AudioEndpointDescriptor? Descriptor);

sealed record PhysicalDestinationChoice(OutputRouteDefinition Output,
                                        int ChannelIndex,
                                        string DisplayName);

public partial class MainWindow : Window {
    static readonly Color[] RouteColors = [
        Color.FromRgb(52, 120, 165),
        Color.FromRgb(192, 112, 50),
        Color.FromRgb(64, 143, 104),
        Color.FromRgb(143, 84, 153)
    ];
    static readonly Dictionary<string, (double Azimuth, double Elevation)> DefaultPositions =
        new(StringComparer.OrdinalIgnoreCase) {
            ["FL"] = (-30, 0), ["FR"] = (30, 0), ["FC"] = (0, 0), ["LFE"] = (0, 0),
            ["BL"] = (-150, 0), ["BR"] = (150, 0), ["SL"] = (-90, 0), ["SR"] = (90, 0),
            ["TFL"] = (-45, 45), ["TFR"] = (45, 45),
            ["TBL"] = (-135, 45), ["TBR"] = (135, 45)
        };

    readonly RuntimePaths runtimePaths;
    readonly ProfileRepository profileRepository;
    readonly EndpointQuery endpointQuery;
    readonly BridgeProcessService bridgeProcessService = new();
    readonly NspCaptureService nspCaptureService = new();
    readonly Eac3ProducerService eac3ProducerService = new();
    DspStageService dspStageService = null!;
    readonly ObservableCollection<StageChannelRow> dspChannels = [];
    string? dspConfigPath;
    readonly ISpatialProviderService spatialProviderService =
        new DeferredSpatialProviderService();
    readonly List<AudioEndpointDescriptor> activeEndpoints = [];
    readonly ObservableCollection<PhysicalDestinationChoice> physicalDestinationChoices = [];
    readonly DispatcherTimer bridgeStatusTimer = new() {
        Interval = TimeSpan.FromSeconds(1)
    };
    readonly DispatcherTimer meterTimer = new() {
        Interval = TimeSpan.FromMilliseconds(100)
    };
    readonly BridgeMeterReader meterReader = new();
    readonly Dictionary<string, ChannelMeterControl> channelMeters =
        new(StringComparer.OrdinalIgnoreCase);
    LayoutDocument? document;
    SpeakerDefinition? selectedSpeaker;
    string? currentPath;
    bool updatingControls;
    SpeakerDefinition? draggedSpeaker;
    Canvas? dragCanvas;
    Point dragStartPoint;
    Vector dragPointerOffset;
    bool dragActivated;
    bool updatingEndpointChoices;
    bool bridgeCommandRunning;
    bool bridgeModeSelectionInitialized;
    public ObservableCollection<EndpointChoice> EndpointChoices { get; } = [];

    public MainWindow() {
        runtimePaths = RuntimePaths.ResolveForCurrentProcess();
        runtimePaths.EnsureUserDirectories();
        dspStageService = new DspStageService(runtimePaths);
        profileRepository = new ProfileRepository(runtimePaths.ProfilesRoot);
        endpointQuery = new EndpointQuery(runtimePaths);
        InitializeComponent();
        PhysicalDestinationCombo.ItemsSource = physicalDestinationChoices;
        DspChannelList.ItemsSource = dspChannels;
        Loaded += WindowLoaded;
        Closed += (_, _) => {
            bridgeStatusTimer.Stop();
            meterTimer.Stop();
            meterReader.Dispose();
        };
        bridgeStatusTimer.Tick += (_, _) => UpdateBridgeStatus();
        meterTimer.Tick += (_, _) => UpdateBridgeMeters();
        UpdateBridgeControlLabels();
    }

    async void WindowLoaded(object sender, RoutedEventArgs e) {
        ProfileDocument? activeProfile = profileRepository.LoadActive();
        if (activeProfile is null) {
            StatusText.Text = "需要先完成扬声器初始配置";
            PathText.Text = runtimePaths.ProfilesRoot;
        } else {
            LoadProfile(activeProfile);
        }
        await RefreshEndpointsAsync();
        LoadDspStage();
        UpdateBridgeStatus();
        bridgeStatusTimer.Start();
        meterTimer.Start();
    }

    void LoadProfile(string path) {
        try {
            LayoutDocument imported = LayoutDocument.Load(path);
            ProfileDocument profile = imported.ToProfile();
            profileRepository.Save(profile);
            profileRepository.SetActive(profile.Id);
            LoadProfile(profile);
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "无法打开配置文件",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void LoadProfile(ProfileDocument profile) {
        try {
            document = LayoutDocument.FromProfile(profile);
            currentPath = IOPath.Combine(
                runtimePaths.ProfilesRoot,
                $"{profile.Id:D}.ini");
            SpeakerList.ItemsSource = document.Speakers;
            OutputGrid.ItemsSource = document.Outputs;
            LayoutNameText.Text = document.Name;
            PathText.Text = currentPath;
            PopulateRouteLegend();
            RebuildPhysicalDestinationChoices();
            RebuildEndpointChoices();
            RebuildBridgeMeters();
            SpeakerList.SelectedIndex = 0;
            StatusText.Text = "配置文件已加载";
            RedrawMaps();
            // The speaker list is what a freshly generated config would describe,
            // so switching layout changes what "rebuild" produces.
            UpdateDspSummary();
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "无法打开配置文件",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    async void RefreshEndpointsClick(object sender, RoutedEventArgs e) =>
        await RefreshEndpointsAsync();

    async Task RefreshEndpointsAsync() {
        RefreshEndpointsButton.IsEnabled = false;
        try {
            activeEndpoints.Clear();
            activeEndpoints.AddRange(await endpointQuery.QueryAsync());
            activeEndpoints.Sort((first, second) =>
                StringComparer.CurrentCultureIgnoreCase.Compare(first.Name, second.Name));
            RebuildEndpointChoices();
            StatusText.Text = document is null
                ? $"需要先完成扬声器初始配置 · " +
                  $"活动音频端点：{activeEndpoints.Count}"
                : $"活动音频端点：{activeEndpoints.Count}";
        } catch (Exception error) {
            RebuildEndpointChoices();
            StatusText.Text = document is null
                ? "需要先完成扬声器初始配置 · " +
                  $"无法刷新音频端点：{error.Message}"
                : $"无法刷新音频端点：{error.Message}";
        } finally {
            RefreshEndpointsButton.IsEnabled = true;
        }
    }

    void RebuildEndpointChoices() {
        updatingEndpointChoices = true;
        try {
            EndpointChoices.Clear();
            var duplicateNames = activeEndpoints
                .GroupBy(endpoint => endpoint.Name, StringComparer.OrdinalIgnoreCase)
                .Where(group => group.Count() > 1)
                .Select(group => group.Key)
                .ToHashSet(StringComparer.OrdinalIgnoreCase);

            foreach (AudioEndpointDescriptor endpoint in activeEndpoints) {
                string displayName = duplicateNames.Contains(endpoint.Name)
                    ? $"{endpoint.Name} [{ShortEndpointId(endpoint.Id)}]"
                    : endpoint.Name;
                EndpointChoices.Add(new EndpointChoice(endpoint.Id, displayName, endpoint));
            }

            if (document is not null) {
                foreach (OutputRouteDefinition output in document.Outputs) {
                    EndpointMatchResult match = EndpointMatcher.Match(
                        new EndpointIdentity(
                            output.EndpointId,
                            output.EndpointName,
                            output.ContainerId,
                            output.ExpectedChannels),
                        activeEndpoints);
                    if (match.Endpoint is not null) {
                        AudioEndpointDescriptor endpoint = match.Endpoint;
                        output.ConfigureEndpoint(
                            endpoint.Id,
                            endpoint.Name,
                            endpoint.ContainerId,
                            Math.Max(endpoint.MaximumChannels48k, output.Speakers.Count));
                    }
                    if (EndpointChoices.Any(choice => choice.Filter.Equals(
                            output.Endpoint, StringComparison.OrdinalIgnoreCase))) continue;
                    EndpointChoices.Add(new EndpointChoice(
                        output.Endpoint, $"{output.EndpointName}（不可用）", null));
                }
            }
            OutputGrid.Items.Refresh();
        } finally {
            updatingEndpointChoices = false;
        }
    }

    void EndpointSelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (updatingEndpointChoices || sender is not ComboBox comboBox ||
            comboBox.DataContext is not OutputRouteDefinition output ||
            comboBox.SelectedItem is not EndpointChoice choice ||
            choice.Descriptor is null) return;
        if (output.Endpoint.Equals(choice.Filter, StringComparison.OrdinalIgnoreCase)) return;
        AudioEndpointDescriptor endpoint = choice.Descriptor;
        output.ConfigureEndpoint(
            endpoint.Id,
            endpoint.Name,
            endpoint.ContainerId,
            Math.Max(endpoint.MaximumChannels48k, output.Speakers.Count));
        if (selectedSpeaker is not null &&
            ReferenceEquals(document?.OutputFor(selectedSpeaker.Name), output)) {
            DestinationEndpointText.Text = output.Endpoint;
        }
        StatusText.Text = $"音频端点已更新：{output.Name}";
    }

    void RebuildPhysicalDestinationChoices() {
        physicalDestinationChoices.Clear();
        if (document is null) return;
        foreach (OutputRouteDefinition output in document.Outputs) {
            for (int index = 0; index < output.Speakers.Count; ++index) {
                physicalDestinationChoices.Add(new PhysicalDestinationChoice(
                    output, index,
                    $"{output.Name} / {output.PhysicalChannelName(index)}（通道 {index + 1})"));
            }
        }
    }

    static string ShortEndpointId(string endpointId) {
        int closingBrace = endpointId.LastIndexOf('}');
        int start = Math.Max(0, closingBrace - 8);
        return endpointId[start..Math.Max(start, closingBrace)];
    }

    BridgeMode SelectedBridgeMode =>
        PcmModeButton.IsChecked == true ? BridgeMode.Pcm :
        NativeMatModeButton.IsChecked == true ? BridgeMode.NativeMat :
        DtsXModeButton.IsChecked == true ? BridgeMode.DtsX :
        Eac3ModeButton.IsChecked == true ? BridgeMode.Eac3 :
        NspModeButton.IsChecked == true ? BridgeMode.Nsp : BridgeMode.Atmos;

    BridgeLatencyMode SelectedBridgeLatencyMode =>
        SafeLatencyButton.IsChecked == true ? BridgeLatencyMode.Safe :
        LowLatencyButton.IsChecked == true ? BridgeLatencyMode.Low :
        BridgeLatencyMode.Balanced;

    static string BridgeName(BridgeMode mode) => mode switch {
        BridgeMode.Atmos => "Dolby Atmos（Windows）",
        BridgeMode.NativeMat => "原生 Dolby MAT",
        BridgeMode.DtsX => "DTS:X",
        BridgeMode.Pcm => "内置 PCM 7.1.4",
        BridgeMode.Nsp => "NSP 7.1.4（免驱动）",
        BridgeMode.Eac3 => "接收 E-AC-3",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };

    string BridgePidPath(BridgeMode mode) => IOPath.Combine(runtimePaths.LogsRoot, mode switch {
        BridgeMode.Atmos => "bridge-atmos.pid",
        BridgeMode.NativeMat => "bridge-native-mat.pid",
        BridgeMode.DtsX => "bridge-dtsx.pid",
        BridgeMode.Pcm => "bridge-pcm.pid",
        BridgeMode.Nsp => "bridge-nsp.pid",
        BridgeMode.Eac3 => "bridge-eac3.pid",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    });

    string BridgeLogPath(BridgeMode mode) => IOPath.Combine(runtimePaths.LogsRoot, mode switch {
        BridgeMode.Atmos => "bridge-atmos.log",
        BridgeMode.NativeMat => "bridge-native-mat.log",
        BridgeMode.DtsX => "bridge-dtsx.log",
        BridgeMode.Pcm => "bridge-pcm.log",
        BridgeMode.Nsp => "bridge-nsp.log",
        BridgeMode.Eac3 => "bridge-eac3.log",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    });

    void BridgeControlChanged(object sender, RoutedPropertyChangedEventArgs<double> e) =>
        UpdateBridgeControlLabels();

    void BridgeLatencyChecked(object sender, RoutedEventArgs e) {
        if (BridgePrebufferSlider is null) return;
        BridgePrebufferSlider.Value = SelectedBridgeLatencyMode switch {
            BridgeLatencyMode.Safe => 80,
            BridgeLatencyMode.Balanced => 40,
            BridgeLatencyMode.Low => 20,
            _ => throw new ArgumentOutOfRangeException()
        };
        UpdateBridgeControlLabels();
    }

    void UpdateBridgeControlLabels() {
        if (BridgeGainValue is null || BridgePrebufferValue is null) return;
        BridgeGainValue.Text = BridgeGainSlider.Value.ToString("0.00", CultureInfo.InvariantCulture);
        BridgePrebufferValue.Text = $"{Math.Round(BridgePrebufferSlider.Value):0} ms";
    }

    /// <summary>
    /// 低音管理（分频）。
    ///
    /// 开关和当前选中的 json 路径记在便携 Data\dsp-stage.json，真正送进引擎的
    /// 参数在那份 json 里。那份文件可以就是 AggregateASIO 正在用的那一份，两边
    /// 读到的必须是同一个东西，所以 Studio 私有的开关不往里面塞。
    /// </summary>
    void LoadDspStage() {
        DspStagePointer pointer = dspStageService.LoadPointer();
        dspConfigPath = pointer.ConfigPath ?? dspStageService.DefaultConfigPath;
        updatingControls = true;
        DspEnabledBox.IsChecked = pointer.Enabled;
        updatingControls = false;
        if (!TryOpenDspConfig(dspConfigPath) && document is not null) {
            RebuildDspRows();
        }

        UpdateDspSummary();
    }

    /// <summary>
    /// 把一份 json 读进面板。读不到不算错误 —— 那只是"还没建过"，
    /// 交给调用方去重生成。
    /// </summary>
    bool TryOpenDspConfig(string path) {
        if (!dspStageService.TryLoad(path, out DspStageState state, out string error)) {
            return false;
        }

        updatingControls = true;
        dspChannels.Clear();
        foreach (StageChannel channel in state.Channels) {
            dspChannels.Add(StageChannelRow.From(channel));
        }

        DspBassGainBox.Text = DspJson.NumberText(state.BassGain);
        DspLfeTrimBox.Text = DspJson.NumberText(state.LfeTrim);
        updatingControls = false;
        dspConfigPath = path;
        UpdateDspSummary();
        return true;
    }

    /// <summary>按当前布局的声道名与顺序重生成一份，用来从零开始。</summary>
    void RebuildDspRows() {
        List<string> names = document is null
            ? []
            : [.. document.Speakers.Select(speaker => speaker.Name)];
        dspChannels.Clear();
        foreach (StageChannel channel in DspStageService.CreateDefault(
                     names, LfeChannelIndex(), DspStageService.DefaultFrequencyHz).Channels) {
            dspChannels.Add(StageChannelRow.From(channel));
        }

        if (string.IsNullOrWhiteSpace(DspBassGainBox.Text)) {
            DspBassGainBox.Text = DspJson.NumberText(DspStageService.DefaultBassGainDb);
        }

        if (string.IsNullOrWhiteSpace(DspLfeTrimBox.Text)) {
            DspLfeTrimBox.Text = "0";
        }
    }

    /// <summary>把面板上的东西折回一份状态，准备落盘。</summary>
    DspStageState CollectDspState() {
        foreach (StageChannelRow row in dspChannels) {
            row.Revalidate();
        }

        // samplerate 与 chunksize 沿用文件里的原值：引擎的 CrossoverStage 会按
        // 链路实际速率重新发行配置，这里再改写一次只会让人误会该改哪一边。
        int sampleRate = 48000;
        int chunkSize = 256;
        if (dspConfigPath is not null &&
            dspStageService.TryLoad(dspConfigPath, out DspStageState existing, out _)) {
            sampleRate = existing.SampleRate;
            chunkSize = existing.ChunkSize;
        }

        return new DspStageState(
            sampleRate, chunkSize,
            ReadNumber(DspBassGainBox.Text, DspStageService.DefaultBassGainDb),
            ReadNumber(DspLfeTrimBox.Text, 0.0),
            [.. dspChannels.Select(row => row.Name)],
            [.. dspChannels.Select(row => row.ToChannel())]);
    }

    static double ReadNumber(string text, double fallback) =>
        double.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture,
                        out double value)
            ? value
            : fallback;

    void DspEnabledChanged(object sender, RoutedEventArgs e) {
        if (updatingControls) return;
        SaveDspPointer();
        UpdateDspSummary();
    }

    void DspNumbersChanged(object sender, RoutedEventArgs e) => UpdateDspSummary();

    /// <summary>开关与选中路径写回便携目录。</summary>
    void SaveDspPointer() =>
        dspStageService.SavePointer(new DspStagePointer(
            DspEnabledBox.IsChecked == true, dspConfigPath));

    void PickDspConfigClick(object sender, RoutedEventArgs e) {
        OpenFileDialog dialog = new() {
            Filter = "低音管理配置 (*.json)|*.json|所有文件 (*.*)|*.*",
            Title = "选择要使用的 dsp.json",
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) != true) {
            return;
        }

        if (TryOpenDspConfig(dialog.FileName)) {
            SaveDspPointer();
            StatusText.Text = "低音管理改用：" + dialog.FileName;
        } else {
            MessageBox.Show(this, dialog.FileName + "\n\n不是一份能读的 dsp 配置。",
                "低音管理", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    void SaveDspConfigClick(object sender, RoutedEventArgs e) =>
        WriteDspConfig(dspConfigPath ?? dspStageService.DefaultConfigPath);

    void SaveDspConfigAsClick(object sender, RoutedEventArgs e) {
        SaveFileDialog dialog = new() {
            Filter = "低音管理配置 (*.json)|*.json|所有文件 (*.*)|*.*",
            Title = "把低音管理配置存为",
            DefaultExt = ".json",
            FileName = IOPath.GetFileName(
                dspConfigPath ?? dspStageService.DefaultConfigPath)
        };
        if (dialog.ShowDialog(this) != true) {
            return;
        }

        WriteDspConfig(dialog.FileName);
    }

    void WriteDspConfig(string path) {
        try {
            dspStageService.Save(path, CollectDspState());
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "写入失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        dspConfigPath = path;
        SaveDspPointer();
        StatusText.Text = "低音管理配置已写入：" + path;
        UpdateDspSummary();
    }

    void RebuildDspConfigClick(object sender, RoutedEventArgs e) {
        if (document is null) {
            MessageBox.Show(this, "先在左边加载一个布局，才知道要按哪些声道生成。",
                "低音管理", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        RebuildDspRows();
        UpdateDspSummary();
        StatusText.Text = "已按当前布局重建，记得保存。";
    }

    /// <summary>
    /// 布局里名为 LFE 的声道索引。找不到就退回标准 7.1 顺序的第 4 个 —— 布局
    /// 可能用别的叫法，而分频必须知道往哪汇。
    /// </summary>
    int LfeChannelIndex() {
        if (document is null) return 3;
        for (int index = 0; index < document.Speakers.Count; index++) {
            if (string.Equals(document.Speakers[index].Name, "LFE",
                              StringComparison.OrdinalIgnoreCase)) {
                return index;
            }
        }

        return 3;
    }

    /// <summary>
    /// 起桥时交给引擎的路径。没启用就返回 null ——
    /// 引擎那边没有 SAL_CDSP_CONFIG 这个环境变量就是直通。
    ///
    /// 选中的文件还不存在时先按当前布局生成一份落进便携目录，
    /// 免得第一次启用就是空手启动。
    /// </summary>
    string? ResolveDspConfigPath() {
        if (DspEnabledBox.IsChecked != true || document is null) {
            return null;
        }

        string path = dspConfigPath ?? dspStageService.DefaultConfigPath;
        if (File.Exists(path)) {
            return path;
        }

        try {
            List<string> names = [.. document.Speakers.Select(speaker => speaker.Name)];
            string created = dspStageService.EnsureDefaultConfig(names, LfeChannelIndex());
            dspConfigPath = created;
            SaveDspPointer();
            return created;
        } catch (Exception error) {
            StatusText.Text = "低音管理配置生成失败：" + error.Message;
            return null;
        }
    }

    void UpdateDspSummary() {
        if (DspSummary is null) return;
        string path = dspConfigPath ?? dspStageService.DefaultConfigPath;
        bool enabled = DspEnabledBox.IsChecked == true;
        DspConfigPathText.Text = path;
        DspSaveButton.IsEnabled = document is not null;
        DspSaveAsButton.IsEnabled = document is not null;
        DspRebuildButton.IsEnabled = document is not null;
        DspChannelList.IsEnabled = enabled;

        if (!enabled) {
            DspSummary.Text = "未启用（引擎直通）";
            return;
        }

        if (document is null) {
            DspSummary.Text = "需要先加载布局";
            return;
        }

        string engineDirectory =
            IOPath.GetDirectoryName(runtimePaths.EngineExecutable) ??
            runtimePaths.ApplicationRoot;
        if (!File.Exists(IOPath.Combine(engineDirectory, "cdsp_cpp.dll"))) {
            DspSummary.Text = $"缺少 cdsp_cpp.dll（引擎目录 {engineDirectory}）";
            return;
        }

        StageChannelRow? sub = dspChannels.FirstOrDefault(row => row.IsLfe);
        double bassGain = ReadNumber(DspBassGainBox.Text,
                                     DspStageService.DefaultBassGainDb);
        string note = File.Exists(path) ? string.Empty : " · 尚未保存，起桥时生成";
        DspSummary.Text =
            $"{dspChannels.Count} 声道 · 低音汇入 {bassGain:0.#} dB · " +
            $"汇向 {(sub is null ? "未指定 LFE" : sub.Name)}{note}";
    }

    void BridgeModeClick(object sender, RoutedEventArgs e) {
        bridgeModeSelectionInitialized = true;
        UpdateBridgeStatus();
    }

    void SelectBridgeMode(BridgeMode mode) {
        MatModeButton.IsChecked = mode == BridgeMode.Atmos;
        NativeMatModeButton.IsChecked = mode == BridgeMode.NativeMat;
        DtsXModeButton.IsChecked = mode == BridgeMode.DtsX;
        PcmModeButton.IsChecked = mode == BridgeMode.Pcm;
        NspModeButton.IsChecked = mode == BridgeMode.Nsp;
        Eac3ModeButton.IsChecked = mode == BridgeMode.Eac3;
    }

    bool TryGetLiveBridgeProcess(BridgeMode mode, out Process? process) {
        process = null;
        try {
            string pidPath = BridgePidPath(mode);
            if (!File.Exists(pidPath)) return false;
            string value = File.ReadAllText(pidPath).Trim();
            if (!int.TryParse(value, out int processId)) return false;
            process = Process.GetProcessById(processId);
            string processName = process.ProcessName;
            if (!process.HasExited &&
                (processName.Equals("SpatialAudioLab.CLI", StringComparison.OrdinalIgnoreCase) ||
                 processName.Equals("dolby-probe", StringComparison.OrdinalIgnoreCase))) return true;
            process.Dispose();
            process = null;
            return false;
        } catch (Exception error) when (error is ArgumentException or IOException or
                                            InvalidOperationException or UnauthorizedAccessException) {
            process?.Dispose();
            process = null;
            return false;
        }
    }

    List<BridgeMode> RunningBridgeModes() {
        List<BridgeMode> result = [];
        foreach (BridgeMode mode in Enum.GetValues<BridgeMode>()) {
            if (TryGetLiveBridgeProcess(mode, out Process? process)) result.Add(mode);
            process?.Dispose();
        }
        return result;
    }

    void UpdateBridgeStatus() {
        List<BridgeMode> runningModes = RunningBridgeModes();
        BridgeMode statusMode = runningModes.Count == 1 ? runningModes[0] : SelectedBridgeMode;
        if (!bridgeModeSelectionInitialized && runningModes.Count == 1) {
            SelectBridgeMode(statusMode);
            bridgeModeSelectionInitialized = true;
        }

        if (!bridgeCommandRunning) {
            if (runningModes.Count > 1) {
                BridgeStatusText.Text = "多个桥接同时运行";
            } else if (runningModes.Count == 1 &&
                       TryGetLiveBridgeProcess(statusMode, out Process? process)) {
                using (process) {
                    BridgeStatusText.Text = $"{BridgeName(statusMode)} 运行中 · PID {process!.Id}";
                }
            } else {
                BridgeStatusText.Text = $"{BridgeName(statusMode)} 已停止";
            }
        }

        bool anyRunning = runningModes.Count != 0;
        bool selectedModeRunning = runningModes.Count == 1 &&
            runningModes[0] == SelectedBridgeMode;
        BridgeStatusDot.Fill = new SolidColorBrush(runningModes.Count > 1
            ? Color.FromRgb(178, 80, 65)
            : anyRunning
                ? Color.FromRgb(55, 145, 99)
                : Color.FromRgb(139, 148, 154));
        if (!bridgeCommandRunning) {
            BridgeStartButton.IsEnabled = !selectedModeRunning;
            BridgeStopButton.IsEnabled = anyRunning;
            BridgeStartText.Text = !anyRunning
                ? "启动"
                : selectedModeRunning ? "运行中" : "切换";
        }
        MatModeButton.IsEnabled = !bridgeCommandRunning;
        NativeMatModeButton.IsEnabled = !bridgeCommandRunning;
        DtsXModeButton.IsEnabled = !bridgeCommandRunning;
        PcmModeButton.IsEnabled = !bridgeCommandRunning;
        BridgeGainSlider.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BridgePrebufferSlider.IsEnabled = !anyRunning && !bridgeCommandRunning;
        SafeLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BalancedLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        LowLatencyButton.IsEnabled = !anyRunning && !bridgeCommandRunning;
        BridgeOpenLogButton.IsEnabled = File.Exists(BridgeLogPath(statusMode));
    }

    /// <summary>
    /// Closing the window is what ends a run: the engine's lifetime is owned by
    /// the studio, and the NSP capture keeps holding the shared-memory ring until
    /// it is stopped, so both have to be torn down on the way out.
    /// </summary>
    protected override async void OnClosed(EventArgs e) {
        try {
            foreach (BridgeMode mode in RunningBridgeModes()) {
                await bridgeProcessService.StopAsync(BridgePidPath(mode));
            }
            await nspCaptureService.StopAsync();
            await eac3ProducerService.StopAsync();
        } catch {
            // The window is going away regardless; a failed cleanup must not
            // turn into a crash on shutdown.
        }
        base.OnClosed(e);
    }

    /// <summary>
    /// NSP captures whatever spatial beds Windows builds for the default endpoint,
    /// but it can only place the 12 channels of a 7.1.4 bed. Other formats are
    /// worth trying rather than refusing — Windows Sonic in particular needs
    /// neither Dolby Access nor an HDMI emulator, which makes it the cheapest way
    /// to get 7.1.4 out of Windows — so this reports and lets the operator decide.
    /// </summary>
    async Task<bool> ConfirmNspSpatialFormatAsync() {
        string? mismatch = await SpatialProviderActivation
            .DescribeNspSpatialFormatAsync(runtimePaths).ConfigureAwait(true);
        if (mismatch is null) return true;
        MessageBoxResult answer = MessageBox.Show(
            this,
            "默认端点不是 Dolby Atmos for home theater。\n\n" + mismatch + "\n\n" +
            "Windows Sonic 可直接用：不需要 Dolby Access，也不需要 HDMI 模拟器。\n" +
            "注意：它的静态对象上限是 17，超出 7.1.4 那 12 个时采集会中断。\n\n" +
            "仍要启动吗？",
            "空间音效格式确认",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        return answer == MessageBoxResult.Yes;
    }

    async void StartBridgeClick(object sender, RoutedEventArgs e) {
        if (currentPath is null) return;
        bool switching = RunningBridgeModes().Count != 0;
        if (!SaveProfile(currentPath)) return;

        BridgeMode mode = SelectedBridgeMode;
        string bridgeName = BridgeName(mode);
        bridgeCommandRunning = true;
        BridgeStartButton.IsEnabled = false;
        BridgeStopButton.IsEnabled = false;
        BridgeStatusText.Text = switching
            ? $"正在切换到 {bridgeName}..."
            : $"正在启动 {bridgeName}...";
        try {
            foreach (BridgeMode runningMode in RunningBridgeModes()) {
                await bridgeProcessService.StopAsync(BridgePidPath(runningMode));
            }
            // Leaving NSP mode has to tear the capture down too, or it keeps
            // holding the ring. Restarting NSP mode just starts it again below.
            await nspCaptureService.StopAsync();
            // Same for the E-AC-3 producer: it owns the SAL_EAC3_PCM714 mapping.
            await eac3ProducerService.StopAsync();
            if (mode == BridgeMode.Nsp &&
                !await ConfirmNspSpatialFormatAsync().ConfigureAwait(true)) {
                StatusText.Text = "已取消启动 " + bridgeName;
                return;
            }
            await spatialProviderService.ActivateAsync(mode, runtimePaths);
            if (mode == BridgeMode.Nsp) {
                // The capture side publishes the ring the engine then reads, so it
                // has to be up first; the engine waits for it either way.
                await nspCaptureService.StartAsync(
                    runtimePaths,
                    NspCaptureService.ResolveNspRoot(runtimePaths) ?? string.Empty);
            }
            if (mode == BridgeMode.Eac3) {
                // Eac3Pipe714 是环的生产者，必须先把 Local\SAL_EAC3_PCM714 建出来：
                // 引擎那侧的 NspRingReader 只在启动时打开一次映射，晚到就打不开了。
                await eac3ProducerService.StartAsync(runtimePaths);
            }
            BridgeCommand command = BridgeCommandFactory.Create(
                runtimePaths,
                mode,
                currentPath,
                BridgeGainSlider.Value,
                (int)Math.Round(BridgePrebufferSlider.Value),
                SelectedBridgeLatencyMode,
                ResolveDspConfigPath());
            await bridgeProcessService.StartAsync(command);
            StatusText.Text = switching
                ? $"已切换到 {bridgeName}"
                : $"{bridgeName} 已启动";
        } catch (Exception error) {
            // 桥起不来时生产者已经在跑了，留着它会一直占着环和管道。
            await eac3ProducerService.StopAsync();
            MessageBox.Show(this, error.Message, $"无法启动 {bridgeName}",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = $"启动失败：{bridgeName}";
        } finally {
            bridgeCommandRunning = false;
            UpdateBridgeStatus();
        }
    }

    async void StopBridgeClick(object sender, RoutedEventArgs e) {
        bridgeCommandRunning = true;
        BridgeStartButton.IsEnabled = false;
        BridgeStopButton.IsEnabled = false;
        BridgeStatusText.Text = "正在停止桥接...";
        try {
            foreach (BridgeMode mode in RunningBridgeModes()) {
                await bridgeProcessService.StopAsync(BridgePidPath(mode));
            }
            await nspCaptureService.StopAsync();
            await eac3ProducerService.StopAsync();
            StatusText.Text = "桥接已停止";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "无法停止桥接",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = "停止桥接失败";
        } finally {
            bridgeCommandRunning = false;
            UpdateBridgeStatus();
        }
    }

    void OpenBridgeLogClick(object sender, RoutedEventArgs e) {
        List<BridgeMode> runningModes = RunningBridgeModes();
        BridgeMode mode = runningModes.Count == 1 ? runningModes[0] : SelectedBridgeMode;
        string logPath = BridgeLogPath(mode);
        if (!File.Exists(logPath)) return;
        Process.Start(new ProcessStartInfo(logPath) { UseShellExecute = true });
    }

    void OpenSpatialSettingsClick(object sender, RoutedEventArgs e) {
        try {
            Process.Start(new ProcessStartInfo("ms-settings:sound") {
                UseShellExecute = true
            });
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "无法打开声音设置",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void RebuildBridgeMeters() {
        BridgeMeterPanel.Children.Clear();
        channelMeters.Clear();
        if (document is null) return;
        foreach (SpeakerDefinition speaker in document.Speakers) {
            ChannelMeterControl meter = new(speaker.Name);
            channelMeters.Add(speaker.Name, meter);
            BridgeMeterPanel.Children.Add(meter);
        }
    }

    void UpdateBridgeMeters() {
        if (!meterReader.TryRead(out BridgeMeterSnapshot? snapshot) ||
            snapshot is null || !snapshot.IsFresh) {
            ResetBridgeMeters();
            return;
        }

        HashSet<string> updated = new(StringComparer.OrdinalIgnoreCase);
        foreach (BridgeMeterChannel channel in snapshot.Channels) {
            if (!channelMeters.TryGetValue(channel.Name, out ChannelMeterControl? meter)) continue;
            meter.SetLevel(channel.Rms, channel.Peak);
            updated.Add(channel.Name);
        }
        foreach ((string name, ChannelMeterControl meter) in channelMeters) {
            if (!updated.Contains(name)) meter.Reset();
        }
    }

    void ResetBridgeMeters() {
        foreach (ChannelMeterControl meter in channelMeters.Values) meter.Reset();
    }

    void PopulateRouteLegend() {
        RouteLegend.Children.Clear();
        if (document is null) return;
        for (int index = 0; index < document.Outputs.Count; ++index) {
            RouteLegend.Children.Add(new Rectangle {
                Width = 12,
                Height = 12,
                Fill = new SolidColorBrush(RouteColors[index % RouteColors.Length]),
                Margin = new Thickness(0, 0, 5, 0)
            });
            RouteLegend.Children.Add(new TextBlock {
                Text = document.Outputs[index].Name,
                Margin = new Thickness(0, 0, 12, 0)
            });
        }
    }

    void OpenClick(object sender, RoutedEventArgs e) {
        OpenFileDialog dialog = new() {
            Filter = "扬声器配置 (*.ini)|*.ini|所有文件 (*.*)|*.*",
            InitialDirectory = runtimePaths.ProfilesRoot
        };
        if (dialog.ShowDialog(this) == true) LoadProfile(dialog.FileName);
    }

    void SaveClick(object sender, RoutedEventArgs e) {
        if (currentPath is null) {
            SaveAsClick(sender, e);
            return;
        }
        SaveProfile(currentPath);
    }

    void SaveAsClick(object sender, RoutedEventArgs e) {
        SaveFileDialog dialog = new() {
            Filter = "扬声器配置 (*.ini)|*.ini",
            InitialDirectory = runtimePaths.ProfilesRoot,
            FileName = currentPath is null ? "speaker-layout.ini" : IOPath.GetFileName(currentPath)
        };
        if (dialog.ShowDialog(this) == true) SaveProfile(dialog.FileName);
    }

    bool SaveProfile(string path) {
        if (document is null) return false;
        try {
            ProfileDocument profile = document.ToProfile();
            profileRepository.Save(profile);
            profileRepository.SetActive(profile.Id);
            string canonicalPath = IOPath.Combine(
                runtimePaths.ProfilesRoot,
                $"{profile.Id:D}.ini");
            string requestedPath = IOPath.GetFullPath(path);
            if (!requestedPath.Equals(canonicalPath, StringComparison.OrdinalIgnoreCase)) {
                ProfileIniSerializer.Save(requestedPath, profile);
            }
            currentPath = canonicalPath;
            PathText.Text = currentPath;
            StatusText.Text = requestedPath.Equals(canonicalPath, StringComparison.OrdinalIgnoreCase)
                ? "配置文件已保存"
                : $"配置文件已保存 y exportado a {requestedPath}";
            return true;
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "无法保存配置文件",
                MessageBoxButton.OK, MessageBoxImage.Error);
            return false;
        }
    }

    void SpeakerSelectionChanged(object sender, SelectionChangedEventArgs e) {
        selectedSpeaker = SpeakerList.SelectedItem as SpeakerDefinition;
        UpdateSpeakerControls();
        RedrawMaps();
    }

    void UpdateSpeakerControls() {
        updatingControls = true;
        bool enabled = selectedSpeaker is not null && document is not null;
        AzimuthSlider.IsEnabled = enabled;
        ElevationSlider.IsEnabled = enabled;
        TrimSlider.IsEnabled = enabled;
        PhysicalDestinationCombo.IsEnabled = enabled;
        if (enabled) {
            SelectedNameText.Text = selectedSpeaker!.Name;
            AzimuthSlider.Value = selectedSpeaker.Azimuth;
            ElevationSlider.Value = selectedSpeaker.Elevation;
            TrimSlider.Value = selectedSpeaker.TrimDb;
            var slot = document!.SlotFor(selectedSpeaker.Name);
            PhysicalDestinationCombo.SelectedItem = slot.HasValue
                ? physicalDestinationChoices.FirstOrDefault(choice =>
                    ReferenceEquals(choice.Output, slot.Value.Output) &&
                    choice.ChannelIndex == slot.Value.ChannelIndex)
                : null;
            DestinationEndpointText.Text = slot?.Output.Endpoint ?? "未设置输出";
        } else {
            SelectedNameText.Text = "";
            PhysicalDestinationCombo.SelectedItem = null;
            DestinationEndpointText.Text = "";
        }
        UpdateValueLabels();
        updatingControls = false;
    }

    void SpeakerControlChanged(object sender, RoutedPropertyChangedEventArgs<double> e) {
        if (updatingControls || selectedSpeaker is null) return;
        selectedSpeaker.Azimuth = Math.Round(AzimuthSlider.Value, 1);
        selectedSpeaker.Elevation = Math.Round(ElevationSlider.Value, 1);
        selectedSpeaker.TrimDb = Math.Round(TrimSlider.Value, 1);
        UpdateValueLabels();
        RedrawMaps();
    }

    void UpdateValueLabels() {
        AzimuthValue.Text = $"{AzimuthSlider.Value:0.0}°";
        ElevationValue.Text = $"{ElevationSlider.Value:0.0}°";
        TrimValue.Text = $"{TrimSlider.Value:0.0} dB";
    }

    void PhysicalDestinationSelectionChanged(object sender, SelectionChangedEventArgs e) {
        if (updatingControls || document is null || selectedSpeaker is null ||
            PhysicalDestinationCombo.SelectedItem is not PhysicalDestinationChoice target) return;
        var source = document.SlotFor(selectedSpeaker.Name);
        if (!source.HasValue ||
            (ReferenceEquals(source.Value.Output, target.Output) &&
             source.Value.ChannelIndex == target.ChannelIndex)) return;

        string previousDestination =
            $"{source.Value.Output.Name} / " +
            $"{source.Value.Output.PhysicalChannelName(source.Value.ChannelIndex)}";
        string displaced = document.AssignSpeakerToSlot(
            selectedSpeaker.Name, target.Output, target.ChannelIndex);
        OutputGrid.Items.Refresh();
        UpdateSpeakerControls();
        RedrawMaps();
        StatusText.Text =
            $"{selectedSpeaker.Name} → {target.Output.Name} / " +
            $"{target.Output.PhysicalChannelName(target.ChannelIndex)}；" +
            $"{displaced} -> {previousDestination}（未保存）";
    }

    void ResetPositionsClick(object sender, RoutedEventArgs e) {
        if (document is null) return;
        int reset = 0;
        foreach (SpeakerDefinition speaker in document.Speakers) {
            if (!DefaultPositions.TryGetValue(speaker.Name, out var position)) continue;
            speaker.Azimuth = position.Azimuth;
            speaker.Elevation = position.Elevation;
            ++reset;
        }
        UpdateSpeakerControls();
        RedrawMaps();
        StatusText.Text = $"已重置 {reset} 个扬声器位置（未保存）";
    }

    async void TestSpeakerClick(object sender, RoutedEventArgs e) {
        if (document is null || selectedSpeaker is null || currentPath is null) return;
        if (!SaveProfile(currentPath)) return;
        string executable = runtimePaths.EngineExecutable;
        if (!File.Exists(executable)) {
            MessageBox.Show(this, $"找不到音频引擎：\n{executable}",
                "缺少 SpatialAudioLab.CLI.exe", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        try {
            StatusText.Text = $"正在测试 {selectedSpeaker.Name}";
            ProcessStartInfo start = new(executable) {
                UseShellExecute = false,
                CreateNoWindow = true
            };
            start.ArgumentList.Add("test-speaker");
            start.ArgumentList.Add("3");
            start.ArgumentList.Add(currentPath);
            start.ArgumentList.Add(selectedSpeaker.Name);
            start.ArgumentList.Add(TestGainSlider.Value.ToString(
                "0.###", CultureInfo.InvariantCulture));
            using Process process = Process.Start(start) ??
                throw new InvalidOperationException("无法启动 la prueba.");
            await process.WaitForExitAsync();
            StatusText.Text = process.ExitCode == 0
                ? $"测试完成：{selectedSpeaker.Name}"
                : $"测试结束，退出代码：{process.ExitCode}";
        } catch (Exception error) {
            MessageBox.Show(this, error.Message, "测试失败",
                MessageBoxButton.OK, MessageBoxImage.Error);
            StatusText.Text = "测试失败";
        }
    }

    void CanvasSizeChanged(object sender, SizeChangedEventArgs e) => RedrawMaps();

    void RedrawMaps() {
        if (!IsLoaded || document is null || TopCanvas.ActualWidth < 100 ||
            ElevationCanvas.ActualWidth < 100) return;
        DrawTopView();
        DrawElevationView();
    }

    void DrawTopView() {
        TopCanvas.Children.Clear();
        double width = TopCanvas.ActualWidth;
        double height = TopCanvas.ActualHeight;
        double centerX = width / 2;
        double centerY = height / 2;
        double radius = Math.Max(40, Math.Min(width, height) * 0.39);
        TopCanvas.Children.Add(new Ellipse {
            Width = radius * 2,
            Height = radius * 2,
            Stroke = new SolidColorBrush(Color.FromRgb(201, 207, 211)),
            StrokeThickness = 1
        }.At(centerX - radius, centerY - radius));
        AddLine(TopCanvas, centerX, centerY - radius, centerX, centerY + radius);
        AddLine(TopCanvas, centerX - radius, centerY, centerX + radius, centerY);
        AddListener(TopCanvas, centerX, centerY);

        foreach (SpeakerDefinition speaker in document!.Speakers) {
            double markerRadius = speaker.Name.Equals("LFE", StringComparison.OrdinalIgnoreCase)
                ? radius * 0.18
                : speaker.Elevation >= 25 ? radius * 0.65 : radius * 0.92;
            double radians = speaker.Azimuth * Math.PI / 180;
            double x = centerX + Math.Sin(radians) * markerRadius;
            double y = centerY - Math.Cos(radians) * markerRadius;
            AddSpeakerMarker(TopCanvas, speaker, x, y);
        }
    }

    void DrawElevationView() {
        ElevationCanvas.Children.Clear();
        double width = ElevationCanvas.ActualWidth;
        double height = ElevationCanvas.ActualHeight;
        const double margin = 24;
        List<Point> occupied = new();
        double zeroY = ElevationToY(0, height, margin);
        AddLine(ElevationCanvas, margin, zeroY, width - margin, zeroY);
        AddLine(ElevationCanvas, width / 2, margin, width / 2, height - margin);
        foreach (SpeakerDefinition speaker in document!.Speakers) {
            double x = margin + (speaker.Azimuth + 180) / 360 * (width - margin * 2);
            double y = ElevationToY(speaker.Elevation, height, margin);
            for (int attempt = 0; attempt < 4 &&
                 occupied.Any(point => Math.Abs(point.X - x) < 36 &&
                                       Math.Abs(point.Y - y) < 24); ++attempt) {
                y = Math.Min(height - 18, y + 26);
            }
            occupied.Add(new Point(x, y));
            AddSpeakerMarker(ElevationCanvas, speaker, x, y);
        }
    }

    static double ElevationToY(double elevation, double height, double margin) {
        double normalized = (Math.Clamp(elevation, -30, 90) + 30) / 120;
        return height - margin - normalized * (height - margin * 2);
    }

    void AddSpeakerMarker(Canvas canvas, SpeakerDefinition speaker, double x, double y) {
        OutputRouteDefinition? route = document!.OutputFor(speaker.Name);
        int routeIndex = route is null ? 0 : document.Outputs.IndexOf(route);
        Color color = RouteColors[Math.Abs(routeIndex) % RouteColors.Length];
        bool selected = ReferenceEquals(speaker, selectedSpeaker);
        Border marker = new() {
            Width = 42,
            Height = 30,
            CornerRadius = new CornerRadius(4),
            Background = new SolidColorBrush(color),
            BorderBrush = selected ? Brushes.Black : Brushes.White,
            BorderThickness = new Thickness(selected ? 2 : 1),
            Tag = speaker,
            Cursor = Cursors.Hand,
            Child = new TextBlock {
                Text = speaker.Name,
                Foreground = Brushes.White,
                FontSize = 11,
                FontWeight = FontWeights.SemiBold,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center
            }
        };
        marker.MouseLeftButtonDown += MarkerMouseDown;
        Canvas.SetLeft(marker, x - marker.Width / 2);
        Canvas.SetTop(marker, y - marker.Height / 2);
        canvas.Children.Add(marker);
    }

    void MarkerMouseDown(object sender, MouseButtonEventArgs e) {
        if (sender is not Border marker || marker.Tag is not SpeakerDefinition speaker) return;
        draggedSpeaker = speaker;
        dragCanvas = FindParentCanvas(marker);
        if (dragCanvas is null) return;
        dragStartPoint = e.GetPosition(dragCanvas);
        Point markerCenter = new(
            Canvas.GetLeft(marker) + marker.Width / 2,
            Canvas.GetTop(marker) + marker.Height / 2);
        dragPointerOffset = dragStartPoint - markerCenter;
        dragActivated = false;
        dragCanvas?.CaptureMouse();
        SpeakerList.SelectedItem = speaker;
        e.Handled = true;
    }

    void CanvasMouseMove(object sender, MouseEventArgs e) {
        if (e.LeftButton != MouseButtonState.Pressed || draggedSpeaker is null ||
            dragCanvas is null || !ReferenceEquals(sender, dragCanvas)) return;
        Point pointer = e.GetPosition(dragCanvas);
        if (!dragActivated) {
            if (Math.Abs(pointer.X - dragStartPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
                Math.Abs(pointer.Y - dragStartPoint.Y) < SystemParameters.MinimumVerticalDragDistance) {
                return;
            }
            dragActivated = true;
        }
        Point point = pointer - dragPointerOffset;
        if (ReferenceEquals(dragCanvas, TopCanvas)) {
            double dx = point.X - TopCanvas.ActualWidth / 2;
            double dy = point.Y - TopCanvas.ActualHeight / 2;
            draggedSpeaker.Azimuth = Math.Round(Math.Atan2(dx, -dy) * 180 / Math.PI, 1);
        } else {
            const double margin = 24;
            double usableWidth = Math.Max(1, ElevationCanvas.ActualWidth - margin * 2);
            double usableHeight = Math.Max(1, ElevationCanvas.ActualHeight - margin * 2);
            draggedSpeaker.Azimuth = Math.Round(
                Math.Clamp((point.X - margin) / usableWidth, 0, 1) * 360 - 180, 1);
            double normalized = 1 - Math.Clamp((point.Y - margin) / usableHeight, 0, 1);
            draggedSpeaker.Elevation = Math.Round(normalized * 120 - 30, 1);
        }
        UpdateSpeakerControls();
        RedrawMaps();
        e.Handled = true;
    }

    void CanvasMouseUp(object sender, MouseButtonEventArgs e) {
        if (dragCanvas is null || !ReferenceEquals(sender, dragCanvas)) return;
        dragCanvas.ReleaseMouseCapture();
        draggedSpeaker = null;
        dragCanvas = null;
        dragActivated = false;
        e.Handled = true;
    }

    static Canvas? FindParentCanvas(DependencyObject child) {
        DependencyObject? current = child;
        while (current is not null) {
            if (current is Canvas canvas) return canvas;
            current = VisualTreeHelper.GetParent(current);
        }
        return null;
    }

    static void AddLine(Canvas canvas, double x1, double y1, double x2, double y2) {
        canvas.Children.Add(new Line {
            X1 = x1, Y1 = y1, X2 = x2, Y2 = y2,
            Stroke = new SolidColorBrush(Color.FromRgb(218, 222, 225)),
            StrokeThickness = 1
        });
    }

    static void AddListener(Canvas canvas, double x, double y) {
        Ellipse listener = new() {
            Width = 18, Height = 18,
            Fill = new SolidColorBrush(Color.FromRgb(37, 43, 48)),
            Stroke = Brushes.White, StrokeThickness = 2
        };
        Canvas.SetLeft(listener, x - 9);
        Canvas.SetTop(listener, y - 9);
        canvas.Children.Add(listener);
    }
}

internal static class CanvasElementExtensions {
    public static T At<T>(this T element, double left, double top) where T : UIElement {
        Canvas.SetLeft(element, left);
        Canvas.SetTop(element, top);
        return element;
    }
}
