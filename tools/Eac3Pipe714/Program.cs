using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Threading;

using Cavern;
using Cavern.Channels;
using Cavern.Format;
using Cavern.Format.Renderers;

// E-AC-3（含 Dolby Atmos / JOC）裸流 -> Cavern 解码与对象渲染 -> 12 声道 float32
// -> \\.\pipe\SpatialAudioLab.Pcm714 -> SpatialAudioLab.CLI omniphony-pcm -> 三卡。
//
// 两种输入：
//   Eac3Pipe714 input.ec3                      离线验证一个文件
//   Eac3Pipe714 --input-pipe SpatialAudioLab.Eac3   实时，接 CLI ec3-pipe 剥出来的裸流
//
// 管道那一端已经是现成的：CLI 的 omniphony-pcm 建服务端并等着客户端连上来，
// 只认 12 声道 float32 / 48 kHz 交错数据，正好是 Cavern 的 ref714。

const int Channels = 12;
const int SampleRate = 48000;
// 一个 E-AC-3 syncframe 是 1536 个采样（48 kHz 下 32 ms），
// 与 Cavern712 的 UpdateRate 取值一致，避免每次 Render 跨帧。
// 一个 E-AC-3 syncframe 就是 1536 个采样。UpdateRate 直接对齐它，
// 每次 Render() 推进一帧：
//   * Render(24) 那种批量推进是给文件源用的，批量里只会在开头拉一次数据，
//     数据不够就补静音且不再回头读，实时流会“读一帧然后永久静音”。
const int UpdateRate = 1536;

// Cavern 的 BlockBuffer.CreateForConstantPacketSize 是拿 Stream.Length 当“每次读多少
// 字节”的。管道没有长度，取 Length 会抛异常，它就退回 FormatConsts.blockSize ——
// 10 MB。10 MB 一块意味着第一块里只有几 KB 是真数据、其余全零，解码器读到 syncword 0
// 就判定流结束。所以给管道流一个固定块大小（8192 ≈ 2.7 帧 ≈ 85 ms @ 768 kbps）。
const int PipeBlockSize = 8192;

// FIFO 高水位：缓冲超过它就暂停从管道读，让内核管道缓冲区把压力传回 CLI。
// 这样内存占用和端到端延迟都是有界的（32 KB ≈ 0.35 s @ 768 kbps）。
const int PipeHighWater = 32768;

string input = null;
string inputPipe = null;
string pipeName = "SpatialAudioLab.Pcm714";
double seconds = 0;
double gain = 1;
bool loop = false;
bool nullOut = false;
bool outRing = false;
// 独立于 NSP 的一条桥：NSP_PCM714 只走 NativeSpatialPipe 的空间音频捕获，
// E-AC-3 直通不能占用它，否则两套生产者会互相覆盖 written。
string ringName = @"Local\SAL_EAC3_PCM714";
int ringCapacity = 16384;

for (int i = 0; i < args.Length; ++i) {
    switch (args[i]) {
        case "--null-out": nullOut = true; break;
        case "--out-ring": outRing = true; break;
        case "--ring-name": ringName = args[++i]; break;
        case "--ring-capacity": ringCapacity = int.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture); break;
        case "--input-pipe": inputPipe = args[++i]; break;
        case "--pipe": pipeName = args[++i]; break;
        case "--seconds": seconds = double.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture); break;
        case "--gain": gain = double.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture); break;
        case "--loop": loop = true; break;
        default:
            if (input is null) { input = args[i]; break; }
            PrintUsage();
            return 64;
    }
}

if (input is null && inputPipe is null) {
    PrintUsage();
    return 64;
}
if (input is not null) {
    // 文件模式下才解析绝对路径；管道模式没有文件。
    input = Path.GetFullPath(input);
    if (!File.Exists(input)) {
        Console.Error.WriteLine($"输入文件不存在：{input}");
        return 66;
    }
} else {
    // 管道不能重放，--loop 没有意义。
    loop = false;
}
if (gain < 0 || gain > 1) {
    Console.Error.WriteLine("--gain 必须在 0..1 之间。");
    return 64;
}

