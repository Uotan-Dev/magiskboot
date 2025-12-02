// Windows 7 compatible BCrypt primitives
// Provides implementations for ProcessPrng (Windows 8+)
// Based on YY-Thunks: https://github.com/Chuyu-Team/YY-Thunks/blob/master/src/Thunks/BCryptPrimitives.hpp
// 
// ProcessPrng 在 Windows 7 上不存在，需要回退到 RtlGenRandom (SystemFunction036)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

// RtlGenRandom (advapi32.dll, Windows XP+)
// 也被称为 SystemFunction036
typedef BOOLEAN (WINAPI *PFN_RtlGenRandom)(
    PVOID RandomBuffer,
    ULONG RandomBufferLength
);

// ProcessPrng (bcryptprimitives.dll, Windows 8+)
typedef BOOL (WINAPI *PFN_ProcessPrng)(
    PBYTE pbData,
    SIZE_T cbData
);

static PFN_ProcessPrng pProcessPrng = NULL;
static PFN_RtlGenRandom pRtlGenRandom = NULL;
static volatile LONG bInitialized = 0;

// 动态加载函数
static void InitBCryptCompat(void) {
    if (InterlockedCompareExchange(&bInitialized, 1, 0) == 0) {
        // 尝试加载原生 ProcessPrng (Windows 8+)
        HMODULE hBCryptPrimitives = LoadLibraryA("bcryptprimitives.dll");
        if (hBCryptPrimitives) {
            pProcessPrng = (PFN_ProcessPrng)GetProcAddress(hBCryptPrimitives, "ProcessPrng");
        }
        
        // 如果没有原生 API，加载 RtlGenRandom 作为回退
        if (!pProcessPrng) {
            HMODULE hAdvapi32 = LoadLibraryA("advapi32.dll");
            if (hAdvapi32) {
                pRtlGenRandom = (PFN_RtlGenRandom)GetProcAddress(hAdvapi32, "SystemFunction036");
            }
        }
    }
}

// ProcessPrng 实现
BOOL WINAPI ProcessPrng(
    PBYTE pbData,
    SIZE_T cbData
) {
    InitBCryptCompat();
    
    // 如果系统支持原生 ProcessPrng，使用原生的
    if (pProcessPrng) {
        return pProcessPrng(pbData, cbData);
    }
    
    // 回退到 RtlGenRandom (Windows XP/7 兼容)
    if (pRtlGenRandom) {
        // RtlGenRandom 最大一次只能生成 MAXLONG 字节
        ULONG maxChunk = 0x7FFFFFFF; // MAXLONG
        SIZE_T remaining = cbData;
        PBYTE current = pbData;
        
        while (remaining > 0) {
            ULONG chunk = (remaining > maxChunk) ? maxChunk : (ULONG)remaining;
            if (!pRtlGenRandom(current, chunk)) {
                SetLastError(ERROR_GEN_FAILURE);
                return FALSE;
            }
            current += chunk;
            remaining -= chunk;
        }
        return TRUE;
    }
    
    // 如果连 RtlGenRandom 都没有（不应该发生），返回失败
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
}

// ============================================================================
// 为 Rust raw-dylib undecorated 符号创建 __imp_ 别名
// 
// Rust 1.78+ 使用 raw-dylib + undecorated 特性，即使在 32 位 Windows 下
// 也寻找无装饰的符号名 __imp_ProcessPrng 而不是 __imp__ProcessPrng@8
// 
// 参考: https://github.com/rust-lang/rust/pull/100732
// ============================================================================

#ifdef __i386__
// 32 位：使用汇编创建符号别名
__asm__(".global __imp_ProcessPrng\n\t"
        ".section .data\n\t"
        ".align 4\n\t"
        "__imp_ProcessPrng:\n\t"
        ".long _ProcessPrng@8\n\t"
        ".text\n\t");
#else
// 64位：直接创建指针变量即可
void* __imp_ProcessPrng = (void*)&ProcessPrng;
#endif
