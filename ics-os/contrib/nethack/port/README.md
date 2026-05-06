# NetHack ICS-OS port

Port shims and compatibility headers for building NetHack against the ICS-OS SDK.

Planned work:
- Replace/override platform detection for ICS-OS.
- Provide minimal libc/stdio/termcap/termios shims as needed.
- Adapt file I/O to SDK (openfile/fread/fwrite/fstat).
- Implement screen/keyboard via DEX console APIs.
