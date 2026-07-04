/* driver.h — minimal self-registering driver framework.
 *
 * A driver provides a name, an init level, and an init() function. Drivers
 * register themselves with REGISTER_DRIVER(); they are collected into a table
 * by the linker (no central list to edit). drivers_init() runs them in
 * ascending level order, so lower-level subsystems (serial, PCI) come up
 * before the devices that depend on them.
 *
 * To add a driver: drop a .c file under drivers/<subsystem>/, define a
 * driver_t, and REGISTER_DRIVER(it). That's the whole contract.
 */
#ifndef DRIVER_H
#define DRIVER_H

enum {
    DRV_LEVEL_EARLY  = 0,   /* serial console */
    DRV_LEVEL_BUS    = 1,   /* PCI */
    DRV_LEVEL_DEVICE = 2,   /* framebuffer, NIC, audio */
    DRV_LEVEL_LATE   = 3,
};

typedef struct driver {
    const char *name;
    int level;
    int (*init)(void);      /* 0 on success */
} driver_t;

/* Place a pointer to the driver in the "driver_table" section. KEEP() in the
 * linker script stops it being garbage-collected. */
#define REGISTER_DRIVER(var)                                                   \
    static const driver_t *const __drvptr_##var                                \
        __attribute__((used, section("driver_table"))) = &(var)

void drivers_init(void);

#endif /* DRIVER_H */
