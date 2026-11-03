// E-AC-3 passthrough probe for audiodg.exe.
//
// Injected into audiodg, hooks the four system calls a user-mode audio engine can
// use to hand a bitstream to a KS pin (DeviceIoControl / NtDeviceIoControlFile /
// WriteFile / NtWriteFile) and records what passes through. It never modifies the
// data, never blocks the caller and keeps nothing resident in the audio path: the
// hook copies at most 32 bytes into a lock-free ring and returns immediately.
//
// Purpose: find out which call carries IEC 61937 bursts on this machine before any
// real capture code is written around it.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <share.h>
#include <MinHook.h>

#define LOG_PATH L"C:\\Windows\\Temp\\sal_eac3_probe.log"
#define RING_SLOTS 8192
#define SAMPLE_BYTES 32

// CTL_CODE(FILE_DEVICE_KS, 4, METHOD_NEITHER, FILE_WRITE_ACCESS). METHOD_NEITHER
// means InputBufferLength is zero and the real payload hangs off the header.
#define KS_WRITE_STREAM 0x002F8013

// Only the first fields matter here; the layout matches ks.h for x64.
typedef struct {
    ULONG Size;
    ULONG FrameExtent;
    PVOID DataUsed;      // Data + bytes actually filled in
    PVOID Data;
    ULONG Status;
    LONGLONG PresentationTime;
    ULONG OptionsFlags;
    ULONG Reserved;
} KsStreamHeader;

// One captured call. Kept POD and small: it is written from the audio thread.
typedef struct {
    uint32_t call;          // 1=DeviceIoControl 2=NtDeviceIoControlFile 3=WriteFile 4=NtWriteFile
    uint32_t code;          // IOCTL code, or 0 for the write paths
    uint32_t length;        // reported buffer length
    uint32_t hasSync;       // IEC 61937 sync word found in the sample
    uint32_t threadId;
    uint8_t sample[SAMPLE_BYTES];
} ProbeEntry;

static ProbeEntry ring[RING_SLOTS];
static volatile LONG ringWrite, ringRead, dropped;
static volatile LONG ksRecords;

// A running stream produces hundreds of these per second; a few thousand is
// plenty to read the format off, and it keeps the log openable.
#define KS_RECORD_LIMIT 3000

typedef BOOL (WINAPI *DeviceIoControlFn)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *WriteFileFn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef NTSTATUS (NTAPI *NtDeviceIoControlFileFn)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI *NtWriteFileFn)(HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID, ULONG, void*, void*);
typedef LPVOID (WINAPI *MapViewOfFileFn)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
typedef LPVOID (WINAPI *MapViewOfFileExFn)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID);
typedef NTSTATUS (NTAPI *NtMapViewOfSectionFn)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T,
    PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);

static DeviceIoControlFn originalDeviceIoControl;
static WriteFileFn originalWriteFile;
static NtDeviceIoControlFileFn originalNtDeviceIoControlFile;
static NtWriteFileFn originalNtWriteFile;
static MapViewOfFileFn originalMapViewOfFile;
static MapViewOfFileExFn originalMapViewOfFileEx;
static NtMapViewOfSectionFn originalNtMapViewOfSection;
static volatile LONG installed;
static volatile LONG logThread;
static HANDLE stopEvent;

// Bursts are byte streams, so the sync word is compared as raw bytes.
static int HasIec61937Sync(const uint8_t *data, uint32_t length) {
    uint32_t limit = length < SAMPLE_BYTES ? length : SAMPLE_BYTES;
    for (uint32_t i = 0; i + 3 < limit; ++i) {
        if (data[i] == 0x72 && data[i + 1] == 0xF8 && data[i + 2] == 0x1F && data[i + 3] == 0x4E) {
            return 1;
        }
    }
    return 0;
}

