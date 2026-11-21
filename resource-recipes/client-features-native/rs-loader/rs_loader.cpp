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

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
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

// Resolve an address to module file + RVA for post-mortem symbol lookup.
static void LogAddress(const char* tag, const void* address)
{
    if (!address) { LogA("%s=null", tag); return; }
    HMODULE module = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module)) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(module, path, MAX_PATH);
        LogA("%s=%p module=%s rva=%08lX", tag, address, path,
             (unsigned long)((const unsigned char*)address - (const unsigned char*)module));
    } else {
        LogA("%s=%p module=?", tag, address);
    }
}

// Safe read: a corrupt EBP chain must not cause a second exception inside the
// observer. Returns false when the read is not possible.
static bool TryRead(void* address, void* output, size_t size)
{
    __try {
        memcpy(output, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Log-only vectored handler: records the first structured exception with
// registers, a bounded EBP walk and raw stack words, then lets the game
// handle it normally. No state is modified.
static volatile LONG g_crashLogged = 0;
static LONG WINAPI CrashObserver(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_crashLogged, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    LogA("CRASH code=%08lX addr=%p flags=%08lX extra=%p",
         record->ExceptionCode, record->ExceptionAddress,
         record->ExceptionFlags, (void*)record->ExceptionInformation[1]);
    LogAddress("CRASH module", record->ExceptionAddress);

    CONTEXT* context = info->ContextRecord;
    if (context) {
        LogA("CTX eax=%08lX ecx=%08lX edx=%08lX ebx=%08lX", context->Eax,
             context->Ecx, context->Edx, context->Ebx);
        LogA("CTX esp=%08lX ebp=%08lX esi=%08lX edi=%08lX eip=%08lX", context->Esp,
             context->Ebp, context->Esi, context->Edi, context->Eip);

        // Raw stack window: return addresses often sit near ESP.
        unsigned long* stack = reinterpret_cast<unsigned long*>(context->Esp);
        for (int i = 0; i < 16; ++i) {
            unsigned long value = 0;
            if (!TryRead(stack + i, &value, sizeof(value))) break;
            if (value) LogAddress("STACK", reinterpret_cast<const void*>(value));
        }

        // Frame-pointer walk: [ebp] = previous ebp, [ebp+4] = return address.
        unsigned long frame = context->Ebp;
        for (int depth = 0; depth < 12 && frame; ++depth) {
            unsigned long next = 0;
            if (!TryRead(reinterpret_cast<void*>(frame), &next, sizeof(next))) break;
            unsigned long ret = 0;
            TryRead(reinterpret_cast<void*>(frame + 4), &ret, sizeof(ret));
            if (ret) LogAddress("FRAME", reinterpret_cast<const void*>(ret));
            if (next <= frame) break;
            frame = next;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
    AddVectoredExceptionHandler(1, CrashObserver);
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
