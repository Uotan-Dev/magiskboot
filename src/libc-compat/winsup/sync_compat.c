// Windows XP/7 compatible synchronization primitives
// Provides implementations for WaitOnAddress/WakeByAddress
// Based on YY-Thunks: https://github.com/Chuyu-Team/YY-Thunks
// 
// 原理：使用 NtWaitForKeyedEvent/NtReleaseKeyedEvent (Windows XP+)
// 如果系统有原生 API 就用原生的，否则用兼容实现

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

// NT API 声明
typedef LONG NTSTATUS;
#define STATUS_SUCCESS_COMPAT ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT_COMPAT ((NTSTATUS)0x00000102L)

typedef NTSTATUS (NTAPI *PFN_NtWaitForKeyedEvent)(
    HANDLE KeyedEventHandle,
    PVOID Key,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout
);

typedef NTSTATUS (NTAPI *PFN_NtReleaseKeyedEvent)(
    HANDLE KeyedEventHandle,
    PVOID Key,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout
);

// 原生 API 函数指针
typedef BOOL (WINAPI *PFN_WaitOnAddress)(
    volatile VOID *Address,
    PVOID CompareAddress,
    SIZE_T AddressSize,
    DWORD dwMilliseconds
);

typedef VOID (WINAPI *PFN_WakeByAddressSingle)(PVOID Address);
typedef VOID (WINAPI *PFN_WakeByAddressAll)(PVOID Address);

static PFN_WaitOnAddress pWaitOnAddress = NULL;
static PFN_WakeByAddressSingle pWakeByAddressSingle = NULL;
static PFN_WakeByAddressAll pWakeByAddressAll = NULL;
static PFN_NtWaitForKeyedEvent pNtWaitForKeyedEvent = NULL;
static PFN_NtReleaseKeyedEvent pNtReleaseKeyedEvent = NULL;
static volatile LONG bInitialized = 0;

// 动态加载函数
static void InitSyncCompat(void) {
    if (InterlockedCompareExchange(&bInitialized, 1, 0) == 0) {
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        HMODULE hApiSet = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
        
        // 尝试加载原生 API (Windows 8+)
        if (hKernel32) {
            pWaitOnAddress = (PFN_WaitOnAddress)GetProcAddress(hKernel32, "WaitOnAddress");
            pWakeByAddressSingle = (PFN_WakeByAddressSingle)GetProcAddress(hKernel32, "WakeByAddressSingle");
            pWakeByAddressAll = (PFN_WakeByAddressAll)GetProcAddress(hKernel32, "WakeByAddressAll");
        }
        
        if (!pWaitOnAddress && hApiSet) {
            pWaitOnAddress = (PFN_WaitOnAddress)GetProcAddress(hApiSet, "WaitOnAddress");
            pWakeByAddressSingle = (PFN_WakeByAddressSingle)GetProcAddress(hApiSet, "WakeByAddressSingle");
            pWakeByAddressAll = (PFN_WakeByAddressAll)GetProcAddress(hApiSet, "WakeByAddressAll");
        }
        
        // 如果没有原生 API，加载 NT API 用于回退实现
        if (!pWaitOnAddress) {
            HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
            if (hNtdll) {
                pNtWaitForKeyedEvent = (PFN_NtWaitForKeyedEvent)GetProcAddress(hNtdll, "NtWaitForKeyedEvent");
                pNtReleaseKeyedEvent = (PFN_NtReleaseKeyedEvent)GetProcAddress(hNtdll, "NtReleaseKeyedEvent");
            }
        }
    }
}

