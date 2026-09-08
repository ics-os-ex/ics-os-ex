@echo off
rem msvcrt.dll + ramdisk.dll are obsolete/legacy: the kernel now provides
rem /ramdisk natively and the in-OS toolchain is ELF64 (no PE runtime).
copy /icsos/apps/crt1.o /ramdisk/crt1.o
copy /icsos/apps/tccsdk.o /ramdisk/tccsdk.o
copy /icsos/apps/libtcc1.o /ramdisk/libtcc1.o
copy /icsos/apps/posix.o /ramdisk/posix.o
copy /icsos/apps/setjmp.o /ramdisk/setjmp.o
pcut rd: /ramdisk/
cls
cd icsos
set PATH=/icsos/apps
set SDK_HOME=/icsos/tcc1
@echo
@echo  ICS-OS Distribution
@echo    Compilers : gcc  cc1  as  ld  ar  objcopy  make  tcc  nasm
@echo    Editors   : vim  ed
@echo    Tools     : cp  rm  mkdir  pak  lzozip  hxdmp  sh
@echo    SDK       : /icsos/tcc1 (headers+libc)   /icsos/include (POSIX)
@echo
@echo  Try:  cd apps
@echo        gcc hello.c -o hello
@echo        hello
@echo
@echo  Type "help" for the command list.
@echo
