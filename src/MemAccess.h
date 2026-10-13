#ifndef WKJELLYWORM_MEMACCESS_H
#define WKJELLYWORM_MEMACCESS_H

#include <cstddef>

bool jellyIsMemReadable(unsigned long addr, size_t size);
bool jellyIsMemWritable(unsigned long addr, size_t size);
bool jellyProbeRead(unsigned long addr, size_t size);

// SEH-guarded copies: false (and `out` undefined) when any byte faults.
bool jellySafeRead(unsigned long addr, void *out, size_t size);
bool jellySafeWrite(unsigned long addr, const void *in, size_t size);
// Copies a NUL-terminated string of at most maxLen chars (no terminator
// written) and returns its length; stops early at an unreadable byte.
size_t jellySafeStrCopy(unsigned long addr, char *dst, size_t maxLen);

#endif //WKJELLYWORM_MEMACCESS_H