// 导出的函数（使用与系统相同的调用约定和签名）
BOOL WINAPI WaitOnAddress(
    volatile VOID *Address,
    PVOID CompareAddress,
    SIZE_T AddressSize,
    DWORD dwMilliseconds
) {
    InitSyncCompat();
    
    // 如果系统支持原生 API，使用原生的
    if (pWaitOnAddress) {
        return pWaitOnAddress(Address, CompareAddress, AddressSize, dwMilliseconds);
    }
    
    // 回退实现
    // 参数检查
    if (AddressSize > 8 || AddressSize == 0 || ((AddressSize - 1) & AddressSize) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    
    // 检查值是否已经不同
    BOOL bSame = FALSE;
    switch (AddressSize) {
        case 1:
            bSame = (*(volatile BYTE*)Address == *(BYTE*)CompareAddress);
            break;
        case 2:
            bSame = (*(volatile WORD*)Address == *(WORD*)CompareAddress);
            break;
        case 4:
            bSame = (*(volatile DWORD*)Address == *(DWORD*)CompareAddress);
            break;
        case 8:
#ifdef _WIN64
            bSame = (*(volatile ULONGLONG*)Address == *(ULONGLONG*)CompareAddress);
#else
            bSame = (InterlockedCompareExchange64((volatile LONGLONG*)Address, 0, 0) == *(LONGLONG*)CompareAddress);
#endif
            break;
    }
    
    if (!bSame) {
        return TRUE;
    }
    
    // 简单的自旋等待实现（Windows XP 兼容）
    DWORD start = GetTickCount();
    while (1) {
        // 重新检查值
        switch (AddressSize) {
            case 1: bSame = (*(volatile BYTE*)Address == *(BYTE*)CompareAddress); break;
            case 2: bSame = (*(volatile WORD*)Address == *(WORD*)CompareAddress); break;
            case 4: bSame = (*(volatile DWORD*)Address == *(DWORD*)CompareAddress); break;
            case 8:
#ifdef _WIN64
                bSame = (*(volatile ULONGLONG*)Address == *(ULONGLONG*)CompareAddress);
#else
                bSame = (InterlockedCompareExchange64((volatile LONGLONG*)Address, 0, 0) == *(LONGLONG*)CompareAddress);
#endif
                break;
        }
        
        if (!bSame) return TRUE;
        
        if (dwMilliseconds != INFINITE) {
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= dwMilliseconds) {
                SetLastError(ERROR_TIMEOUT);
                return FALSE;
            }
        }
        
        Sleep(1);
    }
}

VOID WINAPI WakeByAddressSingle(PVOID Address) {
    InitSyncCompat();
    
    if (pWakeByAddressSingle) {
        pWakeByAddressSingle(Address);
        return;
    }
    
    // 回退实现：在简单的自旋实现中不需要做任何事
    (void)Address;
}

VOID WINAPI WakeByAddressAll(PVOID Address) {
    InitSyncCompat();
    
    if (pWakeByAddressAll) {
        pWakeByAddressAll(Address);
        return;
    }
    
    // 回退实现：在简单的自旋实现中不需要做任何事
    (void)Address;
}

// ============================================================================
// 为 Rust raw-dylib undecorated 符号创建 __imp_ 别名
// 
// Rust 1.78+ 使用 raw-dylib + undecorated 特性，即使在 32 位 Windows 下
// 也寻找无装饰的符号名 __imp_WaitOnAddress 而不是 __imp__WaitOnAddress@16
// 
// 参考: https://github.com/rust-lang/rust/pull/100732
// 
// 问题：在 32 位 MinGW 中，全局变量会自动添加下划线前缀
//       void* __imp_WaitOnAddress 会变成 ___imp_WaitOnAddress
//       但 Rust 寻找的是 __imp_WaitOnAddress (两个下划线)
// 
// 解决：使用内联汇编创建正确的符号别名
// ============================================================================

#ifdef __i386__
// 32 位：使用汇编创建符号别名
__asm__(".global __imp_WaitOnAddress\n\t"
        ".global __imp_WakeByAddressSingle\n\t"
        ".global __imp_WakeByAddressAll\n\t"
        ".section .data\n\t"
        ".align 4\n\t"
        "__imp_WaitOnAddress:\n\t"
        ".long _WaitOnAddress@16\n\t"
        "__imp_WakeByAddressSingle:\n\t"
        ".long _WakeByAddressSingle@4\n\t"
        "__imp_WakeByAddressAll:\n\t"
        ".long _WakeByAddressAll@4\n\t"
        ".text\n\t");
#else
// 64位：直接创建指针变量即可
void* __imp_WaitOnAddress = (void*)&WaitOnAddress;
void* __imp_WakeByAddressSingle = (void*)&WakeByAddressSingle;
void* __imp_WakeByAddressAll = (void*)&WakeByAddressAll;
#endif


