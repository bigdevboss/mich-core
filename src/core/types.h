#ifndef TYPES_H
#define TYPES_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef signed short i16;
typedef signed int i32;
typedef signed long long i64;

#if __SIZEOF_POINTER__ == 8
typedef u64 uptr_t;
typedef u64 usize_t;
typedef u64 reg_t;
typedef u64 paddr_t;
typedef u64 vaddr_t;
#else
typedef u32 uptr_t;
typedef u32 usize_t;
typedef u32 reg_t;
typedef u32 paddr_t;
typedef u32 vaddr_t;
#endif

#endif
