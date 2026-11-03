using System.ComponentModel;
using System.Globalization;

namespace SpeakerLayoutEditor.Services;

/// <summary>
/// 低音管理面板里的一行。与服务层的 <see cref="StageChannel"/> 是两回事：
/// 这个是给 WPF 绑定的可变版本，保存时才折回去。
/// </summary>
internal sealed class StageChannelRow : INotifyPropertyChanged
{
    private bool lfe;
    private double crossover;
    private double delayMs;
    private double gainDb;
    private bool invert;
    private string peq = string.Empty;

    public StageChannelRow(string name)
    {
        Name = name;
    }

    public string Name { get; }

    public bool IsLfe
    {
        get => lfe;
        set
        {
            if (lfe != value)
            {
                lfe = value;
                OnPropertyChanged(nameof(IsLfe));
            }
        }
    }

    public double Crossover
    {
        get => crossover;
        set
        {
            if (Math.Abs(crossover - value) > 1e-9)
            {
                crossover = Math.Clamp(value, DspStageService.MinimumFrequencyHz,
                                       DspStageService.MaximumFrequencyHz);
                OnPropertyChanged(nameof(Crossover));
            }
        }
    }

    public double DelayMs
    {
        get => delayMs;
        set
        {
            if (Math.Abs(delayMs - value) > 1e-9)
            {
                delayMs = Math.Clamp(value, 0, 500);
                OnPropertyChanged(nameof(DelayMs));
            }
        }
    }

    public double GainDb
    {
        get => gainDb;
        set
        {
            if (Math.Abs(gainDb - value) > 1e-9)
            {
                gainDb = Math.Clamp(value, -60, 24);
                OnPropertyChanged(nameof(GainDb));
            }
        }
    }

    public bool Invert
    {
        get => invert;
        set
        {
            if (invert != value)
            {
                invert = value;
                OnPropertyChanged(nameof(Invert));
            }
        }
    }

    public string Peq
    {
        get => peq;
        set
        {
            if (peq != value)
            {
                peq = value;
                OnPropertyChanged(nameof(Peq));
                OnPropertyChanged(nameof(PeqHint));
            }
        }
    }

    /// <summary>
    /// PEQ 串的解释结果，用来当场告诉用户填的东西有没有被采纳 ——
    /// 半截或非法段会被 ParsePeq 丢掉，静默丢掉最容易被当成 bug。
    /// </summary>
    public string PeqHint
    {
        get
        {
            IReadOnlyList<PeqBand> bands = DspJson.ParsePeq(peq);
            if (string.IsNullOrWhiteSpace(peq))
            {
                return string.Empty;
            }

            return bands.Count == 0
                ? "无效"
                : string.Join(" / ", bands.Select(band =>
                    $"{band.Freq.ToString("0.#", CultureInfo.InvariantCulture)}Hz " +
                    $"{band.Gain.ToString("0.#", CultureInfo.InvariantCulture)}dB"));
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public static StageChannelRow From(StageChannel channel) => new(channel.Name)
    {
        IsLfe = channel.Lfe,
        Crossover = channel.Crossover,
        DelayMs = channel.Delay,
        GainDb = channel.Gain,
        Invert = channel.Invert,
        Peq = channel.Peq
    };

    public StageChannel ToChannel() =>
        new(Name, IsLfe, Crossover, DelayMs, GainDb, Invert, Peq.Trim());

    public void Revalidate()
    {
        // 滑杆与复选框即时回写，TextBox 默认等 LostFocus 才回写，
        // 所以保存前统一刷新一次，保证提示与实际落盘的值一致。
        OnPropertyChanged(nameof(Crossover));
        OnPropertyChanged(nameof(DelayMs));
        OnPropertyChanged(nameof(GainDb));
        OnPropertyChanged(nameof(PeqHint));
    }

    private void OnPropertyChanged(string name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