// 必须在构造 Listener 之前换掉全局声道布局，否则混音目标还是默认的立体声。
Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref714));

NamedPipeClientStream pcm = null;
NspRingWriter ring = null;
if (nullOut) {
    Console.WriteLine("--null-out：只解码不输出，用于测吞吐。");
} else if (outRing) {
    // E-AC-3 直通专用的独立桥。线格式与 NSP 一致（同一套 12ch float32 环协议），
    // 但用不同的名字，和 NativeSpatialPipe 的空间音频通道互不干扰。
    try {
        ring = new NspRingWriter(ringName, ringCapacity);
    } catch (Exception error) {
        Console.Error.WriteLine($"创建共享内存环 {ringName} 失败：{error.Message}");
        return 68;
    }
    Console.WriteLine(
        $"PCM 输出：共享内存环 {ringName}（{ring.Channels}ch / {ring.SampleRate} Hz / float32，" +
        $"{ring.Capacity} 帧 ≈ {ring.Capacity * 1000 / ring.SampleRate} ms）");
    Console.WriteLine($"接：SpatialAudioLab.CLI live-eac3-layout 0 <布局.ini> 0.25 40 balanced \"{ringName}\"");
} else {
    pcm = new NamedPipeClientStream(".", pipeName, PipeDirection.Out);
    try {
        pcm.Connect(10000);
    } catch (Exception error) {
        pcm.Dispose();
        Console.Error.WriteLine($"连不上 PCM 管道 \\\\.\\pipe\\{pipeName}：{error.Message}");
        Console.Error.WriteLine("先起 SpatialAudioLab.CLI omniphony-pcm（它是服务端）。");
        return 69;
    }
    Console.WriteLine($"PCM 管道已连接：\\\\.\\pipe\\{pipeName}");
}
using (pcm)
using (ring) {

long totalFrames = 0;
Stopwatch clock = Stopwatch.StartNew();

do {
    NamedPipeClientStream client = null;
    PipeFeedStream feed = null;
    Stream source;
    if (inputPipe is not null) {
        client = new NamedPipeClientStream(".", inputPipe, PipeDirection.In);
        try {
            Console.WriteLine($"等待 \\\\.\\pipe\\{inputPipe} 上的 E-AC-3 码流 ...");
            client.Connect(15000);
        } catch (Exception error) {
            client.Dispose();
            Console.Error.WriteLine($"连不上码流管道 \\\\.\\pipe\\{inputPipe}：{error.Message}");
            Console.Error.WriteLine("先起 SpatialAudioLab.CLI ec3-pipe（它是服务端），并让播放器开始直通。");
            return 70;
        }
        // 管道不能直接交给 Cavern：ReadBytes 只调一次 Read 且不看返回值，剩下的补零；
        // 管道 Read 只给当次可用的字节，第一块之后全是 0，解码器读到 syncword 0 就收工。
        // Cavern 自带的 QueueStream 能读满，但它拿不到数据时 while 空转烧满一核，
        // 对端关掉后更是永远转下去。所以用本地这个阻塞式 FIFO。
        feed = new PipeFeedStream(PipeBlockSize);
        PipeFeedStream target = feed;
        Thread pump = new(() => {
            byte[] temp = new byte[8192];
            try {
                while (true) {
                    // 高水位限流：缓冲够了就先不读管道，把背压交给内核管道缓冲区，
                    // 否则对端读文件远快于实时时会把整部片子堆进内存。
                    while (target.Buffered > PipeHighWater) {
                        Thread.Sleep(2);
                    }
                    int read = client.Read(temp, 0, temp.Length);
                    if (read <= 0) {
                        break;
                    }
                    target.Append(temp, 0, read);
                }
            } catch {
                // 对端关闭/断开都走这里，下面统一收尾。
            }
            target.Complete();
        }) { IsBackground = true, Name = "eac3-pump" };
        pump.Start();
        source = feed;
        Console.WriteLine($"码流管道已连接：\\\\.\\pipe\\{inputPipe}");
    } else {
        source = File.OpenRead(input);
    }

    using (source) {
        EnhancedAC3Reader reader = new(source);
        reader.ReadHeader();
        if (reader.ChannelCount <= 0 || reader.SampleRate <= 0) {
            // 空流（管道对端还没送来数据、或 ec3-pipe 连不上捕获驱动）时
            // ReadHeader 只会得到一堆 0，之后整条链路静默退出。这里给出明确原因。
            Console.Error.WriteLine(inputPipe is null
                ? "输入文件里没有解析出有效的 E-AC-3 帧。"
                : "码流管道没有送来有效的 E-AC-3 数据。先确认 ec3-pipe 已连上捕获驱动，并且播放器正在输出 E-AC-3。");
            return 72;
        }

        using Renderer renderer = reader.GetRenderer();
        Console.WriteLine(inputPipe is null ? $"输入：{Path.GetFileName(input)}" : "输入：实时 E-AC-3 码流");
        Console.WriteLine($"  采样率={reader.SampleRate} Hz, 核心声道={reader.ChannelCount}, " +
            $"对象={renderer.Objects.Count}, 有对象={renderer.HasObjects}, " +
            $"可seek={source.CanSeek}, 声明长度={reader.Length}");

        Listener listener = new(false) {
            SampleRate = SampleRate,
            UpdateRate = UpdateRate,
            AudioQuality = QualityModes.Perfect,
            // 与 Cinema 的 E-AC-3 路径一致：有对象时按 -3 dB 留出余量。
            Volume = (float)((renderer.HasObjects ? .707 : 1.0) * gain),
        };
        listener.AttachSources(renderer.Objects);

        // 节拍必须从“真正开始渲染”这一刻起算。clock 在等管道连接时已经走了一段，
        // 若沿用它，第一次循环会把这段空白当成“欠账”全速补上，一口气向 PCM
        // 管道灌进远超缓冲区容量的数据，阻塞在写管道上，反过来把码流端顶死。
        clock.Restart();
        totalFrames = 0;

        byte[] buffer = Array.Empty<byte>();
        long rendered = 0;
        long nextReport = 0;
        float peak = 0;
        bool finished = false;
        while (!finished) {
            // 码流端收工且缓冲已取空，就地收尾。不能指望 Cavern 自己报结束：
            // 流尾它的解码器只会返回空块，多转一圈就会在 FrameBasedDecoder 里撞 NRE。
            if (feed is not null && feed.Completed && feed.Buffered == 0) {
                Console.WriteLine("  码流管道已结束。");
                break;
            }

            float[] block;
            try {
                block = listener.Render();
            } catch (Exception error) when (error is EndOfStreamException or IOException) {
                break;
            }

            if (block is null || block.Length == 0) {
                break;
            }

            if (seconds > 0 && totalFrames + block.Length / Channels > seconds * SampleRate) {
                int allowed = (int)(seconds * SampleRate - totalFrames) * Channels;
                if (allowed <= 0) { finished = true; break; }
                Array.Resize(ref block, allowed);
                finished = true;
            }

            int bytes = block.Length * sizeof(float);
            if (buffer.Length < bytes) {
                buffer = new byte[bytes];
            }
            Buffer.BlockCopy(block, 0, buffer, 0, bytes);

            if (pcm is not null) {
                try {
                    pcm.Write(buffer, 0, bytes);
                    pcm.Flush();
                } catch (IOException error) {
                    Console.Error.WriteLine($"PCM 管道断开：{error.Message}");
                    return 71;
                }
            }
            if (ring is not null) {
                ring.Write(buffer, bytes);
            }

            // 峰值是判断“到底有没有声音”的唯一硬指标：链路全通但峰值恒为 0，
            // 说明解码/渲染出来就是静音，问题不在管道也不在声卡。
            for (int i = 0; i < block.Length; ++i) {
                float magnitude = Math.Abs(block[i]);
                if (magnitude > peak) { peak = magnitude; }
            }

            int frames = block.Length / Channels;
            rendered += frames;
            totalFrames += frames;

            // 每 2 秒报一次进度和落后量：落后持续变大就说明实时播放跟不上了。
            if (totalFrames >= nextReport) {
                nextReport = totalFrames + SampleRate * 2;
                double produced = totalFrames / (double)SampleRate;
                long consumed = feed?.TotalWritten ?? 0;
                string ringText = ring is null
                    ? string.Empty
                    : $"，环积压 {ring.Backlog} 帧、累计丢 {ring.Dropped} 帧";
                Console.WriteLine(
                    $"  ... {produced:F1} s 已解码，落后时钟 {clock.Elapsed.TotalSeconds - produced:F2} s，" +
                    $"峰值 {peak:F3}，已读码流 {consumed / 1024.0:F1} KB（{consumed / 3072.0:F0} 帧），" +
                    $"待解码 {feed?.Buffered ?? 0} B{ringText}");
                peak = 0;

                // 注：管道断连的主动检测在这里并不可靠（IsConnected 与
                // PeekNamedPipe 在阻塞读期间都会误报），交给上层守护进程处理。
            }

            // 按 1536 帧 / 32 ms 的节拍喂，否则文件会被一口气读完，
            // 环那边来不及消费，全变成 dropped。
            long dueMilliseconds = (long)(totalFrames * 1000.0 / SampleRate);
            int delay = (int)(dueMilliseconds - clock.ElapsedMilliseconds);
            if (delay > 0) {
                Thread.Sleep(delay);
            }
        }

        Console.WriteLine($"  本轮输出 {rendered / (double)SampleRate:F2} s，累计 {totalFrames / (double)SampleRate:F2} s");
    }

    // 本轮结束（--seconds 到点或码流断开）：断开码流端，别让 pump 空转着占住管道。
    client?.Dispose();

    if (seconds > 0 && totalFrames >= seconds * SampleRate) {
        break;
    }
} while (loop);
}

