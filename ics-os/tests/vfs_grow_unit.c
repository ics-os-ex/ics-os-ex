/*
 * Host TAP tests for vfs_units_covering (kernel/vfs/vfs_grow.h).
 * The 16 KiB cluster / 4 KiB write case is the fatwr hole at offset 81920.
 */
#include <stdio.h>
#include "kernel/vfs/vfs_grow.h"

static int g_ok = 1;
static int g_n = 0;

static void check(const char *name, int cond)
{
   g_n++;
   if (!cond) {
      printf("not ok %d - %s\n", g_n, name);
      g_ok = 0;
   } else {
      printf("ok %d - %s\n", g_n, name);
   }
}

int main(void)
{
   unsigned int bpc = 16384;

   printf("TAP version 13\n");
   printf("1..8\n");

   check("zero size uses no units", vfs_units_covering(0, bpc) == 0);
   check("unit 0 is 0", vfs_units_covering(4096, 0) == 0);
   check("partial first cluster", vfs_units_covering(4096, bpc) == 1);
   check("exact one cluster", vfs_units_covering(bpc, bpc) == 1);
   check("first byte of second cluster needs two",
         vfs_units_covering(bpc + 1, bpc) == 2);
   check("4KiB append at exact cluster boundary needs a new unit",
         vfs_units_covering(bpc + 4096, bpc) == 2);
   check("old size/unit+1 over-counted an exact cluster",
         (bpc / bpc + 1) == 2 && vfs_units_covering(bpc, bpc) == 1);
   check("80KiB is five 16KiB clusters",
         vfs_units_covering(81920, bpc) == 5);

   return g_ok ? 0 : 1;
}
