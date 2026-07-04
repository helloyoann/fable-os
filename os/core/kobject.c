/* kobject.c — kernel object base (see include/kobject.h). */

#include "kobject.h"
#include "kernel.h"

static uint32_t next_id = 1;   /* 0 is reserved as "no object" */

void kobject_init(kobject_t *obj, kobj_type_t type, const char *name) {
    obj->type     = type;
    obj->name     = name;
    obj->id       = next_id++;
    obj->refcount = 1;
}

kobject_t *kobject_get(kobject_t *obj) {
    if (obj) obj->refcount++;
    return obj;
}

int32_t kobject_put(kobject_t *obj) {
    if (!obj) return 0;
    if (obj->refcount <= 0) panic("kobject_put: refcount underflow");
    return --obj->refcount;
}

const char *kobject_type_name(kobj_type_t type) {
    switch (type) {
        case KOBJ_DEVICE:        return "device";
        case KOBJ_DRIVER:        return "driver";
        case KOBJ_FILESYSTEM:    return "filesystem";
        case KOBJ_MOUNT:         return "mount";
        case KOBJ_FILE:          return "file";
        case KOBJ_INODE:         return "inode";
        case KOBJ_PROCESS:       return "process";
        case KOBJ_THREAD:        return "thread";
        case KOBJ_MEMORY_REGION: return "memory-region";
        case KOBJ_SOCKET:        return "socket";
        default:                 return "none";
    }
}
