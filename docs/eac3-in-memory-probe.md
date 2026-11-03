# 在普通 Windows 模式下从内存读取 E-AC-3 码流：策略与问题汇总

> 目的：把"在用户态（audiodg.exe 内）抓取 E-AC-3 / IEC 61937 码流"这条探索路上用过的策略、踩过的坑、以及当前状态整理出来，供后续研究直接接着干。
>
> 关联代码：`tools/Eac3Hook/SalEac3Probe.c`（注入 audiodg 的探测 DLL）、`tools/Eac3Hook/SalEac3Injector.exe`（注入器）、`build.ps1`（clang + MinHook 编译）。

---

## 1. 背景与问题

要在 Windows **空间音频（Spatial Audio）** 通路上分析杜比 E-AC-3，第一步是搞清楚：E-AC-3 的 IEC 61937 burst 到底从哪条路径流过，又停在哪块内存里可见。

在普通（非内核调试）Windows 下，音频引擎跑在 `audiodg.exe`（Audio Device Graph，LocalService 账户）。一个用户态进程想看到它的 payload，只有两条可能通路：

- **KS IOCTL 写路径**：音频引擎把数据作为 `KSSTREAM_HEADER` 通过 `DeviceIoControl` / `NtDeviceIoControlFile` 等交给 KS 引脚（pin）。
- **WaveRT 共享循环缓冲**：Vista+ 的 WaveRT 端口驱动模型里，驱动与音频引擎**共享一块映射内存**（cyclic buffer），数据不经 IOCTL 传递，只存在于映射内存中。

本探测器的设计就是同时覆盖这两条路：hook 四个写 API + 解 KS header（策略一），再 hook 三个映射 API 并扫描映射内存（策略二）。

---

## 2. 关键常量

- **IEC 61937 同步字（burst 起始 4 字节，按内存字节序）**：`72 F8 1F 4E`。注意 burst 在内存里是连续的、且 burst 总长 **6144 字节**，同步字只在 burst 开头出现，burst 之间用 `00` 填充。
- **`IOCTL_KS_WRITE_STREAM`**（CTL_CODE(FILE_DEVICE_KS=0x2F, 4, METHOD_NEITHER, FILE_WRITE_ACCESS)）：`0x002F8013`。
- 扫描窗口、上限等：`SCAN_WINDOW=64KB`、`MAX_MAPPING_BYTES=64MB`、`MAPPING_SCAN_LIMIT=30` 轮。

---

## 3. 探测工具结构

- 用 **MinHook** 拦截目标 API；在音频线程里只做"复制最多 32 字节进无锁 ring"（`Interlocked*` 操作），绝不等日志、绝不改数据、绝不阻塞调用者。
- 独立的 `DrainLoop` 线程把 ring 落盘到 `C:\Windows\Temp\sal_eac3_probe.log`（`_wfsopen(..., L"wb", _SH_DENYNO)` 以便边跑边 tail）。
- DLL 生命周期 **120 秒**（deadline），到点自然退出；日志末尾打印 `mappings=<n> hits=<n> dropped=<n>`。
- 日志每行：`CallName code=0xXXXXXXXX len=N sync=S tid=T <32字节hex>`。

`CallName` 取值：

| call | 含义 |
|------|------|
| 1 | DeviceIoControl |
| 2 | NtDeviceIoControlFile |
| 3 | WriteFile |
| 4 | NtWriteFile |
| 5 | KsWriteStream（header->Data 指向的帧） |
| 6 | KsWriteFault（解 header 抛异常） |
| 7 | KsHeader（KSSTREAM_HEADER 前 32 字节） |
| 8 | MapSync（命中映射扫描；code=映射来源：0=VirtualQuery 发现，1/2/3=各映射 API） |
| 9 | MapOffset（命中在映射内的偏移；code=偏移值） |
| 10 | Resident（注入前已存在区域的清单；code 与 len 均为区域大小） |

---

## 4. 策略一：拦截 KS IOCTL 写路径

拦截 `DeviceIoControl` / `NtDeviceIoControlFile` / `WriteFile` / `NtWriteFile`，当 `code == 0x002F8013` 时走 `RecordKsWrite`：

- 复制 `KSSTREAM_HEADER` 前 32 字节（记 `KsHeader`）。
- 解 header 的 `Data`（buffer 指针）与 `DataUsed`（已填字节偏移），算出实际帧长 `used = DataUsed - Data`，复制该帧（记 `KsWriteStream`）。
- `Size/FrameExtent/Data/DataUsed/...` 布局按 `ks.h` 的 64 位结构对齐。

