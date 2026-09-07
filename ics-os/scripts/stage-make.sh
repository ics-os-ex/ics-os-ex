#!/bin/bash
# Stage GNU Make 3.82 sources for an in-OS TinyCC build onto /work.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
DEST="${1:-/tmp/icsos-make}"
TARBALL="$REPO/refernces/make-3.82.tar.gz"
SRC="$REPO/refernces/make-3.82"

mkdir -p "$REPO/refernces"
if [ ! -d "$SRC" ]; then
  if [ ! -f "$TARBALL" ]; then
    curl -fsSL -o "$TARBALL" "https://ftp.gnu.org/gnu/make/make-3.82.tar.gz"
  fi
  tar -C "$REPO/refernces" -xzf "$TARBALL"
fi

rm -rf "$DEST"
mkdir -p "$DEST"
cp -a "$SRC/." "$DEST/"
cp "$ROOT/contrib/gnumake/config.h" "$DEST/config.h"
python3 - "$DEST/job.c" << 'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '#include "make.h"\n'
new = '#include "make.h"\n#ifdef ICSOS\n# include <spawn.h>\n#endif\n'
if old not in s:
    sys.exit('job.c: make.h include not found')
s = s.replace(old, new, 1)
needle = '#else  /* !__EMX__ */\n\n      child->pid = vfork ();'
repl = '''#else  /* !__EMX__ */
#ifdef ICSOS
      {
        pid_t spid;
        int se;
        se = posix_spawn (&spid, argv[0] ? argv[0] : "", 0, 0, argv,
                          child->environment);
        unblock_sigs ();
        if (se != 0)
          {
            perror_with_name ("posix_spawn", argv[0] ? argv[0] : "");
            goto error;
          }
        child->pid = spid;
      }
#else

      child->pid = vfork ();'''
if needle not in s:
    sys.exit('job.c: vfork site not found')
s = s.replace(needle, repl, 1)
s = s.replace(
    '\t  perror_with_name ("vfork", "");\n\t  goto error;\n\t}\n# endif  /* !__EMX__ */',
    '\t  perror_with_name ("vfork", "");\n\t  goto error;\n\t}\n#endif /* !ICSOS */\n# endif  /* !__EMX__ */',
    1)
open(p,'w').write(s)
print('patched job.c for ICSOS posix_spawn')
PY
python3 - "$DEST" << 'PY'
import sys, os
root = sys.argv[1]
needle = b'printf ("MAKE_DBG'
repl = b'if (0) printf ("MAKE_DBG'
for dirpath, dirnames, filenames in os.walk(root):
    for fn in filenames:
        if not fn.endswith('.c'):
            continue
        p = os.path.join(dirpath, fn)
        with open(p, 'rb') as f:
            data = f.read()
        if needle in data:
            with open(p, 'wb') as f:
                f.write(data.replace(needle, repl))
PY

echo "staged GNU make 3.82 under $DEST"
