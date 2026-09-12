/*
 * fat_chain.h — pure, host-testable decision logic for a bounded FAT cluster
 * chain walk.
 *
 * The real walker (get_sector_fromcluster in filesystem/fat12.c) fetches each
 * next-cluster value from the FAT (memory or disk) and asks this function how
 * to proceed. Keeping the decision pure (no kernel I/O) lets tests/fat_chain_
 * unit.c verify the fail-closed behavior (loop detection, out-of-range next
 * pointers) on the host, which is the regression guard for the GPT FAT32
 * pointer-arity hang.
 */
#ifndef FAT_CHAIN_H
#define FAT_CHAIN_H

#define FCH_OK       0   /* keep walking: next is a valid in-range cluster */
#define FCH_EOC      1   /* end of chain: next is the EOC / bad marker */
#define FCH_CORRUPT  (-1) /* next pointer is out of range for the volume */
#define FCH_LOOP     (-2) /* chain is longer than the volume has clusters */

/*
 * Decide the outcome of one step of a bounded FAT chain walk.
 *   next   - the next-cluster value just read from the FAT
 *   eoc    - the end-of-chain marker for the FAT type (>= eoc means EOC)
 *   maxent - highest valid cluster **number** on the volume (FAT data
 *            clusters are numbered 2 .. count+1, so pass count+1).
 *            Also used as the loop bound: a walk longer than this many
 *            steps is treated as a cycle.
 *   steps  - clusters counted so far, including this one (>= 1)
 */
static int fat_chain_step(unsigned int next, unsigned int eoc,
                          unsigned int maxent, unsigned int steps)
{
    if (maxent == 0)
        maxent = 1;
    if (next >= eoc)
        return FCH_EOC;
    if (next > maxent)
        return FCH_CORRUPT;
    if (steps > maxent)
        return FCH_LOOP;
    return FCH_OK;
}

#endif /* FAT_CHAIN_H */
