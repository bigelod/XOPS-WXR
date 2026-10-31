// MinHook - download real files from https://github.com/TsudaKageyu/minhook
// Place MinHook.h here. This stub is enough to compile after you replace it.
#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum MH_STATUS {
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
} MH_STATUS;
MH_STATUS WINAPI MH_Initialize(void);
MH_STATUS WINAPI MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID *ppOriginal);
MH_STATUS WINAPI MH_EnableHook(LPVOID pTarget);
MH_STATUS WINAPI MH_DisableHook(LPVOID pTarget);
#define MH_ALL_HOOKS NULL
#ifdef __cplusplus
}
#endif
