/*******************************************************************
DEX32 API (Application Programmers Interface)
This is the code that manages system calls from user mode programs (Level 3)
currently applications make sys calls using interrupt 0x30h (User Interrupt Gate) 
although a user procedure call is in the works

    DEX educational extensible operating system 1.0 Beta
    Copyright (C) 2004  Joseph Emmanuel DL Dayo

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
********************************************************************/

#include "dex32API.h"

/* Diagnostic: monotonic count of api_syscall() entries.  The selfhost
   spin watchdog (stdlib/time.c) samples this to tell a busy syscall loop
   (sbrk/read/write churn) from a pure-C spin (flat count). */
volatile unsigned long diag_sc_count = 0;

int dex32_getversion(){
   return DEX32_OSVER;
};

int api_addsystemcall(DWORD function_number, void *function_ptr, 
                        DWORD access_check, DWORD flags){

   if (function_number < API_MAXSYSCALLS){  //check if function number is less than allowed number of syscalls
      if (api_syscalltable[function_number].function_ptr == 0){  //check if the number is unused
         api_syscalltable[function_number].access_check = access_check;     
         api_syscalltable[function_number].function_ptr = function_ptr;
         api_syscalltable[function_number].flags = flags;
         return function_number;
      };
      return -1;
   };
   return -1;
};

int api_removesystemcall(DWORD function_number){
   if (function_number < API_MAXSYSCALLS){
      if (api_syscalltable[function_number].function_ptr!=0){
         api_syscalltable[function_number].access_check = 0;     
         api_syscalltable[function_number].function_ptr = 0;
         api_syscalltable[function_number].flags = 0;
         return function_number;
      };
      return -1;
   };
   return -1;
};