**实测线索**：在旧日志里，`0x002F8013` 大约每 **10 ms** 从 audiodg 内部某线程稳定出现，且 `len=0`。这是 `METHOD_NEITHER`：IO 管理器把用户指针原样保留在 `Type3InputBuffer`，**长度字段为 0 但指针 `in` 仍指向有效的 `KSSTREAM_HEADER`**。因此解 header 时要用 `in` 指针本身，而不是依赖 `inSize`。

> 这一条强烈暗示：当前设备的写引脚走的是 **KSSTREAM_HEADER 路径，而非 WaveRT 直写**——payload 很可能就挂在 `header->Data` 上，直接解出来即可，不一定需要方案二。

---

## 5. 策略二：WaveRT 映射内存扫描（方案 A）

**动机**：若设备用的是 WaveRT，数据只在共享循环缓冲里，IOCTL 上看不到。

- hook `MapViewOfFile`(source=1) / `MapViewOfFileEx`(2) / `NtMapViewOfSection`(3)，调用成功后把 `base/size/source` 登记进 `mappings[]`（base 去重，重启播放会重建同一缓冲）。
- `NtMapViewOfSection` 只登记映射进自身进程地址空间的 view（`IsCurrentProcess`）。
- `ReasonableSize` 处理 `dwNumberOfBytesToMap=0` / `ViewSize=0`：退化为 `VirtualQuery` 区域大小，夹到 64MB。

**关键补漏——已存在的缓冲**：WaveRT 缓冲在音频流启动时（往往早于注入）就已映射，只靠 hook 永远看不到。因此加了 `DiscoverResidentRegions()`：用 `VirtualQuery` 走一遍地址空间，只挑 `MEM_COMMIT` + 可写（`PAGE_READWRITE/WRITECOPY/EXECUTE_*`） + `>=16KB` 的区域作为 source=0 候选，并留最多 32 条 `Resident` 清单行作证据。

**扫描细节（踩坑后修正）**：原先用 4KB 网格、每点只看 32 字节，结果基本必然漏判——因为 burst 长达 6144 字节，网格点大多落在 burst 中间而非开头。改为 `FindSync()` **字节级**扫描每个映射前 64KB（`memchr` 找 `0x72` 再验后 3 字节）。`ScanMappings()` 每 **1 秒**跑一次，每 **5 秒**刷新一次 resident 清单；每个映射最多扫 30 轮（约 30 秒）无命中即放弃，命中后打 `MapSync` + `MapOffset` 并标记不再重扫。

---

## 6. 遇到的问题与解决

1. **PowerShell 里 `$pid` 是只读自动变量**，无法赋值（它永远是当前 powershell 的 PID）。—— 改用别的变量名，例如 `$ap = (Get-Process audiodg).Id`。否则注入器会拿到 powershell 自己的 PID 并报 `refusing to inject into powershell.exe`。

2. **旧 DLL 仍驻留在 audiodg，导致"注入成功但新代码不跑"**。对同一条 DLL 路径再次 `LoadLibraryW`，Windows 返回已加载模块，不会重新映射，也不会再进 `DllMain`。结果：磁盘上的新 DLL 被完全忽略，日志还是旧的那份。
   - 判据：日志文件 `LastWriteTime` 不变、首行没有新的版本标记。
   - 解决：**先 `Stop-Process -Name audiodg -Force`**（音频服务会在下次出声时自动重建它），再注入。注入器现在会先枚举目标模块，若 `SalEac3Probe.dll` 已在则 `return 9` 明确报错，不再假装成功。

3. **非管理员无法注入**。audiodg 是 LocalService，注入需 `SeDebugPrivilege`，`OpenProcess` 失败返回 `exit=2`（"SeDebugPrivilege unavailable; run as administrator"）。—— 必须在**提权窗口**（Win+X → 终端(管理员)，或 `Start-Process powershell -Verb RunAs`）里跑注入器。

4. **`LoadLibraryW inside audiodg failed`**：注入器把 DLL 路径字符串写进 audiodg，由它内部 `LoadLibraryW` 解析。相对路径（如 `.\bin\SalEac3Probe.dll`）是**相对于 audiodg 自己的当前目录**解析的，而非注入器的目录，所以找不到文件。
   - 解决：注入器现在先 `GetFullPathNameW` 转绝对路径；更稳妥的做法是把 DLL 拷到 `C:\Windows\Temp`（LocalService 必能读，也避开 D: 盘可能的 ACL 限制），注入那份副本。

