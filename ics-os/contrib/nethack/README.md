# NetHack port notes (ICS-OS)

This directory prepares a port of NetHack to ICS-OS using the SDK stdlib only. Source is GPL-compatible and will be vendored here via the download script.

## What we can ship in-tree
- NetHack source code (NetHack General Public License)
- Port shims in contrib/nethack/port and build glue (Makefile) using the ICS-OS SDK

## Expected layout
```
contrib/nethack/
  nethack/       # third-party source (downloaded)
  port/          # ICS-OS shims
  Makefile       # builds a nethack.exe for ICS-OS
```

## Build steps
1. Download NetHack source:

```
./download-nethack.sh
```

2. Build:

```
make
```

3. Package data (nhdat):

```
make data
```

4. Install:

```
make install
```
