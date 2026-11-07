#ifndef TYPES_H
#define TYPES_H
/* the compiler's own definitions, so they agree with <stdint.h> on both architectures */
typedef __UINT8_TYPE__ uint8_t;
typedef __INT8_TYPE__ int8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __INT64_TYPE__ int64_t;
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__ intptr_t;
typedef int bool;
#define true 1
#define false 0
#define NULL ((void*)0)
#define offsetof(t, m) __builtin_offsetof(t, m)
typedef __builtin_va_list va_list;
#define va_start(v,l) __builtin_va_start(v,l)
#define va_end(v)     __builtin_va_end(v)
#define va_arg(v,l)   __builtin_va_arg(v,l)
#define PACKED __attribute__((packed))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ABSDIFF(a,b) ((a)>(b)?(a)-(b):(b)-(a))
#define CLAMP(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
#define ARRAY_LEN(a) (sizeof(a)/sizeof((a)[0]))
#endif