void api_init(){
   int i;

/******************************Initialize the API****************************/ 
   for (i=0; i<API_MAXSYSCALLS ;i++){
      api_syscalltable[i].access_check = 0;     
      api_syscalltable[i].function_ptr = 0;
   };
     
/************* Add the functions that could be used by user applications*******/
   api_addsystemcall(0, dex32_getversion,0,0);
   api_addsystemcall(1, kb_dequeue,0,0);
   api_addsystemcall(2, getprocessid,0,0);
   api_addsystemcall(3, exit,0,0);
   api_addsystemcall(4, openfilex,0,API_REQUIRE_INTS);
   api_addsystemcall(5, fclose,0,  API_REQUIRE_INTS);
   api_addsystemcall(6, putcEX,0,   API_REQUIRE_INTS);
   api_addsystemcall(7, update_cursor,0,0);
   api_addsystemcall(8, clrscr,0,0);
   api_addsystemcall(9, dex32_sbrk,0,0);
   api_addsystemcall(0xA,gettext,0,0);
   api_addsystemcall(0xB,createuthread,0,0);
   //api_addsystemcall(0xB,createthread,0,0);
   api_addsystemcall(0xC,dex32_wait,0,0);
   api_addsystemcall(0xD,textcolor,0,0);
   api_addsystemcall(0xE,textbackground,0,0);
   api_addsystemcall(0xF,sleep,0,0);
   api_addsystemcall(0x10,totalprocess,0,0);
   api_addsystemcall(0x11,getprocessinfo,0,0);
   api_addsystemcall(0x13,dex32_locktasks,0,0);
   api_addsystemcall(0x14,dex32_unlocktasks,0,0);
   api_addsystemcall(0x15,ps_user_kill,0,0);
   api_addsystemcall(0x16,getparentid,0,0);
   api_addsystemcall(0x17,ungetcx,0,0);
   api_addsystemcall(0x18,signal,0,0);
   api_addsystemcall(0x19,printf,0,0);
   api_addsystemcall(0x1a,getparentid,0,0);
   api_addsystemcall(0x1b,findprocessname,0,0);
   api_addsystemcall(0x1c,loadDLL,0,0);
     
   api_addsystemcall(30,module_getfxn,0,0);
   api_addsystemcall(31,ps_seterror,0,0);
   api_addsystemcall(32,ps_geterror,0,0);
   /*syscall # 33 free for use*/
   api_addsystemcall(34,time_getmycputime,0,0);
   api_addsystemcall(35,vfs_setbuffer,0,0);
   api_addsystemcall(36,vfs_getstat,0,0);
   api_addsystemcall(37,puttext,0,0);
   api_addsystemcall(38,textattr,0,0);
   api_addsystemcall(39,kb_keypressed,0,0);
   api_addsystemcall(49,create_semaphore,0,0);
   api_addsystemcall(0x32,get_semaphore,0,0);
   api_addsystemcall(0x33,set_semaphore,0,0);
   api_addsystemcall(0x34,free_semaphore,0,0);
   api_addsystemcall(0x35,getx,0,0);
   api_addsystemcall(0x36,gety,0,0);
   api_addsystemcall(0x37,setx,0,0);
   api_addsystemcall(0x38,sety,0,0);
   api_addsystemcall(0x39,fread,0,API_REQUIRE_INTS);
   api_addsystemcall(0x40,fgets,0,API_REQUIRE_INTS);
   api_addsystemcall(0x41,fseek,0,0);
   api_addsystemcall(0x42,chdir,0,0);
   api_addsystemcall(0x43,vfs_getcwd,0,0);
   api_addsystemcall(0x45,fwrite,0,API_REQUIRE_INTS);
   api_addsystemcall(0x46,createfile,0,API_REQUIRE_INTS);
   api_addsystemcall(0x47,ftell,0,0);
   api_addsystemcall(0x48,fdelete,0,0);
   api_addsystemcall(0x49,delfile,0,0);
   api_addsystemcall(0x4A,mkdir,0,API_REQUIRE_INTS);
   api_addsystemcall(0x4B,putchar,0,0);
   api_addsystemcall(0x50,dex32_getparametersinfo,0,0);
   api_addsystemcall(0x51,putc,0,0);
   api_addsystemcall(0x52,feof,0,0);
   api_addsystemcall(0x53,getdatetime,0,API_REQUIRE_INTS);
   api_addsystemcall(0x54,sleep,0,0);
   api_addsystemcall(0x55,time,0,0);
   api_addsystemcall(0x56,sendmessage,0,0);
   api_addsystemcall(0x57,getmessage,0,0);
   api_addsystemcall(0x58,fstat,0,0);
   api_addsystemcall(0x59,fflush,0,0);
   api_addsystemcall(0x5B,user_execp,0,0);
   api_addsystemcall(0x5C,user_exec,0,0);
   api_addsystemcall(0x5D,dex32_getfreepages,0,0);
   api_addsystemcall(0x5E,dex32_setservice,0,0);
   api_addsystemcall(0x5F,dex32vga_writepixel,0,0);
   api_addsystemcall(0x60,dex32vga_setgmode,0,0);
   api_addsystemcall(0x61,dex32vga_write_palette,0,0);
   api_addsystemcall(0x62,dex32vga_read_palette,0,0);
   api_addsystemcall(0x70,Dex32CreateDDL,0,0);
   api_addsystemcall(0x71,Dex32SetActiveDDL,0,0);
   api_addsystemcall(0x72,Dex32Clear,0,0);
   api_addsystemcall(0x73,Dex32ScrollUp,0,0);
   api_addsystemcall(0x74,Dex32SetTextColor,0,0);
   api_addsystemcall(0x75,Dex32SetTextBackground,0,0);
   api_addsystemcall(0x76,Dex32NextLn,0,0);
   api_addsystemcall(0x77,Dex32PutC,0,0);
   api_addsystemcall(0x78,Dex32PutChar,0,0);
   api_addsystemcall(0x79,Dex32FreeDDL,0,0);
   api_addsystemcall(0x7A,Dex32GetX,0,0);
   api_addsystemcall(0x7B,Dex32GetY,0,0);
   api_addsystemcall(0x7C,Dex32SetX,0,0);
   api_addsystemcall(0x7D,Dex32SetY,0,0);
   api_addsystemcall(0x7E,Dex32GetAttb,0,0);
   api_addsystemcall(0x7F,Dex32SetDDL,0,0);
   api_addsystemcall(0x80,io_setscroll,0,0);
   api_addsystemcall(0x90,user_fork,0,API_REQUIRE_INTS);
   api_addsystemcall(0x93,devmgr_ioctl,0,API_REQUIRE_INTS);
   api_addsystemcall(0x94,devmgr_finddevice,0,0);
   api_addsystemcall(0x95,console_execute,0,API_REQUIRE_INTS);
   api_addsystemcall(0x96,getprecisetime,0,API_REQUIRE_INTS);
   api_addsystemcall(0x97,fcopy,0,API_REQUIRE_INTS);
   api_addsystemcall(0x98,vfs_listdir,0,API_REQUIRE_INTS);
   api_addsystemcall(0x99,vfs_mount_device,0,API_REQUIRE_INTS);
   api_addsystemcall(0x9A,vfs_unmount_device,0,API_REQUIRE_INTS);
     //------------- recently added by jach
   api_addsystemcall(0x9B,delay,0,API_REQUIRE_INTS);
   api_addsystemcall(0x9C,kb_ready,0,0);
   api_addsystemcall(0x9D,write_text,0,0);
   api_addsystemcall(0x9E,write_char,0,0);
   api_addsystemcall(0x9F,env_getenv,0,0);
   api_addsystemcall(0xA0,env_setenv,0,0);
   api_addsystemcall(0xA1,dex32_thread_join,0,API_REQUIRE_INTS); //join a thread of the current process
   api_addsystemcall(0xA2,rename,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA3,machine_reboot,0,0);
   api_addsystemcall(0xA4,sys_read,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA5,sys_write,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA6,sys_kcmd,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA7,sys_open,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA8,sys_close,0,API_REQUIRE_INTS);
   api_addsystemcall(0xA9,sys_lseek,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAA,sys_preadv,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAB,sys_pwritev,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAC,sys_fsync,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAD,sys_io_uring_setup,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAE,sys_io_uring_enter,0,API_REQUIRE_INTS);
   api_addsystemcall(0xAF,sys_fstat_fd,0,API_REQUIRE_INTS);
   api_addsystemcall(0xB0,sys_fd_file,0,API_REQUIRE_INTS);
   api_addsystemcall(0xB1,sys_waitpid,0,API_REQUIRE_INTS);
   api_addsystemcall(0xB2,sys_spawn,0,API_REQUIRE_INTS);
   api_addsystemcall(0xB3,sys_execve,0,API_REQUIRE_INTS);
   api_addsystemcall(0xB4,sys_getdents,0,API_REQUIRE_INTS);
    api_addsystemcall(0xB5,console_puts,0,0);
   api_addsystemcall(0xB6,dex32_mmap,0,0);
    api_addsystemcall(0xB7,dex32_munmap,0,0);
    api_addsystemcall(0xC0,sys_tcgetattr,0,0);
    api_addsystemcall(0xC1,sys_tcsetattr,0,0);
    api_addsystemcall(0xC2,sys_tcflush,0,0);
    api_addsystemcall(0xC3,sys_ttyioctl,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC4,sys_ttyselect,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC5,sys_dup,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC6,sys_socket,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC7,sys_bind,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC8,sys_listen,0,API_REQUIRE_INTS);
     api_addsystemcall(0xC9,sys_accept,0,API_REQUIRE_INTS);
     api_addsystemcall(0xCA,sys_connect,0,API_REQUIRE_INTS);
     api_addsystemcall(0xCB,sys_send,0,API_REQUIRE_INTS);
     api_addsystemcall(0xCC,sys_recv,0,API_REQUIRE_INTS);
  };


