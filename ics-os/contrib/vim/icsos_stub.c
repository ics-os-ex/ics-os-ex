#include "vim.h"

/*
 * FEAT_TINY link stubs. get_cmd_output() normally lives in misc1.c under
 * FEAT_EVAL/HAVE_LOCALE_H and term_set_winsize() in term.c under HAVE_TGETENT;
 * both are compiled out here yet still referenced (backtick expansion in
 * filepath.c, and mch_set_shellsize() in os_unix.c). ICS-OS has no shell and
 * no terminfo, so both are inert.
 */
char_u *
get_cmd_output(
    char_u    *cmd,
    char_u    *infile,
    int       flags,
    int       *ret_len)
{
    (void)cmd;
    (void)infile;
    (void)flags;
    (void)ret_len;
    return NULL;
}

void
term_set_winsize(int height, int width)
{
    (void)height;
    (void)width;
}
