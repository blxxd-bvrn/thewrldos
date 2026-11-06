#ifndef TYPES_H
#define TYPES_H
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;
typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned int uintptr_t;
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
