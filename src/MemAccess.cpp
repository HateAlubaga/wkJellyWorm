#include <Windows.h>
#include "MemAccess.h"

bool jellyIsMemReadable(unsigned long addr, size_t size) {
	MEMORY_BASIC_INFORMATION mbi{};
	if(!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
		return false;
	if(mbi.State != MEM_COMMIT)
		return false;
	DWORD prot = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
	switch(prot) {
		case PAGE_READONLY:
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READ:
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

bool jellyProbeRead(unsigned long addr, size_t size) {
	if(!addr || size == 0)
		return false;
	if(!jellyIsMemReadable(addr, size))
		return false;
	return !IsBadReadPtr((LPCVOID)addr, size);
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
