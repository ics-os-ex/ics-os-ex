/*
  Host TAP for DOS/batch comment token recognition (script_comment.h).
*/
#include <stdio.h>
#include <string.h>

#include "kernel/console/script_comment.h"

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
    printf("TAP version 13\n1..8\n");
    ok &= check("rem is comment", script_is_comment_token("rem"));
    ok &= check("REM is comment", script_is_comment_token("REM"));
    ok &= check("Rem is comment", script_is_comment_token("Rem"));
    ok &= check("hash is comment", script_is_comment_token("#foo"));
    ok &= check("tick is comment", script_is_comment_token("'foo"));
    ok &= check("copy is not comment", !script_is_comment_token("copy"));
    ok &= check("remark is not rem", !script_is_comment_token("remark"));
    ok &= check("null is not comment", !script_is_comment_token(0));
    return ok ? 0 : 1;
}
