# vi editor port notes (ICS-OS)

This directory prepares a port of a vi-compatible editor for ICS-OS. The plan is to use the BusyBox `vi` applet source because it is compact and GPL-2.0 licensed (GPL-compatible with the project). We will vendor the source in-tree and adapt it to the ICS-OS SDK stdlib.

## What we can ship in-tree
- BusyBox source code (GPL-2.0), including LICENSE and attribution.
- Port shims in contrib/vi/port and build glue (Makefile) using the ICS-OS SDK.

## What we cannot ship in-tree
- Proprietary vim runtime files or non‑GPL compatible vi implementations.

## Expected layout
```
contrib/vi/
  busybox/        # third-party source (to be downloaded)
  port/           # ICS-OS shims
  Makefile        # builds a vi.exe from the applet sources
```

## Build steps
1. Download BusyBox source:

```
./download-busybox.sh
```

2. Build the vi applet:

```
make
```

3. Install to the apps folder:

```
make install
```

## License
BusyBox is GPL-2.0. Ensure the BusyBox LICENSE file remains in contrib/vi/busybox/.
