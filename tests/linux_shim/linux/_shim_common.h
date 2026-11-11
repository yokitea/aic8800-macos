#ifndef AIC_LINUX_SHIM_COMMON_H
#define AIC_LINUX_SHIM_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
typedef uint32_t uint;
typedef unsigned long ulong;
typedef uint64_t dma_addr_t;

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#ifndef jiffies
#define jiffies 0UL
#endif

#ifndef msecs_to_jiffies
#define msecs_to_jiffies(m) ((unsigned long)(m))
#endif

#ifndef time_after
#define time_after(a, b) ((long)((b) - (a)) < 0)
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#endif

#ifndef LINUX_VERSION_CODE
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 6, 0)
#endif

struct sk_buff {
    unsigned char *data;
    unsigned int   len;
};

struct sk_buff_head {
    int dummy;
};

struct list_head {
    struct list_head *next, *prev;
};

typedef struct { int val; } atomic_t;
typedef struct { int dummy; } spinlock_t;
typedef struct { int dummy; } wait_queue_head_t;

struct completion { int done; };
struct semaphore { int count; };
struct timer_list { int dummy; };
struct work_struct { int dummy; };
struct workqueue_struct { int dummy; };
struct task_struct { int dummy; };
struct device { int dummy; };
struct usb_device { int dummy; };
struct usb_interface { int dummy; };
struct usb_anchor { int dummy; };
struct urb { int dummy; };
struct net_device { int dummy; };
struct wireless_dev { int dummy; };

#endif /* AIC_LINUX_SHIM_COMMON_H */
