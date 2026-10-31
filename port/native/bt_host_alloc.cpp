// Bullet's aligned allocation entry points for native BulletCD. Bullet 2.74's
// own btAlignedAllocator.cpp casts pointers to unsigned long, which is 32 bits
// on 64-bit Windows; these are the same two functions on the C runtime's
// aligned allocator.
#include <cstddef>
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif

void* btAlignedAllocInternal(size_t size, int alignment)
{
#ifdef _WIN32
    return _aligned_malloc(size, (size_t)alignment);
#else
    void* p = nullptr;
    return posix_memalign(&p, (size_t)alignment < sizeof(void*) ? sizeof(void*) : (size_t)alignment, size) ? nullptr : p;
#endif
}

void btAlignedFreeInternal(void* ptr)
{
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
