/* kobject.h — kernel object base: consistent identity, ownership, lifetime.
 *
 * PURPOSE
 *   Give every long-lived kernel entity (device, driver, filesystem, mount,
 *   file handle, inode, and later process/thread/socket) the same small header
 *   so identity and lifetime are handled one way everywhere.
 *
 * RESPONSIBILITIES
 *   - Assign each object a unique id and a human-readable name + type.
 *   - Track a reference count so shared objects are freed exactly once.
 *
 * PUBLIC API
 *   kobject_init  — stamp an embedded kobject_t (refcount starts at 1).
 *   kobject_get   — take a reference (returns the object for chaining).
 *   kobject_put   — drop a reference; returns the new count (0 = last owner).
 *   kobject_type_name — printable type label.
 *
 * DEPENDENCIES
 *   None beyond stdint. Single-threaded today, so the refcount is a plain int;
 *   swap in atomics when SMP arrives without changing callers.
 *
 * FUTURE EXTENSION POINTS
 *   Embed kobject_t as the FIRST member of a struct and a pointer to that
 *   struct is also a valid kobject_t*. A release callback can be added here so
 *   kobject_put can free the owner automatically.
 */
#ifndef KOBJECT_H
#define KOBJECT_H

#include <stdint.h>

typedef enum {
    KOBJ_NONE = 0,
    KOBJ_DEVICE,
    KOBJ_DRIVER,
    KOBJ_FILESYSTEM,
    KOBJ_MOUNT,
    KOBJ_FILE,
    KOBJ_INODE,
    KOBJ_PROCESS,
    KOBJ_THREAD,
    KOBJ_MEMORY_REGION,
    KOBJ_SOCKET,
} kobj_type_t;

typedef struct kobject {
    kobj_type_t type;
    const char *name;
    uint32_t    id;
    int32_t     refcount;
} kobject_t;

void        kobject_init(kobject_t *obj, kobj_type_t type, const char *name);
kobject_t  *kobject_get(kobject_t *obj);
int32_t     kobject_put(kobject_t *obj);
const char *kobject_type_name(kobj_type_t type);

#endif /* KOBJECT_H */
