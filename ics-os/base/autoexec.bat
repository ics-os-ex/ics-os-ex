@echo off
rem msvcrt.dll + ramdisk.dll are obsolete/legacy: the kernel now provides
rem /ramdisk natively and the in-OS toolchain is ELF64 (no PE runtime).
rem copy /icsos/apps/ed.exe /ramdisk
pcut rd: /ramdisk/
cls
cd icsos
set PATH=/icsos/apps
set SDK_HOME=/icsos/tcc1
@echo 
@echo Welcome to the ICS Operating System
echo
@echo Institute of Computer Science
@echo University of the Philippines, Los Banos
@echo
@echo Type "help" on the command prompt to
@echo display available commands.
@echo 

