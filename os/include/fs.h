/* fs.h — filesystem subsystem bring-up.
 *
 * PURPOSE
 *   The single entry point that initialises the VFS, registers the available
 *   filesystem implementations, and mounts a root. Keeps kernel_main free of
 *   filesystem-specific knowledge and keeps the VFS core unaware of which
 *   concrete filesystems exist.
 *
 * FUTURE EXTENSION POINTS
 *   As filesystems are added (disk-backed native FS, FAT32, ext2) they get
 *   registered here; mounting them under /mnt/... needs no API change.
 */
#ifndef FS_H
#define FS_H

/* Initialise VFS, register filesystems, mount ramfs at "/". Returns 0 on ok. */
int fs_init(void);

/* Filesystem registration hooks (implemented by each fs). */
void ramfs_register(void);

#endif /* FS_H */
