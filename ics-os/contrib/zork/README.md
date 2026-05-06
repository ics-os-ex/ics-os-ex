# Zork (Z-Machine) Port Notes

This directory is reserved for a Z-Machine interpreter port (e.g., a GPL-compatible interpreter such as Frotz) and the build rules to produce a native ICS-OS executable.

## What we can ship in-tree
- Interpreter source code under a GPL-compatible license (must include LICENSE and source attribution).
- Build glue (Makefile, small platform shims) that uses only the ICS-OS SDK stdlib.

## What we cannot ship in-tree
- The Zork story data files (e.g., ZORK1.DAT). These are copyrighted. You must obtain them legally and copy them into the OS image yourself.

## Expected layout
```
contrib/zork/
  interpreter/   # third-party interpreter source (GPL-compatible)
  port/          # ICS-OS shims (stdio/console hooks, file access)
  Makefile       # build to a native .exe
```

## Next steps
1. Download a GPL-compatible interpreter source (Frotz is GPL-2.0) using:

```
./download-frotz.sh
```

This fetches from the official GitLab mirror: https://gitlab.com/DavidGriffith/frotz

2. Download open story files (not Zork) using:

```
./download-stories.sh
```

This pulls a small set of openly licensed or freely redistributable story files into contrib/zork/stories/ and records sources/licenses in contrib/zork/stories/README.md.

3. We will add the ICS-OS port layer under contrib/zork/port/.
4. Build the interpreter into apps/zork.exe and place the story file on the boot image.

If you already have a preferred interpreter source, drop it into contrib/zork/interpreter/ and let me know so I can wire up the port and build rules.