Console.WriteLine("完成。");
return 0;

static void PrintUsage() {
    Console.Error.WriteLine(
        "Eac3Pipe714 <input.ec3|input.eac3> [--out-ring] [--seconds N] [--gain 0..1] [--loop]");
    Console.Error.WriteLine(
        "Eac3Pipe714 --input-pipe SpatialAudioLab.Eac3 [--out-ring] [--ring-name name] " +
        "[--ring-capacity frames] [--seconds N] [--gain 0..1] [--null-out]");
    Console.Error.WriteLine(
        "  默认输出到 PCM 命名管道（--pipe name，默认 SpatialAudioLab.Pcm714）；" +
        "--out-ring 改为写 Local\\NSP_PCM714，由 CLI live-nsp-layout 消费。");
}

/// <summary>
/// 阻塞式 FIFO，把命名管道这种“随时短读、随时断开”的流伪装成 Cavern 要的
/// “一次 Read 必定读满”的流。
/// </summary>
/// <remarks>
/// Cavern 的 <c>StreamExtensions.ReadBytes</c> 只调用一次 <see cref="Stream.Read(byte[], int, int)"/>、
/// 根本不看返回值，剩下的字节全补零。所以短读等于把数据抹成 0，解码器读到
/// syncword 0 就认定流结束——这正是管道直连时“只有第一帧有声”的原因。
/// 这里 <see cref="Read"/> 会一直等到凑满 <c>count</c> 字节；只有对端结束时才提前返回，
/// 真的没有数据时返回 0。
/// </remarks>
sealed class PipeFeedStream : Stream {
    /// <summary>
    /// 待消费的数据块。整块进出，避免像 Cavern 的 QueueStream 那样逐字节入队。
    /// </summary>
    readonly Queue<byte[]> chunks = new Queue<byte[]>();

