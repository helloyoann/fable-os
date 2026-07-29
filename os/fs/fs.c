/* fs.c — filesystem subsystem bring-up (see include/fs.h). */

#include "fs.h"
#include "vfs.h"
#include "kernel.h"

int fs_init(void) {
    vfs_init();

    int rc = ramfs_register();      /* register available filesystems here */
    if (rc != VFS_OK) {
        kprintf("fs: registering ramfs failed (%d)\n", rc);
        return rc;
    }

    rc = vfs_mount("/", "ramfs");
    if (rc != VFS_OK) {
        kprintf("fs: mounting ramfs at / failed (%d)\n", rc);
        return rc;
    }
    return 0;
}
