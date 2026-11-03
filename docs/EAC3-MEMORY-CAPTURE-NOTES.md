# 正常 Windows 模式下从内存读取 E-AC-3(DD+/JOC) 码流：策略、数据与失败原因汇总

> 记录时间：2026-10-31
> 目标读者：后续接手的自己/他人
> 目标：在**不启用测试签名**的普通 Windows 模式里，从正在播放 Atmos(DD+/JOC Bitstream) 的播放器拿到**顺序正确、零丢帧**的 E-AC-3 码流，交给 Cavern(`Eac3Pipe714`) 解 JOC → 12 声道 → 多端点输出。

---

## 0. 一页结论（TL;DR）

| 结论 | 说明 |
|---|---|
| **Apple Music 在用户态没有 IEC61937/MAT 字节** | 扫 `audiodg` / `AMPLibraryAgent` / `AppleMusic` 三个进程，`F8 72 4E 1F` 命中数恒为 0；交给引擎的是 PCM+对象，MAT 封装在引擎/驱动侧 |
| **Apple Music 的 DD+/JOC 原始码流存在于 `AMPLibraryAgent.exe` 的私有堆** | 可被 `ReadProcessMemory` 读到；`ffprobe` 验证为 `Dolby Digital Plus + Dolby Atmos, 48kHz, 5.1(side)`，448/768 kbps 两档 |
| **Vivid Player(UWP) 的码流存在于 `VividPlayer.App` 的 `MEM_MAPPED` 区** | 一次扫描可发现 7 个候选环（64KB~468KB），首帧即 `0B 77` |
| **顺序问题是致命瓶颈** | 帧集合可以 100% 抓全，但顺序错乱 → 听感“气泡音/不均匀卡顿”。单环 + 地址序最好 99.68%，多环混扫只有 24% |
| **guard page 能拿到 100% 正确顺序，但会破坏播放** | 每次写都进 VEH，音频线程被拖慢 → 声音卡顿，Vivid 直接崩溃 |
| **WASAPI 提交点（`IAudioRenderClient::ReleaseBuffer`）拿不到码流** | 钩子确实被调用（release 计数增长），但 `GetBuffer` 数据不是 `0B77/F872` → 这两个播放器在用户态都是 PCM 提交 |
| **目前唯一"干净"的抓取仍是虚拟驱动 IEC61937 路线** | 即 `tools\Ec3Recorder` + `SpatialAudioLab.CLI ec3-pipe`，代价是需要测试签名模式 |

一句话：**Apple Music / Vivid 这类"应用侧 Atmos"在用户态能看到码流，但没有任何无侵入手段能同时保证"顺序正确 + 不影响播放"。**

---

## 1. 关键事实（实测，可复用）

### 1.1 码流参数

| 项 | 值 |
|---|---|
| 采样率 | 48000 Hz（`fscod=0`） |
| 每帧样本 | 1536（`numblks=3`） |
| 帧率 | 31.25 帧/秒（32 ms/帧） |
| `bsid` | 16（E-AC-3） |
| `acmod / lfeon` | 7 (5.1) / 1 |
| channel_layout | `5.1(side)` |
| 帧长（768k） | 3072 B（`frmsiz=1535`） |
| 帧长（448k） | 1792 B |
| 同步字 | `0B 77`；IEC61937 封装为 `F8 72 4E 1F`（burst type 21 = DD+） |
| 对象数 | Cavern 解码报 `对象=16, 有对象=True` |

> **实测否定了“44.1kHz / 位深不同”的猜测**：抓取流与虚拟驱动录制的参考录音在 `fscod/numblks/bsid/frmsiz` 上完全一致。

### 1.2 写入模型（决定了一切策略的成败）

1. **批写**：播放器每 **128 ms 一次性写 4 帧**（不是每 32 ms 写 1 帧）。
   - 6 ms 轮询下，多数 pass 只有 0~1 个新帧，每 4 个 pass 出现一批；
   - 4 ms 轮询统计：`{每 pass 4 帧: 149 次, 3 帧: 10, 2 帧: 13, 1 帧: 9}`。
2. **多环**：一个 `VirtualAlloc` 大区里并存**多条互不相同的流**（不是同一流的镜像），且播放器会在多个环之间轮换。
   - Apple 单次 dump：`reg0 268 帧 / reg3 261 / reg2 30 / reg1 5`，合计 **37.6 帧/秒 > 31.25**；
   - Vivid：7 个候选环 `65536/18帧, 65536/19, 65536/19, 131072/39, 131072/39, 294912/78, 479232/78`。
