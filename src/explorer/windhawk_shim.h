// The slice of the Windhawk runtime API that the stylers use (File Explorer and
// Start menu), implemented over ProWindows: settings come from <name>-styler.ini,
// logging goes to <name>-styler.log, and the inline hooks are MinHook's.
//
// One shim, two DLLs: the styler's name is fixed at compile time with
// /DPW_STYLER=L"explorer" or L"startmenu". The Start menu build also defines
// PW_STYLER_PACKAGED (its hosts are AppContainer processes, see
// windhawk_shim.cpp). This header is included by styler.cpp ahead of
// everything else, the way Windhawk prepends its own API header to a mod.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <initializer_list>

#ifndef PW_STYLER
#define PW_STYLER L"explorer"
#endif
#define WH_MOD_ID L"prowindows-" PW_STYLER L"-styler"

// ---------------------------------------------------------------- toolchain gaps
// The mod is written for clang and a recent SDK; ProWindows builds with MSVC
// against the 19041 SDK. These are the small differences.
#include <dwmapi.h>
#include <intrin.h>

#define __builtin_return_address(level) _ReturnAddress()

#ifndef NTDDI_WIN10_NI  // dwmapi.h from before Windows 11 22H2
constexpr DWORD DWMWA_USE_HOSTBACKDROPBRUSH = 17;
constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
enum DWM_SYSTEMBACKDROP_TYPE {
    DWMSBT_AUTO = 0,
    DWMSBT_NONE = 1,
    DWMSBT_MAINWINDOW = 2,
    DWMSBT_TRANSIENTWINDOW = 3,
    DWMSBT_TABBEDWINDOW = 4,
};
#endif

// ---------------------------------------------------------------- logging
// Silent unless debug = true is in config.ini (ProWindows copies it into the
// ini as `debug`). printf-style, wide: %s is a wide string, %S a narrow one.
void Wh_Log(PCWSTR format, ...);

// ---------------------------------------------------------------- settings
// The name is a printf format, as in Windhawk: "controlStyles[%d].target".
// A missing key reads as "" / 0, which is how the mod finds the end of an array.
PCWSTR Wh_GetStringSetting(PCWSTR valueName, ...);
void Wh_FreeStringSetting(PCWSTR string);
int Wh_GetIntSetting(PCWSTR valueName, ...);

// ---------------------------------------------------------------- storage
BOOL Wh_GetModStoragePath(PWSTR pathBuffer, UINT bufferChars);

// Values the mod keeps between runs (Windhawk's per-mod storage). One small
// file per key in the storage folder, so two host processes writing different
// keys never rewrite each other's. GetBinaryValue returns the stored size and
// copies at most bufferSize bytes; a missing key reads as 0 bytes / the default.
int Wh_GetIntValue(PCWSTR valueName, int defaultValue);
BOOL Wh_SetIntValue(PCWSTR valueName, int value);
size_t Wh_GetBinaryValue(PCWSTR valueName, void* buffer, size_t bufferSize);
BOOL Wh_SetBinaryValue(PCWSTR valueName, const void* buffer, size_t bufferSize);

// ---------------------------------------------------------------- download
// Synchronous (WinHTTP). The mod only calls it from threads of its own.
struct WH_GET_URL_CONTENT_OPTIONS {
    size_t optionsSize;
    PCWSTR targetFilePath;  // download into this file instead of memory
};

struct WH_URL_CONTENT {
    const char* data;
    size_t length;
    int statusCode;
};

const WH_URL_CONTENT* Wh_GetUrlContent(PCWSTR url,
                                       const WH_GET_URL_CONTENT_OPTIONS* options);
void Wh_FreeUrlContent(const WH_URL_CONTENT* content);

// ---------------------------------------------------------------- hooks
BOOL Wh_ApplyHookOperations();

bool WhSetFunctionHook(void* target, void* hook, void** original);

// The mod's own spelling; Windhawk's returns BOOL.
inline BOOL Wh_SetFunctionHook(void* target, void* hook, void** original) {
    return WhSetFunctionHook(target, hook, original) ? TRUE : FALSE;
}

namespace WindhawkUtils {

template <typename T>
inline BOOL SetFunctionHook(T target, T hook, T* original) {
    return WhSetFunctionHook(reinterpret_cast<void*>(target),
                             reinterpret_cast<void*>(hook),
                             reinterpret_cast<void**>(original));
}

// Hooking by symbol name needs the module's PDB; not supported here. The mod
// only uses it for explorerFrameContainerHeight, and carries on without.
struct SYMBOL_HOOK {
    template <typename T>
    SYMBOL_HOOK(std::initializer_list<PCWSTR> names, T* original, T hook)
        : name(names.size() ? *names.begin() : nullptr),
          pOriginalFunction(reinterpret_cast<void**>(original)),
          hookFunction(reinterpret_cast<void*>(hook)) {}

    PCWSTR name;
    void** pOriginalFunction;
    void* hookFunction;
};

inline bool HookSymbols(HMODULE, const SYMBOL_HOOK*, size_t) {
    Wh_Log(L"HookSymbols is not supported in ProWindows");
    return false;
}

}  // namespace WindhawkUtils

using WindhawkUtils::HookSymbols;

// ---------------------------------------------------------------- mod entry points
// Defined by styler.cpp; run by the lifecycle code in windhawk_shim.cpp.
BOOL Wh_ModInit();
void Wh_ModAfterInit();
void Wh_ModUninit();
void Wh_ModSettingsChanged();
