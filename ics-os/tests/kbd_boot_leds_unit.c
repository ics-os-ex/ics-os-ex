/*
  Host TAP for Caps+Num boot-stage LED encoding (no Scroll Lock LED).
*/
#include <stdio.h>

#include "kernel/hardware/keyboard/kbd_boot_leds.h"

static int check(const char *name, int condition)
{
    if (!condition) {
        printf("not ok - %s\n", name);
        return 0;
    }
    printf("ok - %s\n", name);
    return 1;
}

int main(void)
{
    int ok = 1;

    printf("TAP version 13\n1..10\n");

    ok &= check("stage 0 is Caps (C entry, firmware Num cleared separately)",
                kbd_boot_led_bits(0) == KBD_LED_CAPS);
    ok &= check("stage 1 is Caps (IDT)",
                kbd_boot_led_bits(1) == KBD_LED_CAPS);
    ok &= check("stage 2 is Num only (mem_init)",
                kbd_boot_led_bits(2) == KBD_LED_NUM);
    ok &= check("stage 3 is Caps+Num (console)",
                kbd_boot_led_bits(3) == (KBD_LED_CAPS | KBD_LED_NUM));
    ok &= check("stage 4 is Caps (CPU info)",
                kbd_boot_led_bits(4) == KBD_LED_CAPS);
    ok &= check("stage 7 is Caps+Num (alloc)",
                kbd_boot_led_bits(7) == (KBD_LED_CAPS | KBD_LED_NUM));
    ok &= check("fault pattern 7 is Caps+Num (visible without Scroll)",
                kbd_boot_led_bits(7) == (KBD_LED_CAPS | KBD_LED_NUM));
    ok &= check("stage 2 never lights Caps (distinct from console)",
                !(kbd_boot_led_bits(2) & KBD_LED_CAPS) &&
                (kbd_boot_led_bits(2) & KBD_LED_NUM));
    ok &= check("Scroll bit is never used",
                !(kbd_boot_led_bits(0) & KBD_LED_SCROLL) &&
                !(kbd_boot_led_bits(2) & KBD_LED_SCROLL) &&
                !(kbd_boot_led_bits(3) & KBD_LED_SCROLL) &&
                !(kbd_boot_led_bits(7) & KBD_LED_SCROLL));
    ok &= check("stage 3 lights Caps so it cannot look like firmware Num Lock",
                (kbd_boot_led_bits(3) & KBD_LED_CAPS) &&
                (kbd_boot_led_bits(3) & KBD_LED_NUM));

    return ok ? 0 : 1;
}