3. **槽位不按写入顺序排列**：同一时刻环内的 `(offset, 曲目帧号)` 是乱序的，例：
   ```
   (15456,1141) (24672,1146) (27744,1148) (30816,1142) (36960,1143)
   (40032,1145) (43104,1144) (49248,1149) (52320,1147)
   ```
   ⇒ **按 offset 排序无效**（最好也只有 52%）。
4. **环几何**：帧偏移满足 `align + k*3072`，`align = firstOff % 3072`（实测 1072 或 2144）；环长 131072（128 KiB）⇒ 131072/3072 = 42.67，**每圈必有一帧跨环**（head 在环尾、tail 在环首）。
5. **环头字段不可靠**：`region+0x10` 形如 `0x0002000f / 0x00030015`（高16疑似seq、低16疑似slot）、`region+0x20` 形如 `0x00150000`（高16 = 21 槽）；实测多处 slot 恒为 0，**不能用来定序**。

### 1.3 码流在哪个进程（重要）

| 播放器 | 码流位置 | 说明 |
|---|---|---|
| Apple Music | **`AMPLibraryAgent.exe`** 私有堆 | `AppleMusic.exe` 只有 1 个 10 MB 假阳性区；`audiodg` 里 0 帧 |
| Vivid Player (UWP) | `VividPlayer.App` 的 `MEM_MAPPED` 区 | 旧代码只扫 `MEM_PRIVATE` 会得 0 帧 |
| mpv (SPDIF 直通) | mpv 自身进程 | 能读到，但环布局与 Apple 不同（帧间有 8 B 前导），链式续接不成立 |
| audiodg | **0 帧** | 任何时刻都扫不到 E-AC-3，`F8 72` 也 0 命中 |

---

## 2. 尝试过的策略清单（按时间顺序）

### S1. 挂 audiodg `RVA 0x31930`（NSP 的做法）—— **失败**
- 依据：NSP 的 `Native-Hook.c` 在该地址 detour，声称拿到空间音频 PCM。
- 实测：注入成功（`ready` JSON 正常），但
  - 普通 LPCM 播放：`hits=0`
  - 真 7.1.4 对象流（`spatial-test 20 27U8A-S 714`）：`hits=0`
  - Apple Music 播放中：`hits=0`
- 结论：该 RVA 在当前 Windows build（10.0.26100.8875）上**不在任何实际音频路径上**；之前的“prologue 字节匹配”只是 MSVC 通用函数序言，不能证明语义。

### S2. 挂 KS 层 `DeviceIoControl / NtDeviceIoControlFile`（`IOCTL_KS_WRITE_STREAM 0x002F8013`）—— **失败**
- 注入 `audiodg` 与 `mpv`，各 12 s：只看到 `0x002f0003`（KS property set）和 `0x00222017`，**零次 `0x002F8013`**。
- `WriteFile/NtWriteFile` 也没有携带码流。
- 结论：bitstream 端点走的是 WaveRT 映射缓冲，**不经过任何可 hook 的 Win32 I/O 调用**。

### S3. 扫 `audiodg` 可写区找 `F8 72` / `0B 77`（`SalBufScan`）—— **全部假阳性**
- 15 s / 1 GB 扫描、299 hits、18 个 blob；解码后发现 dtype/blen 全乱（`dtype=0x06 sub=0xe7 blen=65184`，payload 是 float PCM），ASCII `MAT` 0 次。
- 教训：`F8 72` 在浮点数据里随机出现的概率不低，**必须配合 bsid/帧长/后继帧三重校验**。

### S4. 挂映射视图（`MapViewOfFile/NtMapViewOfSection`，`SalWrt`）—— **失败**
- mpv 14,000+ 次映射记录、0 命中；audiodg 只有 4 个既有映射、0 命中。
- 1 ms 采样器在几百 MB 上太慢（audiodg 一次跑出 6.6M fault）。

### S5. 外部 `ReadProcessMemory` 扫区（关键转折）—— **成功找到码流**
- C# `Add-Type` + `OpenProcess(0x410)` + `VirtualQueryEx` + `ReadProcessMemory`，扫 committed RW、4KB~16MB、校验 `0B77 + bsid==16 + 帧长 256..8192`。
- Apple Music 播放中：`AMPLibraryAgent [26732] hits=63`；区域 dump 后 `ffprobe` 确认 **DD+ Atmos 6ch**。
- 用“加载 `ddp.ec3` 的 holder 进程”做对照，`hits=781`（正好等于文件帧数）⇒ 扫描器可信。
- 代价：`ReadProcessMemory` 会短暂挂起目标线程；8 ms 读 ~700 KB（≈87 MB/s）能把 Apple 的音频线程拖到停播。

