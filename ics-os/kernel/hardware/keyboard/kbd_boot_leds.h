#ifndef KBD_BOOT_LEDS_H
#define KBD_BOOT_LEDS_H

/* i8042 Set LEDs payload: bit0 Scroll, bit1 Num, bit2 Caps.
   Scroll is unused: chiclet boards (N150) have no Scroll LED. */
#define KBD_LED_SCROLL 1u
#define KBD_LED_NUM    2u
#define KBD_LED_CAPS   4u

/*
  Caps + Num only. Stage 0 is Caps (and the driver first sends 0 so firmware
  Num Lock turns off). Then:

    vis = (stage - 1) & 3
    0: Caps           stages 1, 5, 9, 13
    1: Num            stages 2, 6, 10, 14
    2: Caps+Num       stages 3, 7, 11, 15
    3: Caps           stages 4, 8, 12, 16
*/
static inline unsigned char kbd_boot_led_bits(unsigned int stage)
{
    unsigned int vis;

    if (stage == 0)
        return (unsigned char)KBD_LED_CAPS;
    vis = (stage - 1u) & 3u;
    if (vis == 1)
        return (unsigned char)KBD_LED_NUM;
    if (vis == 2)
        return (unsigned char)(KBD_LED_CAPS | KBD_LED_NUM);
    return (unsigned char)KBD_LED_CAPS;
}

#endif