api_arg_t api_syscall(api_arg_t fxn, api_arg_t val, api_arg_t val2,
                     api_arg_t val3, api_arg_t val4, api_arg_t val5){
   char temp[255];
   api_arg_t retval = 0; //the return value of a systemcall is placed here
   api_arg_t (*syscall_function)(api_arg_t p1, api_arg_t p2, api_arg_t p3,
                                  api_arg_t p4, api_arg_t p5);
    diag_sc_count++;
     //cursyscall[] is used for debugging purposes, it stores the last two different
    //system calls that was called and sets op_success if the system
    //call finished without crashing or causing a fault, false oherwise
    if (fxn!=current_process->cursyscall[1]){
      current_process->cursyscall[0]=current_process->cursyscall[1];
      current_process->cursyscall[1]=(DWORD)fxn;
   };
  
   //place a marker to indicate if a systemcall has successfully completed
   current_process->op_success=0;
  
   if (fxn >= API_MAXSYSCALLS) 
      retval = (api_arg_t)-1;
   else{ 
      if (api_syscalltable[fxn].function_ptr != 0){
          //access systemcall table and validate system call
          {
             static volatile unsigned long api_badfn_debug = 0;
             void *fnv = api_syscalltable[fxn].function_ptr;
             unsigned long fnp = (unsigned long)fnv;
             extern char textEnd[];
             unsigned long text_end = (unsigned long)textEnd;
             if ((fnp < 0x100000UL || fnp >= text_end) &&
                 api_badfn_debug < 16) {
                extern int smp_cpu_id(void);
                extern void serial_puts(const char *s);
                extern int sprintf(char *str, const char *fmt, ...);
                char ab[160];
                api_badfn_debug++;
                sprintf(ab,
                        "APIBADFN cpu=%d pid=%d fxn=0x%lx fn=0x%lx textEnd=0x%lx\n",
                        smp_cpu_id(),
                        current_process ? (int)current_process->processid : -1,
                        (unsigned long)fxn, fnp, text_end);
                serial_puts(ab);
                current_process->op_success = 0;
                retval = (api_arg_t)-1;
                return retval;
             }
          }
          syscall_function = (api_arg_t (*)(api_arg_t,api_arg_t,api_arg_t,api_arg_t,api_arg_t))
                             api_syscalltable[fxn].function_ptr;

         //This system calls require interrupts to be enabled
         if (api_syscalltable[fxn].flags&API_REQUIRE_INTS){
            DWORD flags;
            storeflags(&flags);
            startints();
         
            //make the systemcall
            retval = syscall_function(val, val2, val3, val4, val5);

            restoreflags(flags);
         }else{
            retval = syscall_function(val, val2, val3, val4, val5);
         }
      }else{
         printf("dex32_api: An unknown systemcall(0x%X) was called\n",(DWORD)fxn);
         retval = (api_arg_t)-1;
      };
    
      current_process->op_success=1;
   }
   return retval;        
};

