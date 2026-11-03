using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using SpatialAudioLab.Core.Runtime;

namespace SpeakerLayoutEditor.Services;

/// <summary>
/// 低音管理（分频）配置的读写与生成。
///
/// 生成规则照搬 SalAsio 的 src/cli/dsp_editor.cpp（也就是 ui/editor.js 那一套）：
/// 每个非 LFE 声道一条高通路径（带上它自己的 delay/gain/极性/PEQ）加一条低通
/// 路径，LFE 单独一条路径，矩阵把所有低通按 bassGain 汇进 LFE。形状一致，
/// 所以 WebView2 编辑器仍能打开这里写出来的文件，反过来也一样。
/// </summary>
internal sealed class DspStageService
{
    public const double DefaultFrequencyHz = 80.0;
    public const double MinimumFrequencyHz = 40.0;
    public const double MaximumFrequencyHz = 200.0;
    public const double DefaultBassGainDb = -12.0;

    private const int DefaultSampleRate = 48000;
    private const int DefaultChunkSize = 256;
    private const int FilterOrder = 4;

    private readonly RuntimePaths paths;

    public DspStageService(RuntimePaths paths)
    {
        this.paths = paths;
    }

    /// <summary>Studio 自己的开关与选中路径，落在便携 Data 目录里。</summary>
    public string PointerPath => Path.Combine(paths.DataRoot, "dsp-stage.json");

    /// <summary>没有特别指定时用哪份 json，名字跟 SalAsio 的默认保持一致。</summary>
    public string DefaultConfigPath => Path.Combine(paths.DataRoot, "dsp.json");

    public DspStagePointer LoadPointer() => ReadPointer(PointerPath);

    public void SavePointer(DspStagePointer pointer)
    {
        Directory.CreateDirectory(paths.DataRoot);
        File.WriteAllText(
            PointerPath,
            JsonSerializer.Serialize(
                pointer,
                new JsonSerializerOptions { WriteIndented = true }),
            new UTF8Encoding(false));
    }

    /// <summary>
    /// 读一份 dsp.json。缺 _layout 的老文件也能读（按声道数兜底），
    /// 处理方式与 SalAsio 相同。
    /// </summary>
    public bool TryLoad(string path, out DspStageState state, out string error)
    {
        state = CreateDefault([], 0, DefaultFrequencyHz);
        error = string.Empty;
        if (!File.Exists(path))
        {
            error = "文件不存在：" + path;
            return false;
        }

        try
        {
            using JsonDocument json = JsonDocument.Parse(File.ReadAllText(path));
            JsonElement root = json.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                error = "顶层不是 JSON 对象。";
                return false;
            }

            int sampleRate = ReadInt(root, "samplerate", DefaultSampleRate);
            int chunkSize = ReadInt(root, "chunksize", DefaultChunkSize);
            double bassGain = DefaultBassGainDb;
            double lfeTrim = 0.0;
            List<string> layout = ReadStringArray(root, "_layout");

            bool hasUi = root.TryGetProperty("_ui", out JsonElement ui) &&
                         ui.ValueKind == JsonValueKind.Object;
            if (hasUi)
            {
                bassGain = ReadDouble(ui, "bassGain", DefaultBassGainDb);
                lfeTrim = ReadDouble(ui, "lfeTrim", 0.0);
                if (layout.Count == 0)
                {
                    layout = ReadStringArray(ui, "layout");
                }
            }

            if (layout.Count == 0)
            {
                int declared = ReadInt(root, "output_channels", 0);
                if (declared <= 0)
                {
                    error = "既没有 _layout，也读不出 output_channels。";
                    return false;
                }

                layout = [];
                for (int index = 0; index < declared; index++)
                {
                    layout.Add($"ch{index + 1}");
                }
            }

            Dictionary<string, JsonElement> byName = [];
            if (hasUi && ui.TryGetProperty("channels", out JsonElement uiChannels) &&
                uiChannels.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement item in uiChannels.EnumerateArray())
                {
                    string name = ReadString(item, "name", string.Empty);
                    if (name.Length > 0)
                    {
                        byName[name] = item;
                    }
                }
            }

