/* drivers.c — run all registered drivers in init-level order. */

#include "driver.h"
#include "kernel.h"

/* Provided by the linker (see linker.ld / GNU ld __start_/__stop_ symbols). */
extern const driver_t *const __start_driver_table[];
extern const driver_t *const __stop_driver_table[];

void drivers_init(void) {
    const driver_t *const *base = __start_driver_table;
    const driver_t *const *end = __stop_driver_table;
    int count = (int)(end - base);

    for (int level = DRV_LEVEL_EARLY; level <= DRV_LEVEL_LATE; level++) {
        for (int i = 0; i < count; i++) {
            const driver_t *d = base[i];
            if (d->level != level) continue;
            int rc = d->init ? d->init() : 0;
            if (rc != 0)
                kprintf("driver %s: init failed (%d)\n", d->name, rc);
        }
    }
}
