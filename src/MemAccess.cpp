#include <Windows.h>
#include <cstring>
#include "MemAccess.h"

// Readability used to be checked with VirtualQuery before every access. In
// WA's WOW64 process that is a ~1 us system call, and Lua scripts make
// thousands of memory reads per frame (readMemString even checked byte by
// byte), which cost several milliseconds per frame. The memory is now touched
// under SEH instead: ~10 ns when readable, and a fault is caught. Writability
// still uses VirtualQuery, as it cannot be probed without writing.

namespace {

const ULONG_PTR kPageSize = 0x1000;
const ULONG_PTR kMinAddress = 0x10000; // the first 64 KB are never mapped

bool rangeOk(ULONG_PTR addr, size_t size) {
	return addr >= kMinAddress && size > 0 && addr + size > addr;
}

// Reads the first byte of every page in [addr, addr + size): page protection
// is per page, so that covers every byte of the range.
bool touchPages(ULONG_PTR addr, size_t size) {
	__try {
		(void)*(volatile const char *)addr;
		ULONG_PTR end = addr + size;
		for(ULONG_PTR page = (addr & ~(kPageSize - 1)) + kPageSize; page < end; page += kPageSize)
			(void)*(volatile const char *)page;
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

} // namespace

bool jellyIsMemReadable(unsigned long addr, size_t size) {
	if(size == 0)
		size = 1;
	if(!rangeOk(addr, size))
		return false;
	return touchPages(addr, size);
}

bool jellyProbeRead(unsigned long addr, size_t size) {
	if(!addr || size == 0)
		return false;
	return jellyIsMemReadable(addr, size);
}

bool jellySafeRead(unsigned long addr, void *out, size_t size) {
	if(!rangeOk(addr, size))
		return false;
	__try {
		memcpy(out, (const void *)addr, size);
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

bool jellySafeWrite(unsigned long addr, const void *in, size_t size) {
	if(!rangeOk(addr, size))
		return false;
	__try {
		memcpy((void *)addr, in, size);
		return true;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

size_t jellySafeStrCopy(unsigned long addr, char *dst, size_t maxLen) {
	// volatile: the count must be exact at the instruction that faults
	volatile size_t n = 0;
	if(!rangeOk(addr, 1))
		return 0;
	__try {
		while(n < maxLen) {
			char c = *(volatile const char *)(addr + n);
			if(!c)
				break;
			dst[n] = c;
			n = n + 1;
		}
	} __except(EXCEPTION_EXECUTE_HANDLER) {
	}
	return n;
}

bool jellyIsMemWritable(unsigned long addr, size_t size) {
	MEMORY_BASIC_INFORMATION mbi{};
	if(!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
		return false;
	if(mbi.State != MEM_COMMIT)
		return false;
	DWORD prot = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
	switch(prot) {
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			break;
		default:
			return false;
	}
	ULONG_PTR end = (ULONG_PTR)addr + size;
	ULONG_PTR regionEnd = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
	return end <= regionEnd;
}