            List<StageChannel> channels = [];
            foreach (string name in layout)
            {
                bool configured = byName.TryGetValue(name, out JsonElement node);
                channels.Add(new StageChannel(
                    name,
                    configured && ReadBool(node, "lfe", false),
                    configured ? ReadDouble(node, "hp", DefaultFrequencyHz)
                               : DefaultFrequencyHz,
                    configured ? ReadDouble(node, "delay", 0.0) : 0.0,
                    configured ? ReadDouble(node, "gain", 0.0) : 0.0,
                    configured && ReadBool(node, "invert", false),
                    configured ? ReadString(node, "peq", string.Empty) : string.Empty));
            }

            state = new DspStageState(
                sampleRate, chunkSize, bassGain, lfeTrim, layout, channels);
            return true;
        }
        catch (Exception parseError)
        {
            error = parseError.Message;
            return false;
        }
    }

    public void Save(string path, DspStageState state)
    {
        string? directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(path, BuildJson(state), new UTF8Encoding(false));
    }

    /// <summary>
    /// 按当前布局造一份默认配置：所有声道落在同一个分频点上，
    /// 名字叫 LFE 的那个（调用方给的索引）收集低音。
    /// </summary>
    public static DspStageState CreateDefault(
        IReadOnlyList<string> layout, int lfeIndex, double crossoverHz,
        double bassGainDb = DefaultBassGainDb, int sampleRate = DefaultSampleRate,
        int chunkSize = DefaultChunkSize)
    {
        List<string> names = [.. layout];
        List<StageChannel> channels = [];
        for (int index = 0; index < names.Count; index++)
        {
            // LFE 也填同一个分频点。它不参与高通 —— BuildConfig 里 LFE 那条
            // 路径压根没有 lr_highpass —— 之所以照样写 80，是为了让生成出来的
            // 文件与 SalAsio 的逐字段一致，两边并排看时不会平白多一处差异。
            channels.Add(new StageChannel(
                names[index], index == lfeIndex, crossoverHz,
                0.0, 0.0, false, string.Empty));
        }

        return new DspStageState(sampleRate, chunkSize, bassGainDb, 0.0, names, channels);
    }

    /// <summary>
    /// 引擎还没起来，或者选中的文件还不存在时，按当前布局生成一份落到
    /// <see cref="DefaultConfigPath"/>，好让面板立刻有东西可编辑。
    /// </summary>
    public string EnsureDefaultConfig(IReadOnlyList<string> layout, int lfeIndex)
    {
        if (File.Exists(DefaultConfigPath))
        {
            return DefaultConfigPath;
        }

        Save(DefaultConfigPath, CreateDefault(layout, lfeIndex, DefaultFrequencyHz));
        return DefaultConfigPath;
    }

    /// <summary>生成整个 cdsp 文档，顺序与 dsp_editor.cpp 的 BuildConfig 一致。</summary>
    public static string BuildJson(DspStageState state)
    {
        IReadOnlyList<StageChannel> channels = state.Channels;
        int count = channels.Count;
        int lfeIndex = count;
        for (int index = 0; index < count; index++)
        {
            if (channels[index].Lfe)
            {
                lfeIndex = index;
                break;
            }
        }

        StringBuilder json = new();
        json.AppendLine("{");
        json.AppendLine($"  \"samplerate\": {state.SampleRate},");
        json.AppendLine($"  \"chunksize\": {state.ChunkSize},");
        json.AppendLine($"  \"input_channels\": {count},");
        json.AppendLine($"  \"output_channels\": {count},");
        json.AppendLine($"  \"_layout\": [{DspJson.QuoteList(state.Layout)}],");
        json.AppendLine("  \"paths\": [");

        List<string> pathEntries = [];
        int[] highPass = new int[count];
        int[] lowPass = new int[count];
        int nextPath = 0;
        for (int index = 0; index < count; index++)
        {
            if (index == lfeIndex)
            {
                continue;
            }

            StageChannel channel = channels[index];
            double crossover = channel.Crossover > 0.0 ? channel.Crossover : DefaultFrequencyHz;
            StringBuilder filters = new();
            filters.Append(
                $"{{\"type\": \"lr_highpass\", \"freq\": {DspJson.NumberText(crossover)}, " +
                $"\"order\": {FilterOrder}}}");
            foreach (PeqBand band in DspJson.ParsePeq(channel.Peq))
            {
                filters.Append(
                    $", {{\"type\": \"peq\", \"freq\": {DspJson.NumberText(band.Freq)}, " +
                    $"\"gain_db\": {DspJson.NumberText(band.Gain)}, " +
                    $"\"q\": {DspJson.NumberText(band.Q)}}}");
            }

            highPass[index] = nextPath++;
            pathEntries.Add(PathBlock(index, filters.ToString(), channel,
                                      channel.Gain, channel.Invert, channel.Delay));

            lowPass[index] = nextPath++;
            StringBuilder lowPassFilter = new();
            lowPassFilter.Append(
                $"{{\"type\": \"lr_lowpass\", \"freq\": {DspJson.NumberText(crossover)}, " +
                $"\"order\": {FilterOrder}}}");
            pathEntries.Add(PathBlock(index, lowPassFilter.ToString(), channel,
                                      0.0, false, channel.Delay));
        }

        int lfePath = 0;
        if (lfeIndex < count)
        {
            StageChannel channel = channels[lfeIndex];
            StringBuilder filters = new();
            foreach (PeqBand band in DspJson.ParsePeq(channel.Peq))
            {
                if (filters.Length > 0)
                {
                    filters.Append(", ");
                }

                filters.Append(
                    $"{{\"type\": \"peq\", \"freq\": {DspJson.NumberText(band.Freq)}, " +
                    $"\"gain_db\": {DspJson.NumberText(band.Gain)}, " +
                    $"\"q\": {DspJson.NumberText(band.Q)}}}");
            }

            lfePath = nextPath++;
            pathEntries.Add(PathBlock(lfeIndex, filters.ToString(), channel,
                                      state.LfeTrim + channel.Gain, false, channel.Delay));
        }

        json.AppendLine(string.Join("," + Environment.NewLine, pathEntries));
        json.AppendLine("  ],");
        json.AppendLine("  \"matrix\": [");

        List<string> matrixEntries = [];
        for (int index = 0; index < count; index++)
        {
            StringBuilder entry = new();
            entry.AppendLine($"    {{\"out\": {index}, \"sources\": [");
            if (index == lfeIndex)
            {
                List<string> sources =
                    [$"        {{\"path\": {lfePath}, \"gain_db\": 0}}"];
                for (int other = 0; other < count; other++)
                {
                    if (other == lfeIndex)
                    {
                        continue;
                    }

                    sources.Add(
                        $"        {{\"path\": {lowPass[other]}, \"gain_db\": " +
                        $"{DspJson.NumberText(state.BassGain)}}}");
                }

                entry.AppendLine(string.Join("," + Environment.NewLine, sources));
            }
            else
            {
                entry.AppendLine(
                    $"        {{\"path\": {highPass[index]}, \"gain_db\": 0}}");
            }

            entry.Append("      ]}");
            matrixEntries.Add(entry.ToString());
        }

        json.AppendLine(string.Join("," + Environment.NewLine, matrixEntries));
        json.AppendLine("  ],");
        json.AppendLine("  \"_ui\": {");
        json.AppendLine($"    \"samplerate\": {state.SampleRate},");
        json.AppendLine($"    \"chunksize\": {state.ChunkSize},");
        json.AppendLine($"    \"bassGain\": {DspJson.NumberText(state.BassGain)},");
        json.AppendLine($"    \"lfeTrim\": {DspJson.NumberText(state.LfeTrim)},");
        json.AppendLine($"    \"layout\": [{DspJson.QuoteList(state.Layout)}],");
        json.AppendLine("    \"channels\": [");

        List<string> uiEntries = [];
        for (int index = 0; index < count; index++)
        {
            StageChannel channel = channels[index];
            uiEntries.Add(
                "      {" + Environment.NewLine +
                $"        \"name\": \"{DspJson.Escape(channel.Name)}\"," + Environment.NewLine +
                $"        \"lfe\": {BoolText(channel.Lfe)}," + Environment.NewLine +
                $"        \"hp\": {DspJson.NumberText(channel.Crossover)}," + Environment.NewLine +
                $"        \"delay\": {DspJson.NumberText(channel.Delay)}," + Environment.NewLine +
                $"        \"gain\": {DspJson.NumberText(channel.Gain)}," + Environment.NewLine +
                $"        \"invert\": {BoolText(channel.Invert)}," + Environment.NewLine +
                $"        \"peq\": \"{DspJson.Escape(channel.Peq)}\"" + Environment.NewLine +
                "      }");
        }

        json.AppendLine(string.Join("," + Environment.NewLine, uiEntries));
        json.AppendLine("    ]");
        json.AppendLine("  }");
        json.AppendLine("}");
        return json.ToString();
    }

    private static string BoolText(bool value) => value ? "true" : "false";

    private static string PathBlock(
        int input, string filters, StageChannel channel,
        double gainDb, bool invert, double delayMs)
    {
        StringBuilder body = new();
        body.AppendLine("    {");
        body.AppendLine($"      \"input\": {input},");
        body.AppendLine($"      \"filters\": [{filters}],");
        body.Append($"      \"gain_db\": {DspJson.NumberText(gainDb)}");
        if (delayMs != 0.0)
        {
            body.AppendLine(",");
            body.Append($"      \"delay_ms\": {DspJson.NumberText(delayMs)}");
        }

        if (invert)
        {
            body.AppendLine(",");
            body.Append("      \"invert\": true");
        }

        body.AppendLine();
        body.Append("    }");
        return body.ToString();
    }

    private static DspStagePointer ReadPointer(string path)
    {
        if (!File.Exists(path))
        {
            return new DspStagePointer(false, null);
        }

        try
        {
            using JsonDocument json = JsonDocument.Parse(File.ReadAllText(path));
            JsonElement root = json.RootElement;
            string? configPath = ReadString(root, "ConfigPath", string.Empty);
            return new DspStagePointer(
                ReadBool(root, "Enabled", false),
                string.IsNullOrWhiteSpace(configPath) ? null : configPath);
        }
        catch (Exception)
        {
            return new DspStagePointer(false, null);
        }
    }

    private static List<string> ReadStringArray(JsonElement node, string name)
    {
        List<string> values = [];
        if (node.ValueKind != JsonValueKind.Object ||
            !node.TryGetProperty(name, out JsonElement value) ||
            value.ValueKind != JsonValueKind.Array)
        {
            return values;
        }

        foreach (JsonElement item in value.EnumerateArray())
        {
            values.Add(item.ValueKind == JsonValueKind.String ? item.GetString() ?? "" : "");
        }

        return values;
    }

    private static int ReadInt(JsonElement node, string name, int fallback) =>
        node.ValueKind == JsonValueKind.Object &&
        node.TryGetProperty(name, out JsonElement value) &&
        value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out int result)
            ? result
            : fallback;

    private static double ReadDouble(JsonElement node, string name, double fallback) =>
        node.ValueKind == JsonValueKind.Object &&
        node.TryGetProperty(name, out JsonElement value) &&
        value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out double result)
            ? result
            : fallback;

    private static bool ReadBool(JsonElement node, string name, bool fallback) =>
        node.ValueKind == JsonValueKind.Object &&
        node.TryGetProperty(name, out JsonElement value) &&
        value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean()
            : fallback;

    private static string ReadString(JsonElement node, string name, string fallback) =>
        node.ValueKind == JsonValueKind.Object &&
        node.TryGetProperty(name, out JsonElement value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? fallback
            : fallback;
}