#ifdef __x86_64__
/* Linux x86-64 syscall ABI (SYSCALL instruction) → DEX api_syscall table.
   Host-built apps use int 0x30; in-OS TinyCC may emit `syscall`. */
api_arg_t syscallentry64(api_arg_t sysno, api_arg_t a0, api_arg_t a1,
                         api_arg_t a2, api_arg_t a3, api_arg_t a4)
{
   struct k_iovec iov;
   api_arg_t r = (api_arg_t)(long)-38;
   switch ((unsigned)sysno) {
   case 0:  /* read(fd, buf, n) */
      r = api_syscall(0xA4, a0, a1, a2, 0, 0); break;
   case 1:  /* write(fd, buf, n) */
      r = api_syscall(0xA5, a0, a1, a2, 0, 0); break;
   case 2:  /* open(path, flags, mode) */
      r = api_syscall(0xA7, a0, a1, a2, 0, 0); break;
   case 3:  /* close(fd) */
       r = api_syscall(0xA8, a0, 0, 0, 0, 0); break;
    case 32: /* dup(oldfd) */
       r = api_syscall(0xC5, a0, 0, 0, 0, 0); break;
    case 4:  /* stat */
       r = api_syscall(36, a0, a1, 0, 0, 0); break;
   case 5:  /* fstat(fd, buf) */
      r = api_syscall(0xAF, a0, a1, 0, 0, 0); break;
   case 8:  /* lseek */
      r = api_syscall(0xA9, a0, a1, a2, 0, 0); break;
   case 12: /* brk */
      if (a0 == 0)
         r = (api_arg_t)(uintptr)current_process->knext;
      else {
         unsigned long cur = (unsigned long)(uintptr)current_process->knext;
         unsigned long want = (unsigned long)a0;
         if (want > cur)
            api_syscall(9, (api_arg_t)(want - cur), 0, 0, 0, 0);
         r = (api_arg_t)(uintptr)current_process->knext;
      }
      break;
   case 17: /* pread64 */
      iov.iov_base = (void *)(uintptr)a1;
      iov.iov_len = (unsigned long)a2;
      r = (api_arg_t)sys_preadv((int)a0, &iov, 1, (long)a3);
      break;
   case 18: /* pwrite64 */
      iov.iov_base = (void *)(uintptr)a1;
      iov.iov_len = (unsigned long)a2;
      r = (api_arg_t)sys_pwritev((int)a0, &iov, 1, (long)a3);
      break;
   case 19: /* readv — treat as preadv at current offset */
      {
         long off = sys_lseek((int)a0, 0, 1);
         long n = sys_preadv((int)a0, (const struct k_iovec *)(uintptr)a1,
                             (int)a2, off);
         if (n > 0)
            sys_lseek((int)a0, off + n, 0);
         r = (api_arg_t)n;
      }
      break;
   case 20: /* writev */
      {
         long off = sys_lseek((int)a0, 0, 1);
         long n = sys_pwritev((int)a0, (const struct k_iovec *)(uintptr)a1,
                              (int)a2, off);
         if (n > 0)
            sys_lseek((int)a0, off + n, 0);
         r = (api_arg_t)n;
      }
      break;
   case 39: /* getpid */
      r = api_syscall(2, 0, 0, 0, 0, 0); break;
   case 60: /* exit */
      r = api_syscall(3, a0, 0, 0, 0, 0); break;
   case 74: /* fsync */
      r = api_syscall(0xAC, a0, 0, 0, 0, 0); break;
   case 79: /* getcwd */
      r = api_syscall(0x43, a0, a1, 0, 0, 0); break;
   case 201: /* time */
      r = api_syscall(0x55, a0, 0, 0, 0, 0); break;
   case 257: /* openat(dirfd, path, flags, mode) */
      (void)a0;
      r = api_syscall(0xA7, a1, a2, a3, 0, 0); break;
   case 295: /* preadv */
      r = api_syscall(0xAA, a0, a1, a2, a3, 0); break;
   case 296: /* pwritev */
      r = api_syscall(0xAB, a0, a1, a2, a3, 0); break;
   case 425: /* io_uring_setup */
      r = api_syscall(0xAD, a0, a1, 0, 0, 0); break;
   case 426: /* io_uring_enter */
      r = api_syscall(0xAE, a0, a1, a2, a3, 0); break;
   default:
      (void)a4;
      r = (api_arg_t)(long)-38; /* -ENOSYS */
      break;
   }
   return r;
}
#endif      

