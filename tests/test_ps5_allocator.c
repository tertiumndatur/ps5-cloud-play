#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *__wrap_malloc(size_t size);
void __wrap_free(void *address);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *address, size_t size);
int __wrap_posix_memalign(void **address, size_t alignment, size_t size);
char *__wrap_strdup(const char *source);
size_t cloudplay_allocator_metric(int metric);

void *__real_malloc(size_t size) { return malloc(size); }
void __real_free(void *address) { free(address); }
void *__real_realloc(void *address, size_t size) { return realloc(address, size); }
int __real_posix_memalign(void **address, size_t alignment, size_t size)
{
    return posix_memalign(address, alignment, size);
}

static void *stress(void *opaque)
{
    uintptr_t seed = (uintptr_t)opaque + 1;
    void *slots[256] = { 0 };
    for(size_t iteration = 0; iteration < 20000; iteration++)
    {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        size_t slot = (seed >> 24) % 256;
        seed = seed * UINT64_C(6364136223846793005) + 1;
        size_t size = 1 + ((seed >> 16) % (256 * 1024));
        if(slots[slot] && (seed & 1))
        {
            void *resized = __wrap_realloc(slots[slot], size);
            assert(resized);
            slots[slot] = resized;
        }
        else
        {
            __wrap_free(slots[slot]);
            slots[slot] = __wrap_malloc(size);
            assert(slots[slot]);
            memset(slots[slot], (int)slot, size);
        }
    }
    for(size_t i = 0; i < 256; i++)
        __wrap_free(slots[i]);
    return NULL;
}

int main(void)
{
    void *zeroed = __wrap_calloc(4096, 8);
    assert(zeroed);
    for(size_t i = 0; i < 4096 * 8; i++)
        assert(((unsigned char *)zeroed)[i] == 0);
    __wrap_free(zeroed);

    void *aligned = NULL;
    assert(__wrap_posix_memalign(&aligned, 4096, 12345) == 0);
    assert(((uintptr_t)aligned & 4095) == 0);
    __wrap_free(aligned);

    char *copy = __wrap_strdup("allocator-check");
    assert(copy && strcmp(copy, "allocator-check") == 0);
    __wrap_free(copy);

    void *system = __real_malloc(128);
    assert(system);
    system = __wrap_realloc(system, 256);
    assert(system);
    __wrap_free(system);

    pthread_t workers[4];
    for(uintptr_t i = 0; i < 4; i++)
        assert(pthread_create(&workers[i], NULL, stress, (void *)i) == 0);
    for(size_t i = 0; i < 4; i++)
        assert(pthread_join(workers[i], NULL) == 0);

    assert(cloudplay_allocator_metric(0) >= 1);
    assert(cloudplay_allocator_metric(1) >= 64 * 1024 * 1024);
    assert(cloudplay_allocator_metric(2) == 0);
    assert(cloudplay_allocator_metric(3) > 0);
    assert(cloudplay_allocator_metric(4) == 0);
    return 0;
}
