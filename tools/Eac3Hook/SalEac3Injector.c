// Injects SalEac3Probe.dll into audiodg.exe.
//
// audiodg runs as LocalService, so opening it needs SeDebugPrivilege. The injector
// only does LoadLibraryW remotely; everything else happens inside the probe DLL.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <tlhelp32.h>

static BOOL EnableDebugPrivilege(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return FALSE;
    }
    LUID luid;
    BOOL ok = LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid);
    if (ok) {
        TOKEN_PRIVILEGES privileges;
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), NULL, NULL) &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

// A probe that is still loaded keeps running its old code and never enters
// DllMain again, so a rebuilt dll on disk silently reads as "nothing changed".
static BOOL IsProbeLoaded(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return FALSE;   // cannot tell; let the load attempt speak for itself
    }
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    BOOL found = FALSE;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!_wcsicmp(entry.szModule, L"SalEac3Probe.dll")) {
                found = TRUE;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: SalEac3Injector <audiodg pid> <probe.dll>\n");
        fwprintf(stderr, L"  log lands in C:\\Windows\\Temp\\sal_eac3_probe.log\n");
        return 1;
    }

    DWORD pid = wcstoul(argv[1], NULL, 10);
    if (!pid) {
        fwprintf(stderr, L"bad pid\n");
        return 1;
    }
    if (GetFileAttributesW(argv[2]) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"probe dll not found: %ls\n", argv[2]);
        return 1;
    }

    // audiodg resolves the path against its own cwd, not the injector's, so a
    // relative name passed to LoadLibraryW there simply does not exist. Make it
    // absolute here while we still have the right cwd.
    wchar_t dllPath[MAX_PATH];
    if (!GetFullPathNameW(argv[2], MAX_PATH, dllPath, NULL)) {
        fwprintf(stderr, L"GetFullPathName failed: %lu\n", GetLastError());
        return 1;
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"probe dll not found: %ls\n", dllPath);
        return 1;
    }

    const DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                         PROCESS_DUP_HANDLE;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process && GetLastError() == ERROR_ACCESS_DENIED) {
        if (!EnableDebugPrivilege()) {
            fwprintf(stderr, L"SeDebugPrivilege unavailable; run as administrator\n");
            return 2;
        }
        process = OpenProcess(access, FALSE, pid);
    }
    if (!process) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 3;
    }

    // Refuse to inject into anything but the audio device graph.
    wchar_t image[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, image, &size)) {
        fwprintf(stderr, L"QueryFullProcessImageName failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 4;
    }
    const wchar_t *name = wcsrchr(image, L'\\');
    name = name ? name + 1 : image;
    if (_wcsicmp(name, L"audiodg.exe")) {
        fwprintf(stderr, L"refusing to inject into %ls\n", name);
        CloseHandle(process);
        return 5;
    }

    if (IsProbeLoaded(pid)) {
        fwprintf(stderr, L"SalEac3Probe.dll is already loaded in audiodg; "
                         L"stop audiodg first so the new build gets a DllMain\n");
        CloseHandle(process);
        return 9;
    }

    SIZE_T byteCount = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, NULL, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dllPath, byteCount, NULL)) {
        fwprintf(stderr, L"writing the dll path failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 6;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    HANDLE thread = CreateRemoteThread(process,
        NULL, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(kernel, "LoadLibraryW"), remote, 0, NULL);
    if (!thread) {
        fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 7;
    }

    WaitForSingleObject(thread, 10000);
    DWORD load = 0;
    GetExitCodeThread(thread, &load);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (!load) {
        fwprintf(stderr, L"LoadLibraryW inside audiodg failed\n");
        return 8;
    }
    wprintf(L"probe loaded at 0x%08lX, log: C:\\Windows\\Temp\\sal_eac3_probe.log\n", load);
    return 0;
}
