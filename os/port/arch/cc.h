/* arch/cc.h — lwIP compiler/architecture abstraction for talk-os. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* x86 is little-endian. */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* We have no <inttypes.h>; define lwIP's printf format strings directly. */
#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

/* Struct packing (GCC). */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x

/* Diagnostics + assertions routed to the kernel console. */
extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { kprintf("\nlwIP ASSERT: %s\n", x); panic("lwip assert"); } while (0)

/* Randomness for TCP ISN / DNS txids. */
extern unsigned int lwip_rand(void);
#define LWIP_RAND() ((u32_t)lwip_rand())

#endif /* LWIP_ARCH_CC_H */
