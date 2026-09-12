/*
  Name: fat_chain_unit.c
  Description: Host unit tests for the bounded FAT cluster-chain walk decision
  (kernel/filesystem/fat_chain.h). This is the fail-closed logic that bounds
  get_sector_fromcluster in filesystem/fat12.c; it is a pure function, so it is
  verified on the host. Covers: a valid step, end-of-chain (EOC / bad marker),
  an out-of-range next pointer (corrupt volume), and loop detection (a chain
  longer than the volume has clusters).
*/
#include <stdio.h>

#include "kernel/filesystem/fat_chain.h"

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
    unsigned int eoc = 0x0FFFFFF8u;   /* FAT32 end-of-chain marker */

    printf("TAP version 13\n1..14\n");

    /* --- valid step ---------------------------------------------------- */
    check("a valid in-range next cluster continues the walk",
          fat_chain_step(5u, eoc, 100u, 1u) == FCH_OK);
    check("next equal to maxent is still valid (full-volume file)",
          fat_chain_step(100u, eoc, 100u, 50u) == FCH_OK);

    /* --- end of chain -------------------------------------------------- */
    check("next equal to the EOC marker ends the chain",
          fat_chain_step(0x0FFFFFF8u, eoc, 100u, 1u) == FCH_EOC);
    check("next above the EOC marker (bad-cluster) ends the chain",
          fat_chain_step(0x0FFFFFFFu, eoc, 100u, 1u) == FCH_EOC);

    /* --- out-of-range next pointer (corrupt) --------------------------- */
    check("next just above maxent but below EOC is corrupt",
          fat_chain_step(101u, eoc, 100u, 1u) == FCH_CORRUPT);
    check("next far above maxent (still below EOC) is corrupt",
          fat_chain_step(0x0FFFFFF7u, eoc, 100u, 1u) == FCH_CORRUPT);

    /* --- loop detection (chain longer than the volume) ----------------- */
    check("step count at maxent is still allowed",
          fat_chain_step(50u, eoc, 100u, 100u) == FCH_OK);
    check("step count above maxent is a loop",
          fat_chain_step(50u, eoc, 100u, 101u) == FCH_LOOP);

    /* --- degenerate maxent (treated as 1) ----------------------------- */
    check("maxent 0 is treated as 1 (single-cluster volume)",
          fat_chain_step(1u, eoc, 0u, 1u) == FCH_OK);
    check("maxent 0: next pointer 2 is corrupt",
          fat_chain_step(2u, eoc, 0u, 1u) == FCH_CORRUPT);

    /* --- FAT numbering: N data clusters occupy ids 2 .. N+1 ----------- */
    {
        unsigned int eoc16 = 0xFFFFu;
        unsigned int count = 8119u;
        unsigned int max_id = count + 1u; /* 8120 */
        check("last FAT cluster id (count+1) is in range",
              fat_chain_step(max_id, eoc16, max_id, 100u) == FCH_OK);
        check("one past last cluster id is corrupt (not EOC)",
              fat_chain_step(max_id + 1u, eoc16, max_id, 100u) == FCH_CORRUPT);
        check("full-volume chain length equal to max id is allowed",
              fat_chain_step(50u, eoc16, max_id, max_id) == FCH_OK);
        check("chain longer than max id is a loop",
              fat_chain_step(50u, eoc16, max_id, max_id + 1u) == FCH_LOOP);
    }

    (void)g_n;
    return g_ok ? 0 : 1;
}