// Audio threads must never wait for the log, so this only copies into the ring.
static void Record(uint32_t call, uint32_t code, const void *data, uint32_t length) {
    // Writing the log is a WriteFile inside audiodg, so the probe would record
    // its own log lines as payload. Those came back as ASCII in an earlier run.
    if ((call == 3 || call == 4) &&
        (LONG)GetCurrentThreadId() == InterlockedCompareExchange(&logThread, 0, 0)) {
        return;
    }
    if ((call == 5 || call == 7) && InterlockedIncrement(&ksRecords) > KS_RECORD_LIMIT) {
        return;
    }
    LONG w = InterlockedCompareExchange(&ringWrite, 0, 0);
    LONG r = InterlockedCompareExchange(&ringRead, 0, 0);
    if ((uint32_t)(w - r) >= RING_SLOTS) {
        InterlockedIncrement(&dropped);
        return;
    }
    ProbeEntry *slot = &ring[(uint32_t)w % RING_SLOTS];
    slot->call = call;
    slot->code = code;
    slot->length = length;
    slot->threadId = GetCurrentThreadId();
    slot->hasSync = 0;
    memset(slot->sample, 0, SAMPLE_BYTES);
    if (data && length) {
        uint32_t copy = length < SAMPLE_BYTES ? length : SAMPLE_BYTES;
        __try {
            memcpy(slot->sample, data, copy);
            slot->hasSync = HasIec61937Sync(slot->sample, copy);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            slot->length = 0xFFFFFFFFu;
        }
    }
    InterlockedExchange(&ringWrite, w + 1);
}

// Defined after the hooks so the KS layout stays in one place.
static void RecordKsWrite(uint32_t call, ULONG code, void *headerPointer);

// WaveRT hands the audio engine a shared cyclic buffer instead of streaming it
// through IOCTL, so the payload only shows up in mapped memory. Every mapping
// audiodg creates is remembered and rescanned until a burst shows up.
//
// The buffer is usually mapped when the stream starts, which can be well before
// this probe is injected, so the first pass walks the address space and treats
// every plausible committed region as a candidate (source 0 = VirtualQuery).
#define MAX_MAPPINGS 256
#define SCAN_WINDOW (64u << 10)     // bytes examined per mapping and pass
#define MAX_MAPPING_BYTES (64u << 20)
#define MAPPING_SCAN_LIMIT 30       // passes before a mapping is given up on

typedef struct {
    void *base;
    SIZE_T size;
    uint32_t source;    // 0=VirtualQuery 1=MapViewOfFile 2=MapViewOfFileEx 3=NtMapViewOfSection
    uint32_t hits;
    uint32_t scans;
} Mapping;

static Mapping mappings[MAX_MAPPINGS];
static volatile LONG mappingCount;
static uint32_t residentRecords;

// Byte-accurate on purpose: a 4 KB grid mostly lands in the middle of a burst,
// because an E-AC-3 burst is 6144 bytes and the sync word sits at its start.
static int64_t FindSync(const unsigned char *data, SIZE_T length) {
    if (length > SCAN_WINDOW) {
        length = SCAN_WINDOW;
    }
    if (length < 4) {
        return -1;
    }
    const unsigned char *cursor = data;
    SIZE_T remaining = length;
    while (remaining >= 4) {
        const unsigned char *hit = (const unsigned char *)memchr(cursor, 0x72, remaining - 3);
        if (!hit) {
            break;
        }
        if (hit[1] == 0xF8 && hit[2] == 0x1F && hit[3] == 0x4E) {
            return (int64_t)(hit - data);
        }
        remaining -= (SIZE_T)(hit - cursor) + 1;
        cursor = hit + 1;
    }
    return -1;
}

// A caller that passes 0 for the length means "the whole section"; the region
// around the base address is the closest cheap answer.
static SIZE_T ReasonableSize(void *base, SIZE_T requested) {
    SIZE_T size = requested;
    if (!size) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(base, &info, sizeof(info)) == sizeof(info)) {
            size = info.RegionSize;
        }
    }
    if (size > MAX_MAPPING_BYTES) {
        size = MAX_MAPPING_BYTES;
    }
    return size;
}

static int IsCurrentProcess(HANDLE process) {
    if (!process || process == (HANDLE)-1) {
        return 1;
    }
    return GetProcessId(process) == GetCurrentProcessId();
}

