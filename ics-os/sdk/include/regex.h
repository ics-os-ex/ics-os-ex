/* ICS-OS SDK: minimal POSIX extended-regex (ERE) API.
 *
 * NetHack's sys/share/posixregex.c implements its nhregex interface on top of
 * regcomp/regexec/regfree/regerror with REG_EXTENDED | REG_NOSUB.  The ICS-OS
 * SDK provides a small self-contained ERE engine (see regex_ere_* in
 * sdk/posix.c) sufficient for the config-file / symbol-matching patterns the
 * game uses.  This is a best-effort subset of the POSIX ERE grammar:
 *   - literals (and \-escapes: \n \t \r \\ \. etc.)
 *   - . (any char except NUL), [..] classes (with ranges, negation, .] leading)
 *   - character sets via alternation
 *   - *  +  ?  quantifiers
 *   - ( )  grouping and alternation |
 *   - ^ and $  anchors
 * Matching is "search" (leftmost substring match), which is what regexec with
 * nmatch=0 reports and what NetHack relies on for option/symbol matching.
 */
#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>

#define REG_NOSUB   0x01   /* ignore match-vector related stuff */
#define REG_EXTENDED 0x00  /* use ERE syntax (default) */
#define REG_ICASE   0x02   /* case-insensitive (honored) */
#define REG_NOSPEC  0x04   /* treat pattern as literal (honored) */

#define REG_NOERROR    0
#define REG_BADPAT     1
#define REG_ECOLLATE   2
#define REG_EBRACK     3
#define REG_EBRACE     4
#define REG_ESPACE     5
#define REG_BADRPT     6
#define REG_EEND       7
#define REG_ESUBR      8
#define REG_NOMATCH    94  /* no match (regexec only) */
#define REG_ESIZE      95  /* buffer too small (regerror) */

typedef struct {
    int re_magic;
    int re_nsub;
    int re_nerr;
    /* internal engine data follows; opaque to callers */
    struct {
        int *code;     /* encoded instruction words */
        int  code_len;
        int  code_cap;
        int  nerr;
        char errmsg[64];
    } d;
} regex_t;

typedef struct {
    size_t rm_so;
    size_t rm_eo;
} regoff_t;

int  regcomp(regex_t *preg, const char *pattern, int cflags);
int  regexec(const regex_t *preg, const char *string, size_t nmatch,
             void *pmatch, int eflags);
void regfree(regex_t *preg);
size_t regerror(int errcode, const regex_t *preg, char *buf, size_t buflen);

#endif /* _REGEX_H */