5. **架构匹配**：注入的 DLL 必须是 **x64**（audiodg 是 64 位进程）。clang 默认产 64 位，已用 PE `Machine=0x8664` 确认。

6. **探测自身的日志写入被当成 payload 记录**：`_wfsopen/fprintf` 在 audiodg 内走 `WriteFile`，又被我们的 `WriteFile` hook 记录，于是日志里出现 `69636549...` 这种其实是 ASCII `"iceIoControl code=0x002F8013 len..."` 的"假 payload"。
   - 解决：`Record()` 里对 `call==3/4` 且 `GetCurrentThreadId()==logThread`（DrainLoop 启动时登记的日志线程 tid）直接丢弃。

7. **KS write 的 `len=0` 不是"没有数据"**：见 §4，`METHOD_NEITHER` 下长度字段为 0 但 `in` 指针仍有效，必须解 header。

---

## 7. 当前状态

截至整理时，**尚未拿到 `sync=1` 的真正命中**——因为注入一直没真正在 audiodg 里跑起来（先后卡在 `$pid` 错误、旧 DLL 驻留、非管理员、相对路径失败）。上述 1–6 都已修复，工具已重新编译并确认：
- `SalEac3Probe.dll` 内含 `MapOffset`/`MapSync`/`Resident` 标签、首行版本标记 `sal-eac3-probe v3`；
- `SalEac3Injector.exe` 增加了重复注入检测与绝对路径转换。

下一步就是按 §8 用"绝对路径 + 管理员 + 先播放再注入"跑通，看 `KsWriteStream`/`KsHeader` 还是 `MapSync`/`MapOffset` 先命中。

---

## 8. 复跑速查（管理员 PowerShell）

```powershell
# 1) 提权窗口里：拷 DLL 到可读取位置并注入
Start-Process powershell -Verb RunAs -ArgumentList '-NoExit','-Command',`
  'Copy-Item D:\sp\spatial-audio-lab\tools\Eac3Hook\bin\SalEac3Probe.dll C:\Windows\Temp\SalEac3Probe.dll -Force; `
   $ap=(Get-Process audiodg).Id; `
   D:\sp\spatial-audio-lab\tools\Eac3Hook\bin\SalEac3Injector.exe $ap C:\Windows\Temp\SalEac3Probe.dll'

# 2) 先让 Apple Music 在放 E-AC-3，注入后继续播 10~20 秒，普通窗口检查
(Get-Content C:\Windows\Temp\sal_eac3_probe.log -TotalCount 1)   # 必须 sal-eac3-probe v3 pid=<当前audiodg>
(Get-Item    C:\Windows\Temp\sal_eac3_probe.log).LastWriteTime    # 必须刚更新
Select-String -Path C:\Windows\Temp\sal_eac3_probe.log -Pattern 'sync=1|MapSync|MapOffset|KsHeader' | Select-Object -First 20
Get-Content  C:\Windows\Temp\sal_eac3_probe.log -Tail 2           # 末行 mappings=... hits=... dropped=...
```

三个判据都过，才说明新版本真的在跑；否则别急着解读日志内容。

---

## 9. 日志判读要点

- 看到 `KsWriteStream ... sync=1`：IOCTL 路径即 payload 来源，后续可直接录 `header->Data` 指向的帧。
- 看到 `MapSync code=0x00000003`：命中来自 `NtMapViewOfSection`（注入后新建的映射，最可能是 WaveRT 缓冲）；`code=0x00000000` 表示是注入前通过 `VirtualQuery` 发现的区域。
- `MapOffset` 的 `code` 是 burst 在缓冲内的偏移，后面 32 字节应以 `72 F8 1F 4E` 开头，可作为反复抓取的固定定位点。
- 注意区分"真 E-AC-3（含 JOC）"与"普通 PCM / Dolby Digital (AC-3)"：同步字 61937 的 burst 头部后续还有子流 ID（`0x01` = AC-3，`0x15` = E-AC-3 / DD+，`0x16` = E-AC-3 JOC），需按 IEC 61937-2 解析 `data-type`，别只认同步字就下结论。

## 10. 与既有架构的关系（勿混）

本探测独立于 Studio 的 `NativeSpatialPipe` 通路：后者经共享内存环 `Local\NSP_PCM714` 做空间音频采集；E-AC-3 直通有自己独立的环名（见代码库既有约定）。本工具不读写那些环，只做 audiodg 内的"在哪条路、哪块内存能看到 61937 burst"的取证。