    /// <summary>
    /// 队首块已经取走的字节数。
    /// </summary>
    int headOffset;

    /// <summary>
    /// 还没被取走的字节总数。
    /// </summary>
    long buffered;

    /// <summary>
    /// 对端已经不会再写数据。
    /// </summary>
    bool completed;

    /// <summary>
    /// 对外声明的读取块大小，见 <see cref="Length"/>。
    /// </summary>
    readonly int blockSize;

    public PipeFeedStream(int blockSize) => this.blockSize = blockSize;

    /// <summary>
    /// 缓冲里还有多少字节没被读走。生产者据此做高水位限流。
    /// </summary>
    public long Buffered {
        get { lock (chunks) { return buffered; } }
    }

    /// <summary>
    /// 对端是否已经结束。
    /// </summary>
    public bool Completed {
        get { lock (chunks) { return completed; } }
    }

    /// <summary>
    /// 累计写入的字节数，用于统计码流吞吐。
    /// </summary>
    public long TotalWritten { get; private set; }

    /// <summary>
    /// 生产者推入一段数据。
    /// </summary>
    public void Append(byte[] data, int offset, int count) {
        if (count <= 0) {
            return;
        }
        byte[] copy = new byte[count];
        Buffer.BlockCopy(data, offset, copy, 0, count);
        lock (chunks) {
            chunks.Enqueue(copy);
            buffered += count;
            TotalWritten += count;
            Monitor.PulseAll(chunks);
        }
    }

