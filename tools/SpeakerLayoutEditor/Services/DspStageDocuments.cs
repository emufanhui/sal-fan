using System.Globalization;
using System.Linq;
using System.Text;

namespace SpeakerLayoutEditor.Services;

/// <summary>单个 PEQ 段。参数串写成 freq:gain:q，多段用逗号或分号分隔。</summary>
internal sealed record PeqBand(double Freq, double Gain, double Q);

/// <summary>
/// 一个声道的低音管理参数。字段名与 SalAsio 的 dsp_editor 保持一致，
/// 这样同一份 json 两边读出来的是同一个东西。
/// </summary>
internal sealed record StageChannel(
    string Name,
    bool Lfe,
    double Crossover,
    double Delay,
    double Gain,
    bool Invert,
    string Peq);

/// <summary>
/// 一份完整的 dsp 配置。paths/matrix 由它生成，_layout/_ui 会写回文件。
/// </summary>
internal sealed record DspStageState(
    int SampleRate,
    int ChunkSize,
    double BassGain,
    double LfeTrim,
    IReadOnlyList<string> Layout,
    IReadOnlyList<StageChannel> Channels);

/// <summary>
/// Studio 自己的状态：开关与当前选中的 json 路径。刻意不写进 dsp.json，
/// 那份文件要拿给 SalAsio 共用，塞 Studio 私有的开关进去不合适。
/// </summary>
internal sealed record DspStagePointer(bool Enabled, string? ConfigPath);

/// <summary>手写 JSON 用到的小工具，同时被读写两侧复用。</summary>
internal static class DspJson
{
    /// <summary>整数不带小数尾巴，其余最多六位有效数字，固定用 "." 做小数点。</summary>
    public static string NumberText(double value) =>
        Math.Abs(value) < 1e12 && Math.Abs(value - Math.Round(value)) < 1e-9
            ? value.ToString("0", CultureInfo.InvariantCulture)
            : value.ToString("0.######", CultureInfo.InvariantCulture);

    public static string Escape(string text)
    {
        StringBuilder result = new(text.Length + 8);
        foreach (char character in text)
        {
            switch (character)
            {
                case '"': result.Append("\\\""); break;
                case '\\': result.Append("\\\\"); break;
                case '\n': result.Append("\\n"); break;
                case '\r': result.Append("\\r"); break;
                case '\t': result.Append("\\t"); break;
                default: result.Append(character); break;
            }
        }

        return result.ToString();
    }

    public static string QuoteList(IEnumerable<string> values) =>
        string.Join(", ", values.Select(value => "\"" + Escape(value) + "\""));

    /// <summary>
    /// freq:gain:q，多段用逗号或分号隔开。不足三个数或 q 不为正的段直接丢掉，
    /// 与 SalAsio 一致 —— 半截 PEQ 比静默错位安全。
    /// </summary>
    public static IReadOnlyList<PeqBand> ParsePeq(string text)
    {
        List<PeqBand> bands = [];
        if (string.IsNullOrWhiteSpace(text))
        {
            return bands;
        }

        foreach (string part in text.Split([',', ';'], StringSplitOptions.RemoveEmptyEntries))
        {
            List<double> numbers = [];
            foreach (string piece in part.Split([':', ' ', '\t'],
                                               StringSplitOptions.RemoveEmptyEntries))
            {
                if (numbers.Count >= 3)
                {
                    break;
                }

                if (double.TryParse(piece, NumberStyles.Float,
                                    CultureInfo.InvariantCulture, out double value))
                {
                    numbers.Add(value);
                }
            }

            if (numbers.Count >= 3 && numbers[2] > 0.0)
            {
                bands.Add(new PeqBand(numbers[0], numbers[1], numbers[2]));
            }
        }

        return bands;
    }
}
