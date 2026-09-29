#include "vim.h"

/*
 * FEAT_TINY link stub. get_cmd_output() normally lives in misc1.c under
 * FEAT_EVAL/HAVE_LOCALE_H and is compiled out here yet still referenced by
 * backtick expansion in filepath.c. ICS-OS has no shell, so it is inert.
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