    /// <summary>
    /// 生产者收工：唤醒正在等数据的读者，之后 <see cref="Read"/> 只在有存货时返回。
    /// </summary>
    public void Complete() {
        lock (chunks) {
            completed = true;
            Monitor.PulseAll(chunks);
        }
    }

    /// <summary>
    /// 块大小。Cavern 的 <c>BlockBuffer.CreateForConstantPacketSize</c> 会拿
    /// <see cref="Stream.Length"/> 当读取块大小，管道没有真实长度，取 Length 会抛异常，
    /// 它就退回 10 MB 一块——那等于把整条流当成“第一块 + 无穷零”。这里返回约定值。
    /// </summary>
    public override long Length => blockSize;

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;

    public override long Position {
        get => throw new NotSupportedException();
        set => throw new NotSupportedException();
    }

    public override void Flush() { }

    public override int Read(byte[] buffer, int offset, int count) {
        if (count <= 0) {
            return 0;
        }
        lock (chunks) {
            // 等满一整块：Cavern 会把没读到的部分当成 0，短读就是无声的静音。
            while (buffered < count && !completed) {
                Monitor.Wait(chunks);
            }
            int taken = (int)Math.Min(buffered, count);
            int written = 0;
            while (written < taken) {
                byte[] head = chunks.Peek();
                int next = Math.Min(head.Length - headOffset, taken - written);
                Buffer.BlockCopy(head, headOffset, buffer, offset + written, next);
                written += next;
                headOffset += next;
                if (headOffset == head.Length) {
                    chunks.Dequeue();
                    headOffset = 0;
                }
            }
            buffered -= written;
            return written;
        }
    }

    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    protected override void Dispose(bool disposing) {
        // 让可能还卡在 Read 里的线程醒过来，别把退出拖住。
        Complete();
        base.Dispose(disposing);
    }
}

/// <summary>
/// NativeSpatialPipe 的 PCM 环写入端（默认 <c>Local\NSP_PCM714</c>）。
/// </summary>
/// <remarks>
/// 环的线格式由 <c>src\nsp_ring_reader.h</c> 固定：64 字节头 + capacity 帧 × 12 声道
/// 交错数据，<c>written</c> 是单调递增的绝对帧计数，消费者按 <c>written % capacity</c>
/// 取模寻址、落后时自行跳过，所以生产者只管往前写，不需要等空间。
/// 伴随事件是 <c>&lt;name&gt;_Data</c>，消费者用 SYNCHRONIZE 打开。
/// </remarks>
sealed class NspRingWriter : IDisposable {
    /// <summary>环头长度，与 nsp_ring_reader.h 的 NspHeader 一致。</summary>
    const int HeaderBytes = 64;

    /// <summary>"NSP7" 小端。</summary>
    const uint Magic = 0x3750534E;

    const uint Version = 1;

