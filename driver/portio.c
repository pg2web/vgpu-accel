/* SPDX-License-Identifier: MIT */
/*
 * x86 port I/O for the virtio library's legacy (I/O BAR) path. MSVC provides these as
 * intrinsics; clang-cl does not, so they are implemented here.
 */
#include <ntddk.h>

UCHAR __inbyte(USHORT Port)
{
    UCHAR v;
    __asm__ __volatile__("inb %w1, %b0" : "=a"(v) : "Nd"(Port));
    return v;
}

USHORT __inword(USHORT Port)
{
    USHORT v;
    __asm__ __volatile__("inw %w1, %w0" : "=a"(v) : "Nd"(Port));
    return v;
}

ULONG __indword(USHORT Port)
{
    ULONG v;
    __asm__ __volatile__("inl %w1, %k0" : "=a"(v) : "Nd"(Port));
    return v;
}

void __outbyte(USHORT Port, UCHAR Value)
{
    __asm__ __volatile__("outb %b0, %w1" : : "a"(Value), "Nd"(Port));
}

void __outword(USHORT Port, USHORT Value)
{
    __asm__ __volatile__("outw %w0, %w1" : : "a"(Value), "Nd"(Port));
}

void __outdword(USHORT Port, ULONG Value)
{
    __asm__ __volatile__("outl %k0, %w1" : : "a"(Value), "Nd"(Port));
}
