// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime entry points required by the statically linked ps5-opengl/Mesa SDK.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint64_t sceKernelGetProcessTime(void);

void cloudplay_glapi_tls_context_init(void) __asm__("_ZTH23_mesa_glapi_tls_Context");

void cloudplay_glapi_tls_context_init(void)
{
}

int mkstemps(char *template_name, int suffix_length)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned counter;
    const size_t length = template_name ? strlen(template_name) : 0;
    if (suffix_length < 0 || length < (size_t)suffix_length + 6u)
    {
        errno = EINVAL;
        return -1;
    }
    char *name = template_name + length - (size_t)suffix_length - 6u;
    for (int index = 0; index < 6; ++index)
    {
        if (name[index] != 'X')
        {
            errno = EINVAL;
            return -1;
        }
    }
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        uint64_t value = sceKernelGetProcessTime() +
            (uint64_t)__atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED) *
                UINT64_C(0x9E3779B97F4A7C15);
        for (int index = 0; index < 6; ++index)
        {
            name[index] = letters[value % 36u];
            value /= 36u;
        }
        const int file = open(template_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (file >= 0 || errno != EEXIST)
            return file;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template_name)
{
    return mkstemps(template_name, 0);
}

int isatty(int descriptor)
{
    (void)descriptor;
    errno = ENOTTY;
    return 0;
}

extern int __real_sceSystemServiceHideSplashScreen(void);
static int cloudplay_splash_released;

void hui_release_splash(void)
{
    cloudplay_splash_released = 1;
}

int __wrap_sceSystemServiceHideSplashScreen(void)
{
    return cloudplay_splash_released ? __real_sceSystemServiceHideSplashScreen() : 0;
}

void openlog(const char *identifier, int option, int facility)
{
    (void)identifier;
    (void)option;
    (void)facility;
}

FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}
