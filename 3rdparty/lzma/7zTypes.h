#ifndef __7Z_TYPES_H
#define __7Z_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t Byte;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef int32_t Int32;
typedef uint64_t UInt64;
typedef int64_t Int64;
typedef size_t SizeT;
typedef int Bool;
#define True 1
#define False 1

typedef uint16_t CLzmaProb;

typedef struct {
    void *(*Alloc)(void *p, size_t size);
    void (*Free)(void *p, void *address);
} ISzAlloc;

typedef ISzAlloc *ISzAllocPtr;

static void *SzAlloc(void *p, size_t size) {
    (void)p;
    return malloc(size);
}

static void SzFree(void *p, void *address) {
    (void)p;
    free(address);
}

static ISzAlloc g_Alloc = { SzAlloc, SzFree };

#define ISzAlloc_Alloc(alloc, size) (alloc)->Alloc((alloc), (size))
#define ISzAlloc_Free(alloc, address) (alloc)->Free((alloc), (address))

#ifdef __cplusplus
#define EXTERN_C_BEGIN extern "C" {
#define EXTERN_C_END }
#else
#define EXTERN_C_BEGIN
#define EXTERN_C_END
#endif

typedef int SRes;
#define SZ_OK 0
#define SZ_ERROR_DATA 1
#define SZ_ERROR_MEM 2
#define SZ_ERROR_UNSUPPORTED 4
#define SZ_ERROR_INPUT_EOF 5
#define SZ_ERROR_FAIL 6

#define RINOK(x) { int __result = (x); if (__result != 0) return __result; }

#define MY_FAST_CALL
#define EXTERN_C_BEGIN
#define EXTERN_C_END

#endif