static void RememberMapping(uint32_t source, void *base, SIZE_T size) {
    if (!base || !size) {
        return;
    }
    if (size > MAX_MAPPING_BYTES) {
        size = MAX_MAPPING_BYTES;
    }
    LONG count = InterlockedCompareExchange(&mappingCount, 0, 0);
    for (LONG i = 0; i < count && i < MAX_MAPPINGS; ++i) {
        if (mappings[i].base == base) {
            return;   // the same buffer shows up again when a stream restarts
        }
    }
    LONG index = InterlockedIncrement(&mappingCount) - 1;
    if (index >= MAX_MAPPINGS) {
        return;
    }
    Mapping *entry = &mappings[index];
    entry->base = base;
    entry->size = size;
    entry->source = source;
    entry->hits = 0;
    entry->scans = 0;
}

// WaveRT only ever writes into writable views, so read-only regions (images,
// data files) are skipped: that keeps the candidate list short enough to rescan.
static void DiscoverResidentRegions(void) {
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    unsigned char *cursor = NULL;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery(cursor, &info, sizeof(info)) == sizeof(info)) {
        unsigned char *next = (unsigned char *)info.BaseAddress + info.RegionSize;
        if (info.State == MEM_COMMIT && (info.Protect & writable) &&
            !(info.Protect & PAGE_GUARD) && info.RegionSize >= 16384) {
            RememberMapping(0, info.BaseAddress, info.RegionSize);
            if (residentRecords < 32) {
                // Inventory line: keeps only the size, so the log stays readable.
                Record(10, (uint32_t)info.RegionSize, NULL, (uint32_t)info.RegionSize);
                ++residentRecords;
            }
        }
        if (!next || next <= cursor) {
            break;
        }
        cursor = next;
    }
}

