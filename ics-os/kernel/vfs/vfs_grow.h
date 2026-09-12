#ifndef VFS_GROW_H
#define VFS_GROW_H

/*
 * How many allocation units cover `bytes`. unit==0 is treated as 0.
 * size/unit+1 over-counts a file that is an exact multiple of the unit, so
 * vfs_directwrite skipped fat_addsectors at cluster boundaries and left
 * unread (zero) holes. Host-tested by tests/vfs_grow_unit.c.
 */
static inline unsigned int vfs_units_covering(unsigned int bytes,
                                              unsigned int unit)
{
   if (unit == 0)
      return 0;
   if (bytes == 0)
      return 0;
   return (bytes + unit - 1) / unit;
}

#endif
