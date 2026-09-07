/* pathdef.c for the ICS-OS freestanding vim build.
 *
 * vim's normal build generates this from pathdef.c.in via ./configure. ICS-OS
 * does not run configure, so it is provided here with values appropriate to
 * the ICS-OS filesystem layout. These are the strings reported by ":version".
 */
#include "vim.h"

char_u *default_vim_dir = (char_u *)"/icsos/share/vim";
char_u *default_vimruntime_dir = (char_u *)"";
char_u *all_cflags = (char_u *)"-m64 -std=gnu99 -w -nostdlib -ffreestanding -static -DHAVE_CONFIG_H ";
char_u *all_lflags = (char_u *)"-m64 -no-pie -nostdlib -static -Wl,--gc-sections ";
char_u *compiled_user = (char_u *)"icsos";
char_u *compiled_sys = (char_u *)"ics-os";
