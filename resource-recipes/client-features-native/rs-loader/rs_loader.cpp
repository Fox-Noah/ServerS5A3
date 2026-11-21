// RS.dll proxy + mods plugin loader (x86 / MSVC / static CRT)
//
// DNF.exe imports RS.dll. We take that slot:
//   1. forward PutRSInfo / PutRSInfo2 / ClearRSInfo / ClearRSInfo2
//      to the untouched original DLL (RS_orig.dll), by raw jmp so the
//      original calling convention and argument count are preserved;
//   2. on the first export call, scan <exe dir>\mods\*.dll and load every
//      DLL found; if the DLL exports DnfModInit (cdecl, no args) we call it.
//
// Nothing is loaded from DllMain (loader lock).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#pragma comment(lib, "kernel32.lib")

extern "C" {
    FARPROC g_pPutRSInfo = NULL;
    FARPROC g_pClearRSInfo = NULL;
}

static HMODULE g_hOrig = NULL;
static volatile LONG g_initState = 0;   // 0 = pending, 1 = running, 2 = done

static wchar_t g_szLog[MAX_PATH];
static wchar_t g_szOrig[MAX_PATH];
static wchar_t g_szMods[MAX_PATH];

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static void LogA(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    FILE* f = NULL;
    if (_wfopen_s(&f, g_szLog, L"a") == 0 && f) {
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    va_end(ap);
}

static FARPROC ResolveExport(HMODULE mod, const char* name, WORD ordinal)
{
    FARPROC p = GetProcAddress(mod, name);
    if (!p) p = GetProcAddress(mod, MAKEINTRESOURCEA(ordinal));
    return p;
}

// Fallback only when RS_orig.dll cannot be loaded. A naked `ret` keeps the
// stack untouched regardless of the original calling convention.
extern "C" __declspec(naked) void NoopTail(void)
{
    __asm { ret }
}

// ---------------------------------------------------------------------------
// one-time initialisation
// ---------------------------------------------------------------------------
static void DoInit(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LogA("GetModuleFileNameW failed err=%lu", GetLastError());
        return;
    }
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    else exe[0] = 0;

    swprintf_s(g_szLog,  MAX_PATH, L"%srs_loader.log", exe);
    swprintf_s(g_szOrig, MAX_PATH, L"%sRS_orig.dll", exe);
    swprintf_s(g_szMods, MAX_PATH, L"%smods\\*.dll", exe);

    LogA("=== rs_loader init ===");
    LogA("exe dir: %ls", exe);

    // --- original RS.dll ---------------------------------------------------
    g_hOrig = LoadLibraryW(g_szOrig);
    if (!g_hOrig) {
        LogA("RS_orig.dll LOAD FAILED err=%lu", GetLastError());
    } else {
        g_pPutRSInfo   = ResolveExport(g_hOrig, "PutRSInfo",   3);
        g_pClearRSInfo = ResolveExport(g_hOrig, "ClearRSInfo", 1);
        LogA("RS_orig resolved: PutRSInfo=%p ClearRSInfo=%p",
             (void*)g_pPutRSInfo, (void*)g_pClearRSInfo);
    }
    if (!g_pPutRSInfo)   g_pPutRSInfo   = (FARPROC)&NoopTail;
    if (!g_pClearRSInfo) g_pClearRSInfo = (FARPROC)&NoopTail;

    // --- mods --------------------------------------------------------------
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(g_szMods, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        LogA("no mods\\*.dll (err=%lu)", GetLastError());
        LogA("=== init done ===");
        return;
    }

    int loaded = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        wchar_t full[MAX_PATH];
        swprintf_s(full, MAX_PATH, L"%smods\\%s", exe, fd.cFileName);

        HMODULE m = LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            LogA("mod LOAD FAILED: %ls (err=%lu)", fd.cFileName, GetLastError());
            continue;
        }

        FARPROC init = GetProcAddress(m, "DnfModInit");
        if (init) {
            ((void (__cdecl *)(void))init)();
            LogA("mod LOADED: %ls (DnfModInit called)", fd.cFileName);
        } else {
            LogA("mod LOADED: %ls (no DnfModInit)", fd.cFileName);
        }
        ++loaded;
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    LogA("mods loaded: %d", loaded);
    LogA("=== init done ===");
}

extern "C" void EnsureInitialized(void)
{
    LONG prev = InterlockedCompareExchange(&g_initState, 1, 0);
    if (prev == 0) {
        __try {
            DoInit();
        }
        __finally {
            InterlockedExchange(&g_initState, 2);
        }
    } else if (prev == 1) {
        while (g_initState == 1) Sleep(1);
    }
}

// ---------------------------------------------------------------------------
// exported stubs. `call` only adds/removes a return address; the original
// arguments stay on the stack. `jmp [ptr]` tail-calls the real function with
// an untouched stack frame, so any calling convention / argument count works.
// ---------------------------------------------------------------------------
extern "C" __declspec(naked) void PutRSInfo(void)
{
    __asm {
        call EnsureInitialized
        jmp  dword ptr [g_pPutRSInfo]
    }
}

extern "C" __declspec(naked) void PutRSInfo2(void)
{
    __asm {
        call EnsureInitialized
        jmp  dword ptr [g_pPutRSInfo]
    }
}

extern "C" __declspec(naked) void ClearRSInfo(void)
{
    __asm {
        call EnsureInitialized
        jmp  dword ptr [g_pClearRSInfo]
    }
}

extern "C" __declspec(naked) void ClearRSInfo2(void)
{
    __asm {
        call EnsureInitialized
        jmp  dword ptr [g_pClearRSInfo]
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (g_hOrig) {
            FreeLibrary(g_hOrig);
            g_hOrig = NULL;
        }
    }
    return TRUE;
}