### S6. 抓取器 v1~v7（外部轮询）—— **覆盖/顺序都不过关**
| 版本 | 策略 | 结果 |
|---|---|---|
| v1 | 每个候选区独立锚点、从锚点连续发 | 0 帧（锚点落在回绕点，后面没有“连续帧”） |
| v2 | 整区 diff（只发变化的帧） | 859 帧/10 s ≈ 2.7× 重复（写入游标处的半帧被反复发） |
| v3 | 同 offset 两次读一致才发（稳定帧） | 303 帧/10 s = 9.70 s ✔ 但 4 处坏帧 |
| v4 | 按写入游标单调输出 | 216 帧/8 s（86%），用户“依旧卡顿” |
| v5 | 每轮只排空“新帧最多”的环 | 75% 覆盖，**用户直接听出“提速”** |
| v6 | 多环全扫 + 双读校验 | 100% 覆盖但顺序 24%（多环交错） |
| v7 | 换环时按“最后发出帧的 hash”找锚点续接 | 86~91%，步进 `{4:57,10:6}`（每批丢 3 帧） |

### S7. 环几何修复（splice 跨环帧）—— **部分成功**
- 已知 `ringStart≈1072, ringEnd≈132144`，对 `off+len > ringEnd` 的帧，用 `ringStart` 处的 tail 覆盖后 `tail` 字节。
- 20 anomalies/346 帧 → 解决了大部分撕裂，但仍有 4~5% 坏帧；Cavern 对单帧损坏即抛 `IndexOutOfRangeException`。

### S8. 6-pass 稳定（`STABLE_PASSES 6`）—— **80.5% 覆盖，仍气泡**
- 要求同一 offset 连续 6 个 pass 不变（≈150 ms）才发；结果 755 帧/30 s。
- 用户听完：“如果你说那是撕裂音也行，总之问题依旧” ⇒ 撕裂不是主因。

### S9. guard page 记录写入顺序（`sal_watch3`）—— **顺序 100%，但破坏播放**
- 把环页设 `PAGE_GUARD`，VEH 里记录 `ExceptionInformation[1]`（真实访问地址）后还原 guard。
- Apple：468 条记录、`hit 468 / mono+1 467`、**ratio 100.0%**。
- 但用户实测：“当抓取器工作的时候，连外接音响的声音都变得卡顿了，还把 vivid 搞崩溃了” ⇒ **每次写都进 VEH 的代价不可接受**，路线放弃。

### S10. 进程内被动快照（`sal_ddptap_p1`）—— **不可用**
- 1 ms 周期把环 `memcpy` 到私有快照再解析，无 guard、无页错误；帧按“上帧末尾 == 本帧起点”链式续接。
- mpv 14 s 只 20 帧（0.64 s），mpv 还自行退出。

### S11. 挂 `memcpy/memmove`（`sal_wcopy`、`sal_memtap`）—— **0 帧或崩溃**
- `sal_wcopy`：8 detour + 各自 trampoline → 注入 mpv 两次 `0xC0000005`，mpv 崩溃。
- `sal_memtap`：挂 `ucrtbase!memcpy` + `ntdll!memmove`，`ready {"rings":3,"memcpy":1,"memmove":1}` 但 0 帧 ⇒ 写路径不走这两个导出（内联/其他实现）。

### S12. WASAPI 提交点（`sal_wcl`，patch COM vtable）—— **钩到了，但数据是 PCM**
- 做法：遍历 `EnumAudioEndpoints` 逐个 `Activate(IAudioClient)`+`Initialize`，取一个成功实例的 vtable，改写 `acv[3]`(Initialize)、`rcv[3]`(GetBuffer)、`rcv[4]`(ReleaseBuffer)。
- 结果：MPC-BE `release=126`/4 s、mpv `release=1233`/12 s、Vivid `release=412`/12 s，**全部 `frames=0`**；同期 `device-format` 仍是 `8ch/192000/Dolby MLP / MAT 1.0`（用户态提交是 PCM，MAT 在更下层封装）。
- 副产物：`0x8889000A`（`AUDCLNT_E_ENDPOINT_CREATE_FAILED`）说明 HDMI 端点被占用/MAT 模式，必须遍历其他端点借 vtable。

