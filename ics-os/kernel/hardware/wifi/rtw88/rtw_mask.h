/* Shared Linux rtw88 register-mask semantics (kernel + host tests). */
#ifndef ICSOS_RTW_MASK_H
#define ICSOS_RTW_MASK_H

static unsigned int rtw_mask_merge(unsigned int original, unsigned int mask,
                                   unsigned int data)
{
    unsigned int shift = 0;

    if (!mask)
        return original;
    while (((mask >> shift) & 1u) == 0)
        shift++;
    return (original & ~mask) | ((data << shift) & mask);
}

#endif
