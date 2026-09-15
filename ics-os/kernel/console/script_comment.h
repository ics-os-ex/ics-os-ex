/*
  DOS/batch comment recognition for the kernel script interpreter.
  Host-testable (no kernel types).
*/
#ifndef SCRIPT_COMMENT_H
#define SCRIPT_COMMENT_H

/* 1 if the first token is a batch comment: rem/REM, or '#' / "'" line. */
static inline int script_is_comment_token(const char *tok)
{
    int i;
    char c0, c1, c2;
    if (!tok || !tok[0])
        return 0;
    if (tok[0] == '#' || tok[0] == '\'')
        return 1;
    c0 = tok[0];
    c1 = tok[1];
    c2 = tok[2];
    if ((c0 == 'r' || c0 == 'R') &&
        (c1 == 'e' || c1 == 'E') &&
        (c2 == 'm' || c2 == 'M') &&
        tok[3] == 0)
        return 1;
    /* Also accept "rem." style only as exact rem. */
    (void)i;
    return 0;
}

#endif
