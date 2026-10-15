// How many bytes from p (up to n) can be read without faulting.  Only for the
// netplay debugging dumps (gamestate_serialize.cpp), which follow pointers
// found inside game objects.
#include <cstddef>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" size_t PortReadableBytes(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (p == NULL || VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return 0;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                         | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0)
        return 0;
    const char* end = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    size_t left = static_cast<size_t>(end - static_cast<const char*>(p));
    return left < n ? left : n;
}
#else
extern "C" size_t PortReadableBytes(const void*, size_t)
{
    return 0;
}
#endif