    /// <summary>0 = int16，1 = float32。Cavern 直接出 float32，不走量化。</summary>
    const uint FormatFloat32 = 1;

    const uint PageReadWrite = 0x04;
    const uint FileMapAllAccess = 0xF001F;
    const int ErrorAlreadyExists = 183;

    /// <summary>头内偏移。</summary>
    const int OffsetMagic = 0;
    const int OffsetVersion = 4;
    const int OffsetChannels = 8;
    const int OffsetSampleRate = 12;
    const int OffsetFormat = 16;
    const int OffsetCapacity = 20;
    const int OffsetWritten = 24;
    const int OffsetRead = 32;
    const int OffsetDropped = 40;
    const int OffsetPushes = 48;
    const int OffsetReserved = 56;

    readonly IntPtr mapping;
    readonly IntPtr view;
    readonly IntPtr data;
    readonly EventWaitHandle dataEvent;
    long writeCursor;
    bool disposed;

    /// <summary>实际生效的环深度（帧）。沿用已存在的环时可能与请求值不同。</summary>
    public int Capacity { get; }

    public int Channels => 12;

    public int SampleRate => 48000;

    /// <summary>累计被新数据挤掉、消费者没读到的帧数。</summary>
    public long Dropped { get; private set; }

    /// <summary>
    /// 消费者还欠多少帧没读。写端按墙钟推进、读端按声卡时钟拉取，两者不同源，
    /// 这个数长期贴着容量上限就说明写快于读、环在周期性丢帧。
    /// </summary>
    public long Backlog => writeCursor - Marshal.ReadInt64(view, OffsetRead);

    public long Pushes { get; private set; }

    public NspRingWriter(string name, int capacityFrames) {
        if (string.IsNullOrWhiteSpace(name)) {
            throw new ArgumentException("环名不能为空。", nameof(name));
        }
        if (capacityFrames < 1024) {
            throw new ArgumentOutOfRangeException(nameof(capacityFrames), "环深度至少 1024 帧。");
        }

        int frameBytes = Channels * sizeof(float);
        int totalBytes = HeaderBytes + capacityFrames * frameBytes;
        mapping = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, PageReadWrite,
            0, (uint)totalBytes, name);
        if (mapping == IntPtr.Zero) {
            throw new System.ComponentModel.Win32Exception(
                Marshal.GetLastWin32Error(), $"CreateFileMapping 失败：{name}");
        }
        bool existed = Marshal.GetLastWin32Error() == ErrorAlreadyExists;

        // 0 表示映射到对象末尾，这样即便已有环的容量比我们请求的大也不会越界。
        view = MapViewOfFile(mapping, FileMapAllAccess, 0, 0, UIntPtr.Zero);
        if (view == IntPtr.Zero) {
            int lastError = Marshal.GetLastWin32Error();
            CloseHandle(mapping);
            throw new System.ComponentModel.Win32Exception(lastError, $"MapViewOfFile 失败：{name}");
        }

        uint magic = (uint)Marshal.ReadInt32(view, OffsetMagic);
        uint channels = (uint)Marshal.ReadInt32(view, OffsetChannels);
        uint rate = (uint)Marshal.ReadInt32(view, OffsetSampleRate);
        uint format = (uint)Marshal.ReadInt32(view, OffsetFormat);
        uint capacity = (uint)Marshal.ReadInt32(view, OffsetCapacity);

