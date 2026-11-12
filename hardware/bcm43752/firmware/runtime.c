#include "native_calls.h"
#include <stddef.h>

/* Freestanding GCC may lower structure copies/zero initializers to these
 * symbols even with -fno-builtin. Keep them in the checked patch image. */
void *memcpy(void *to, const void *from, size_t bytes)
{
    return ((void *(*)(void *, const void *, size_t))native_0x151a21)(to, from, bytes);
}

void *memset(void *to, int value, size_t bytes)
{
    unsigned char *p = to;
    while (bytes--)
        *p++ = (unsigned char)value;
    return to;
}
