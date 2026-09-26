// SPDX-License-Identifier: GPL-3.0-or-later
// Narrow POSIX shims for symbols missing from the public PS5 libc import stubs.
#if CLOUDPLAY_PS5
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <net/if.h>
#include <pthread.h>
#include <pwd.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xlocale/_locale.h>

// The console app uses the C numeric locale throughout. json-c switches its
// thread locale to C while parsing numbers; represent that as the global C
// locale rather than handing it a made-up pointer.
locale_t uselocale(locale_t loc) { (void)loc; return LC_GLOBAL_LOCALE; }
locale_t duplocale(locale_t loc) { return loc; }
locale_t newlocale(int mask, const char *name, locale_t base) {
    (void)mask;
    (void)base;
    if (!name || (name[0] && strcmp(name, "C") && strcmp(name, "POSIX"))) {
        errno = EINVAL;
        return NULL;
    }
    return LC_GLOBAL_LOCALE;
}
int freelocale(locale_t loc) { (void)loc; return 0; }

static pthread_mutex_t time_lock = PTHREAD_MUTEX_INITIALIZER;
struct tm *gmtime_r(const time_t *value, struct tm *out) {
    if (!value || !out) return NULL;
    pthread_mutex_lock(&time_lock);
    struct tm *tmp = gmtime(value);
    if (tmp) memcpy(out, tmp, sizeof(*out));
    pthread_mutex_unlock(&time_lock);
    return tmp ? out : NULL;
}

struct tm *localtime_r(const time_t *value, struct tm *out) {
    if (!value || !out) return NULL;
    pthread_mutex_lock(&time_lock);
    struct tm *tmp = localtime(value);
    if (tmp) memcpy(out, tmp, sizeof(*out));
    pthread_mutex_unlock(&time_lock);
    return tmp ? out : NULL;
}

// The app runs with a fixed UTF-8 C locale. PacBrew libiconv queries these
// libc locale helpers while choosing its conversion table.
char *nl_langinfo(nl_item item) {
    return item == CODESET ? "UTF-8" : "";
}
int ___mb_cur_max(void) { return 4; }

int pipe2(int fds[2], int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) { errno = EINVAL; return -1; }
    if (pipe(fds) < 0) return -1;
    if ((flags & O_CLOEXEC) &&
        (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
         fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0)) goto fail;
    if ((flags & O_NONBLOCK) &&
        (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 ||
         fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0)) goto fail;
    return 0;
fail:
    { int saved = errno; close(fds[0]); close(fds[1]); errno = saved; }
    return -1;
}

// Cloud endpoints do not use IPv6 link-local scopes. Unknown interface names
// must fail explicitly rather than silently resolving to the wrong interface.
unsigned int if_nametoindex(const char *name) { (void)name; return 0; }
char *if_indextoname(unsigned int index, char *name) {
    (void)index; (void)name; errno = ENXIO; return NULL;
}

// libevent calls this to supplement arc4random's own system-seeded pool.
// arc4random remains the source of random bytes used by the application.
void arc4random_addrandom(unsigned char *bytes, int count) {
    (void)bytes; (void)count;
}

int dladdr(const void *addr, Dl_info *info) {
    (void)addr;
    if (info) memset(info, 0, sizeof(*info));
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buffer, size_t size,
               struct passwd **result) {
    (void)uid; (void)pwd; (void)buffer; (void)size;
    if (result) *result = NULL;
    return ENOENT;
}

ssize_t sendmmsg(int fd, struct mmsghdr *msgs, size_t count, int flags) {
    if (!count) { errno = EINVAL; return -1; }
    size_t done = 0;
    for (; done < count; ++done) {
        ssize_t sent = sendmsg(fd, &msgs[done].msg_hdr, flags);
        if (sent < 0) return done ? (ssize_t)done : -1;
        msgs[done].msg_len = (unsigned int)sent;
    }
    return (ssize_t)done;
}

ssize_t recvmmsg(int fd, struct mmsghdr *msgs, size_t count, int flags,
                 const struct timespec *timeout) {
    (void)timeout;
    if (!count) { errno = EINVAL; return -1; }
    size_t done = 0;
    for (; done < count; ++done) {
        ssize_t received = recvmsg(fd, &msgs[done].msg_hdr,
                                   flags | (done ? MSG_DONTWAIT : 0));
        if (received < 0) return done ? (ssize_t)done : -1;
        msgs[done].msg_len = (unsigned int)received;
    }
    return (ssize_t)done;
}

// Preserve the caller's stack frame for setjmp/longjmp. An ordinary C wrapper
// would leave a dead stack frame in the saved jump context.
__asm__(".globl _setjmp\n_setjmp:\n jmp setjmp\n"
        ".globl _longjmp\n_longjmp:\n jmp longjmp\n");
#endif
