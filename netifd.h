#ifndef __NETIFD_H
#define __NETIFD_H

#include <sys/socket.h>
#include <net/if.h>

#include <stdbool.h>
#include <stdio.h>

#include <libubox/list.h>
#include <libubox/uloop.h>

#include <libubus.h>
#include <uci.h>

#ifdef DEBUG
#define DPRINTF(format, ...) fprintf(stderr, "%s(%d): " format, __func__, __LINE__, ## __VA_ARGS__)
#else
#define DPRINTF(format, ...) no_debug(format, ## __VA_ARGS__)
#endif

static inline void no_debug(const char *fmt, ...)
{
}

#define __init __attribute__((constructor))

struct device;
struct interface;

extern struct uci_context *uci_ctx;
extern bool config_init;

int avl_strcmp(const void *k1, const void *k2, void *ptr);
void config_init_interfaces(const char *name);

#ifdef __linux__
static inline int fls(int x)
{
    int r = 32;

    if (!x)
        return 0;
    if (!(x & 0xffff0000u)) {
        x <<= 16;
        r -= 16;
    }
    if (!(x & 0xff000000u)) {
        x <<= 8;
        r -= 8;
    }
    if (!(x & 0xf0000000u)) {
        x <<= 4;
        r -= 4;
    }
    if (!(x & 0xc0000000u)) {
        x <<= 2;
        r -= 2;
    }
    if (!(x & 0x80000000u)) {
        x <<= 1;
        r -= 1;
    }
    return r;
}
#endif

#endif
