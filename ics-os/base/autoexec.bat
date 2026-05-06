@echo off
@echo Loading Microsoft C runtime library...
loadmod /lib1/msvcrt.dll
@echo Initializing RAM Disk...
loadmod /lib1/ramdisk.dll -blocks 1000
mount fat ramdisk /ramdisk
rem copy /apps/ed.exe /ramdisk
pcut rd: /ramdisk/
cls
cd /
@echo 
@echo Welcome to the ICS Operating System
echo
@echo Institute of Computer Science
@echo University of the Philippines, Los Banos
@echo
@echo Type "help" on the command prompt to
@echo display available commands.
@echo 