### S13. 批内 4 帧的定序（关键难点）
- 4 ms 轮询时，一次 pass 会同时看到 4 个新帧，`t` 相同 ⇒ **先后不可判定**；
- 尝试用环头 `+0x10`/`+0x20` 的 seq/slot 推断：v8/v9 只有 46~55%；
- 结论：只有“页/地址级写入顺序”能定序（S9），或把轮询降到 <8 ms 让每 pass 只剩 1 帧（不现实：Apple 是批量写）。

### S14. 定位写入函数（`sal_watch4` + 调用栈）—— **能定位，但代价同 S9**
- guard 命中时 `RtlCaptureStackBackTrace`；RIP 解析结果：`ntdll+0x1283d`、`RTWorkQ+0xc443`、`ucrtbase+0xee44d/0xee451` ⇒ 环页由 **ucrtbase 的 memcpy/memmove** 写入，调用者在 Apple 代码里。
- mpv 上验证有效（`caller` 解析到 `mpv.exe+0xf737f0` 等）。
- 坑：`Get-Process … $p.Modules` 的 `BaseAddress` 是 `IntPtr`，需 `.ToInt64()`；PowerShell 单引号里的 `\n` 是字面量，要拼 `` `r`n ``。

### S15. MAT 扫描（`mat2.ps1`）—— **确认用户态没有 MAT**
- 找 `F8 72 4E 1F` 并解 burst type/len：`audiodg 0 / AMPLibraryAgent 0 / AppleMusic 0`。

---

## 3. 度量汇总（对参考录音逐帧打分）

方法：抓取帧按 3072 B 求 SHA1，在 `C:\Users\fanhu\Music\01 - Ariana Grande - Everyday (feat. Future).ec3`（13,609 帧全唯一）里查索引，统计**单调映射率**与**步进=1 比例**。

| 方案 | 帧数/时长 | 覆盖率 | 单调率 / 步进1 | 结论 |
|---|---|---|---|---|
| raw3 外部 dump（20 s） | 747 / 21.98 s | ~100% | 51% / — | 集合完整、顺序错 |
| 单环 + 地址序（w4） | 629 / 20 s | 100% | **99.68%（626/629）** | 最好，但换环就断 |
| 多环全扫（w6） | 943 / 30 s | 100% | 24% | 交错 |
| 单环 + hash 锚点续接（w7） | 807 / 30 s | 86% | 88.29% | 每批丢 3 帧 |
| 同上 + 250 ms 阈值 | 825 / 30 s | 88% | 91.36% | 仍丢 |
| guard page 地址序（watch3） | 468 / 14.9 s | 100% | **100.0%** | 顺序完美，但破坏播放 |
| 6-pass 稳定 | 755 / 30 s | 80.5% | — | 丢帧 |
| v5（挑最大环） | 469 / 15 s | 75% | 99% | 用户听出“提速” |

---

## 4. 失败根因（为什么这条路这么难）

1. **没有官方接口**：Windows 不暴露“某进程当前提交的 bitstream”。
2. **码流藏在堆里，布局无常**：多环、多流、槽位乱序、跨环撕裂，且随播放器版本/曲目变化。
3. **批量写 4 帧**：任何 >8 ms 的采样周期都会把 4 帧塞进同一时刻，顺序无解。
4. **定序信号只有“写地址”**：guard page 能拿到，但 VEH 开销直接毁掉音频线程；环头字段不可信。
5. **跨进程读取有副作用**：`ReadProcessMemory` 会挂起目标，高频读 = 制造欠载 → 用户听到的卡顿可能来自抓取器本身。
6. **应用侧不是 bitstream 提交**：Apple/Vivid 提交 PCM+对象，MAT 在引擎/驱动层；这一点已被 S15 与 S12 双重证实。

---

## 5. 副作用事故记录（务必避免重演）

| 事故 | 原因 | 后果 / 恢复 |
|---|---|---|
| Apple Music 停播（多次） | 高频 `ReadProcessMemory`（87 MB/s）或 guard page | 声音停、进度条冻结；结束 `AppleMusic`+`AMPLibraryAgent` 重开即可；端点若挂需重启 |
| `AUDCLNT_E_ENDPOINT_CREATE_FAILED 0x8889000A` | 反复 `Stop-Process audiodg` | HDMI 端点无法创建，重启恢复 |
| 写出 **12.24 GB** `fast.ec3` | `sal_ddptap_fast` 去重失效，每 pass 重发整环 | 磁盘写满、Apple 崩溃；已加“落盘硬上限”教训 |
| Vivid 崩溃 | guard page + 高频扫描 | 重开播放器 |
| AppleMusic 崩溃 `0xc0000005` @ `AMP.Services.dll+0x36a14c` | 多轮注入后 | 用户手动重开 |

