@echo off
' Dist/etcher autoexec: no USB file copies at boot. On Intel ADL-N, MSC
' reads race the CDC hotplug pump and hang forever in fcopy. SDK .o files
' stay on /icsos/apps; gcc.exe links them from there.
@echo
@echo  ICS-OS Distribution
@echo    Compilers : gcc  cc1  as  ld  ar  objcopy  make  tcc  nasm
@echo    Editors   : vim  ed
@echo    Tools     : cp  rm  mkdir  pak  lzozip  hxdmp  sh
@echo    SDK       : /icsos/tcc1 (headers+libc)   /icsos/include (POSIX)
@echo    Runtime   : /icsos/apps/*.o (no boot-time ramdisk seed)
@echo
@echo  Try:  cd apps
@echo        gcc hello.c -o hello
@echo        hello
@echo
@echo  Type "help" for the command list.  Type "sh" for the POSIX shell.
@echo  Large apps (vim ~2MiB) load from USB; Ctrl-C aborts a hung exec,
@echo  F4 force-kills the foreground, C-b c opens a fresh kernel prompt.
@echo
pcut rd: /ramdisk/
cd icsos
set PATH=/icsos/apps
set SDK_HOME=/icsos/tcc1
@echo PATH=/icsos/apps  SDK_HOME=/icsos/tcc1
@echo
