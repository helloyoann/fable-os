/* assert.h — freestanding shim. A failed assertion panics the kernel. */
#ifndef PORT_ASSERT_H
#define PORT_ASSERT_H

void panic(const char *msg);

#define assert(expr) do { if (!(expr)) panic("assert failed: " #expr); } while (0)

#endif /* PORT_ASSERT_H */