        if (existed) {
            // NSP 采集进程或上一轮留下的环：只接受完全一致的格式，再沿用它的深度。
            if (magic != Magic || (uint)Marshal.ReadInt32(view, OffsetVersion) != Version ||
                channels != (uint)Channels || rate != (uint)SampleRate ||
                format != FormatFloat32 || capacity == 0) {
                UnmapViewOfFile(view);
                CloseHandle(mapping);
                throw new InvalidOperationException(
                    $"共享内存名 {name} 已被占用，但内容不是 12ch/48k/float32 的 NSP 环。");
            }
            Capacity = (int)capacity;
            // 上一轮的 written/read 是另一条时间线，从消费者当前位置续写，
            // 否则第一次读会把上一秒的残留当成新声音播出来。
            long read = Marshal.ReadInt64(view, OffsetRead);
            writeCursor = read;
            Marshal.WriteInt64(view, OffsetWritten, writeCursor);
        } else {
            Marshal.WriteInt32(view, OffsetMagic, (int)Magic);
            Marshal.WriteInt32(view, OffsetVersion, (int)Version);
            Marshal.WriteInt32(view, OffsetChannels, Channels);
            Marshal.WriteInt32(view, OffsetSampleRate, SampleRate);
            Marshal.WriteInt32(view, OffsetFormat, (int)FormatFloat32);
            Marshal.WriteInt32(view, OffsetCapacity, capacityFrames);
            Marshal.WriteInt64(view, OffsetWritten, 0);
            Marshal.WriteInt64(view, OffsetRead, 0);
            Marshal.WriteInt64(view, OffsetDropped, 0);
            Marshal.WriteInt64(view, OffsetPushes, 0);
            Marshal.WriteInt64(view, OffsetReserved, 0);
            Capacity = capacityFrames;
            writeCursor = 0;
        }

        data = IntPtr.Add(view, HeaderBytes);
        // 手动创建比等消费者先起来更稳：读端只是 OpenEventW(SYNCHRONIZE)。
        dataEvent = new EventWaitHandle(false, EventResetMode.AutoReset, name + "_Data", out _);
    }

    /// <summary>
    /// 把一段 12 声道交错 float32 PCM 推进环里。
    /// </summary>
    /// <param name="interleaved">交错样本</param>
    /// <param name="byteCount">有效字节数，必须是 48 的整数倍</param>
    public void Write(byte[] interleaved, int byteCount) {
        if (disposed) {
            return;
        }

        int frameBytes = Channels * sizeof(float);
        int frames = byteCount / frameBytes;
        if (frames <= 0) {
            return;
        }
        // 单次写入不可能超过整个环，超了只保留最后 Capacity 帧。
        int startFrame = 0;
        if (frames > Capacity) {
            startFrame = frames - Capacity;
            Dropped += frames - Capacity;
            frames = Capacity;
        }

        // 统计这一次写入覆盖掉了多少“消费者还没读”的帧。必须取增量：
        // 用总积压减去容量的话，无消费者时积压本身在涨，dropped 会按平方增长。
        long read = Marshal.ReadInt64(view, OffsetRead);
        long before = Math.Max(0, writeCursor - read);
        long after = Math.Max(0, before + frames);
        long overflow = Math.Max(0, after - Capacity) - Math.Max(0, before - Capacity);
        if (overflow > 0) {
            Dropped += overflow;
        }

        int slot = (int)(writeCursor % Capacity);
        int head = Math.Min(frames, Capacity - slot);
        Marshal.Copy(interleaved, startFrame * frameBytes,
            IntPtr.Add(data, slot * frameBytes), head * frameBytes);
        if (head < frames) {
            Marshal.Copy(interleaved, (startFrame + head) * frameBytes,
                data, (frames - head) * frameBytes);
        }

        writeCursor += frames;
        Marshal.WriteInt64(view, OffsetWritten, writeCursor);
        ++Pushes;
        Marshal.WriteInt64(view, OffsetPushes, Pushes);
        Marshal.WriteInt64(view, OffsetDropped, Dropped);
        dataEvent.Set();
    }

    public void Dispose() {
        if (disposed) {
            return;
        }
        disposed = true;
        dataEvent.Dispose();
        if (view != IntPtr.Zero) {
            UnmapViewOfFile(view);
        }
        if (mapping != IntPtr.Zero) {
            CloseHandle(mapping);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(IntPtr hFile, IntPtr fileMappingAttributes,
        uint protect, uint maximumSizeHigh, uint maximumSizeLow, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr mappingObject, uint desiredAccess,
        uint fileOffsetHigh, uint fileOffsetLow, UIntPtr bytesToMap);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UnmapViewOfFile(IntPtr baseAddress);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);
}