**规则**：任何落盘抓取必须 ≤200 MB 硬上限；抓取器必须能一键停；不要杀 `audiodg`。

---

## 6. 工程坑清单

- 提权脚本必须**纯 ASCII**（PS 5.1 按 ANSI 读 UTF-8-no-BOM）；中文目录用 `Get-ChildItem 'D:\sp' -Directory | ? Name -like '*cinema*'` 定位。
- `Start-Process -ArgumentList` **不会自动加引号**：带空格路径要自己包 `'"'`；不含空格的 ini 不能加引号。
- `Start-Process -Verb RunAs` 不能与 `-RedirectStandardOutput` 同用（参数集冲突）。
- 注入的 DLL 每次用**唯一文件名**、共享环用**唯一名字**，否则多实例互相覆盖 → sink 收到 0 帧。
- 注入器 JSON 是 ANSI/UTF-16 混写，需按 `Encoding.Unicode` 再解一次。
- DLL 必须放在 `C:\Windows\System32\...` 下，`C:\Windows\Temp` 会被 audiodg 拒绝（`error=5`）。
- `Get-Process` 在沙箱 Low IL 下只看到自己；提权子进程能看到全部。
- C 源码改动用 edit 工具或**单行字面量** `.Replace()`；多行 here-string 在 CRLF 文件上不命中。

---

## 7. 现存的可用资产（路径）

| 资产 | 路径 | 状态 |
|---|---|---|
| 外部扫描/抓取器 | `sal\tap\sal_eac3tap*.c(.exe)`、`sal_ddptap_w4.c`、`sal_ddptap_w6/w7/w9/w10.c` | 保留源码，可复现 |
| guard 记录器 | `sal\tap\sal_watch3.c` → `sal_watch3.dll` | 顺序 100%，慎用 |
| 调用栈探针 | `sal\tap\sal_watch4.c` | 定位写函数 |
| WASAPI 钩子 | `sal\tap\sal_wcl.c` → `sal_wcl.dll` | 机制可用，数据是 PCM |
| SalEc3Grab(GUI) | `sal\grab\` | 第一版（guard 版/被动版都有） |
| 参考录音 | `C:\Users\fanhu\Music\*.ec3` | 逐帧打分基准 |
| 驱动的 IEC 路线 | `D:\sp\spatial-audio-lab\tools\Ec3Recorder` + `SpatialAudioLab.CLI ec3-pipe` | **唯一稳定干净** |

---

## 8. 后续可研究方向（按可行性排序）

1. **`IAudioRenderClient::ReleaseBuffer` 的“更下层”**：确认 Apple/Vivid 是否在 `audiodg` 侧把 PCM 封装成 MAT；若是，应在 `audiodg` 的 `IAudioRenderClient`（引擎侧）而不是播放器侧挂钩。
2. **硬件/驱动路线**：虚拟驱动（已装、只差测试签名）+ `Ec3Recorder`，或 WDK 的 `WaveRT` 直通端点；这是目前唯一被验证“零丢帧、顺序正确、不影响播放”的方案。
3. **单页写监视但零开销**：用 `PAGE_GUARD` 只挂在**每 128 ms 的第一个批页**（按统计预测），或在 `memcpy` 的入口做一次“只读判断 + 一次性快照”，避免每写都进 VEH。
4. **ETW/WPR**：`Microsoft-Windows-Audio` provider 是否有 bitstream 事件（未测）。
5. **屏幕/HDMI 侧抓取**：若显示器支持 Atmos 且走 bitstream，可用支持 IEC61937 采集的 HDMI 采集卡（硬件路线，绕开软件）。

---

## 9. 复现步骤（最小）

```powershell
# 1) 找到正在播放的宿主进程（Apple: AMPLibraryAgent；Vivid: VividPlayer.App）
Get-Process AMPLibraryAgent,AppleMusic,VividPlayer.App -EA SilentlyContinue | Select Id,ProcessName,CPU

# 2) 外部扫描（只读，确认码流存在）
sal\tap\sal_eac3tap.exe --pid <pid> --find --seconds 4

# 3) 单环地址序抓取（顺序最好）
sal\tap\sal_eac3tap.exe --pid <pid> --seconds 20 --out cap.ec3

# 4) 校验
ffprobe.exe -hide_banner cap.ec3
Eac3Pipe714.exe cap.ec3 --null-out --seconds 20
```

> 注意：步骤 2/3 都会让目标短暂挂起；不要连续跑超过 20~30 s，也不要对同一进程高频重复。