// Called from the log thread, never from the audio path.
static void ScanMappings(void) {
    LONG count = InterlockedCompareExchange(&mappingCount, 0, 0);
    if (count > MAX_MAPPINGS) {
        count = MAX_MAPPINGS;
    }
    for (LONG i = 0; i < count; ++i) {
        Mapping *entry = &mappings[i];
        if (entry->hits || entry->scans >= MAPPING_SCAN_LIMIT) {
            continue;
        }
        __try {
            int64_t offset = FindSync((const unsigned char *)entry->base, entry->size);
            if (offset >= 0) {
                const unsigned char *burst = (const unsigned char *)entry->base + offset;
                Record(9, (uint32_t)offset, burst, SAMPLE_BYTES);
                Record(8, entry->source, burst, SAMPLE_BYTES);
                entry->hits = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            entry->scans = 99;
        }
        ++entry->scans;
    }
}

static BOOL WINAPI HookDeviceIoControl(HANDLE device, DWORD code, LPVOID in, DWORD inSize,
    LPVOID out, DWORD outSize, LPDWORD returned, LPOVERLAPPED overlapped) {
    BOOL result = originalDeviceIoControl(device, code, in, inSize, out, outSize, returned, overlapped);
    // kernel32 forwards to ntdll; recording both here would double every line.
    if (code == KS_WRITE_STREAM) {
        RecordKsWrite(1, code, in);
    } else {
        Record(1, code, in, inSize);
    }
    return result;
}

static BOOL WINAPI HookWriteFile(HANDLE file, LPCVOID buffer, DWORD length,
    LPDWORD written, LPOVERLAPPED overlapped) {
    BOOL result = originalWriteFile(file, buffer, length, written, overlapped);
    Record(3, 0, buffer, length);
    return result;
}

static NTSTATUS NTAPI HookNtDeviceIoControlFile(HANDLE file, HANDLE event, PVOID apcRoutine,
    PVOID apcContext, PVOID statusBlock, ULONG code, PVOID in, ULONG inSize,
    PVOID out, ULONG outSize) {
    NTSTATUS result = originalNtDeviceIoControlFile(file, event, apcRoutine, apcContext,
        statusBlock, code, in, inSize, out, outSize);
    if (code == KS_WRITE_STREAM) {
        RecordKsWrite(2, code, in);
    } else {
        Record(2, code, in, inSize);
    }
    return result;
}

static NTSTATUS NTAPI HookNtWriteFile(HANDLE file, HANDLE event, PVOID apcRoutine,
    PVOID apcContext, PVOID statusBlock, PVOID buffer, ULONG length,
    void *byteOffset, void *key) {
    NTSTATUS result = originalNtWriteFile(file, event, apcRoutine, apcContext,
        statusBlock, buffer, length, byteOffset, key);
    Record(4, 0, buffer, length);
    return result;
}

// The mapping hooks only note the view. Scanning happens later on the log
// thread, so nothing here touches the mapped bytes.
static LPVOID WINAPI HookMapViewOfFile(HANDLE section, DWORD access, DWORD offsetHigh,
    DWORD offsetLow, SIZE_T bytes) {
    LPVOID base = originalMapViewOfFile(section, access, offsetHigh, offsetLow, bytes);
    if (base) {
        RememberMapping(1, base, ReasonableSize(base, bytes));
    }
    return base;
}

static LPVOID WINAPI HookMapViewOfFileEx(HANDLE section, DWORD access, DWORD offsetHigh,
    DWORD offsetLow, SIZE_T bytes, LPVOID desired) {
    LPVOID base = originalMapViewOfFileEx(section, access, offsetHigh, offsetLow, bytes, desired);
    if (base) {
        RememberMapping(2, base, ReasonableSize(base, bytes));
    }
    return base;
}

static NTSTATUS NTAPI HookNtMapViewOfSection(HANDLE section, HANDLE process, PVOID *baseAddress,
    ULONG_PTR zeroBits, SIZE_T commitSize, PLARGE_INTEGER offset, PSIZE_T viewSize,
    ULONG inherit, ULONG allocationType, ULONG protect) {
    NTSTATUS status = originalNtMapViewOfSection(section, process, baseAddress, zeroBits, commitSize,
        offset, viewSize, inherit, allocationType, protect);
    if (status >= 0 && baseAddress && *baseAddress && IsCurrentProcess(process)) {
        RememberMapping(3, *baseAddress, ReasonableSize(*baseAddress, viewSize ? *viewSize : 0));
    }
    return status;
}

// KS writes arrive as a KSSTREAM_HEADER; report its first four words as well as
// the frame it points at, so a wrong layout guess is visible instead of silent.
static void RecordKsWrite(uint32_t call, ULONG code, void *headerPointer) {
    if (!headerPointer) {
        Record(call, code, NULL, 0);
        return;
    }
    __try {
        Record(7, code, headerPointer, 32);
        const KsStreamHeader *header = (const KsStreamHeader *)headerPointer;
        if (header->Data && header->DataUsed) {
            ULONG used = (ULONG)((const unsigned char *)header->DataUsed -
                                 (const unsigned char *)header->Data);
            if (used > 0 && used <= 65536) {
                Record(5, code, header->Data, used);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Record(6, code, NULL, 0);
    }
}

static const char *CallName(uint32_t call) {
    switch (call) {
        case 1: return "DeviceIoControl";
        case 2: return "NtDeviceIoControlFile";
        case 3: return "WriteFile";
        case 4: return "NtWriteFile";
        case 5: return "KsWriteStream";
        case 6: return "KsWriteFault";
        case 7: return "KsHeader";
        case 8: return "MapSync";      // code=mapping source
        case 9: return "MapOffset";    // code=offset inside the mapping
        case 10: return "Resident";    // code and len=region size
        default: return "?";
    }
}

// Drains the ring to disk on its own thread so the audio path stays untouched.
static DWORD WINAPI DrainLoop(void *unused) {
    (void)unused;
    InterlockedExchange(&logThread, (LONG)GetCurrentThreadId());
    // _SH_DENYNO so the log can be tailed while the probe is still running;
    // an exclusive handle made the only useful diagnostic unreadable.
    FILE *log = _wfsopen(LOG_PATH, L"wb", _SH_DENYNO);
    if (!log) {
        return 0;
    }
    // The build tag makes it obvious when a stale dll was loaded instead of the
    // freshly built one: an old module never runs this code again.
    fprintf(log, "sal-eac3-probe v3 pid=%lu\n", GetCurrentProcessId());
    fflush(log);

    ULONGLONG deadline = GetTickCount64() + 120000;
    ULONGLONG nextScan = GetTickCount64();
    ULONGLONG nextDiscover = nextScan;
    for (;;) {
        // WaveRT buffers are found by rescanning, not by watching a call, so the
        // sweep has to run whether or not anything reached the ring.
        ULONGLONG now = GetTickCount64();
        if (now >= nextScan) {
            if (now >= nextDiscover) {
                DiscoverResidentRegions();
                nextDiscover = now + 5000;
            }
            ScanMappings();
            nextScan = now + 1000;
            fflush(log);
        }

        LONG r = InterlockedCompareExchange(&ringRead, 0, 0);
        LONG w = InterlockedCompareExchange(&ringWrite, 0, 0);
        if (r == w) {
            if (WaitForSingleObject(stopEvent, 200) == WAIT_OBJECT_0) {
                break;
            }
            if (GetTickCount64() > deadline) {
                break;
            }
            continue;
        }
        ProbeEntry *entry = &ring[(uint32_t)r % RING_SLOTS];
        fprintf(log, "%s code=0x%08X len=%u sync=%u tid=%u ",
            CallName(entry->call), entry->code, entry->length, entry->hasSync, entry->threadId);
        for (uint32_t i = 0; i < SAMPLE_BYTES; ++i) {
            fprintf(log, "%02X", entry->sample[i]);
        }
        fprintf(log, "\n");
        if ((r & 0x3F) == 0) {
            fflush(log);
        }
        InterlockedExchange(&ringRead, r + 1);
    }
    // Which classes of mapping carried a burst is the whole answer, so report it
    // even when no IOCTL traffic was seen at all.
    long hits = 0;
    LONG tracked = InterlockedCompareExchange(&mappingCount, 0, 0);
    if (tracked > MAX_MAPPINGS) {
        tracked = MAX_MAPPINGS;
    }
    for (LONG i = 0; i < tracked; ++i) {
        if (mappings[i].hits) {
            ++hits;
        }
    }
    fprintf(log, "mappings=%ld hits=%ld dropped=%ld\n",
        (long)tracked, hits, (long)InterlockedCompareExchange(&dropped, 0, 0));
    fflush(log);
    fclose(log);
    return 0;
}

static DWORD WINAPI Start(void *unused) {
    (void)unused;
    stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stopEvent) {
        return 0;
    }
    if (MH_Initialize() != MH_OK) {
        return 0;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    void *targetDeviceIoControl = (void *)GetProcAddress(kernel, "DeviceIoControl");
    void *targetWriteFile = (void *)GetProcAddress(kernel, "WriteFile");
    void *targetNtDeviceIo = (void *)GetProcAddress(ntdll, "NtDeviceIoControlFile");
    void *targetNtWrite = (void *)GetProcAddress(ntdll, "NtWriteFile");
    // kernel32 forwarders resolve straight to kernelbase, so one lookup covers both.
    void *targetMapView = (void *)GetProcAddress(kernel, "MapViewOfFile");
    void *targetMapViewEx = (void *)GetProcAddress(kernel, "MapViewOfFileEx");
    void *targetNtMapView = (void *)GetProcAddress(ntdll, "NtMapViewOfSection");

    if (targetDeviceIoControl) {
        MH_CreateHook(targetDeviceIoControl, HookDeviceIoControl, (void **)&originalDeviceIoControl);
    }
    if (targetWriteFile) {
        MH_CreateHook(targetWriteFile, HookWriteFile, (void **)&originalWriteFile);
    }
    if (targetNtDeviceIo) {
        MH_CreateHook(targetNtDeviceIo, HookNtDeviceIoControlFile, (void **)&originalNtDeviceIoControlFile);
    }
    if (targetNtWrite) {
        MH_CreateHook(targetNtWrite, HookNtWriteFile, (void **)&originalNtWriteFile);
    }
    if (targetMapView) {
        MH_CreateHook(targetMapView, HookMapViewOfFile, (void **)&originalMapViewOfFile);
    }
    if (targetMapViewEx) {
        MH_CreateHook(targetMapViewEx, HookMapViewOfFileEx, (void **)&originalMapViewOfFileEx);
    }
    if (targetNtMapView) {
        MH_CreateHook(targetNtMapView, HookNtMapViewOfSection, (void **)&originalNtMapViewOfSection);
    }
    MH_EnableHook(MH_ALL_HOOKS);

    InterlockedExchange(&installed, 1);
    DrainLoop(NULL);   // the injector unloads us on stop, so drain inline

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    InterlockedExchange(&installed, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // LoadLibrary from the injector must return fast, so hooking happens here.
        HANDLE thread = CreateThread(NULL, 0, Start, NULL, 0, NULL);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
