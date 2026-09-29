/*
  Name: dex low-level memory management library
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 02/03/04 18:06
  Description: This module handles everything that has to do
  with memory, except the high-level memory functions like malloc....
  
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
*/


#ifndef _DEXMEM_H_
#define _DEXMEM_H_

/*This constants define the selector values used by DEX */
#include "types.h"
#include "memory/memlayout.h"
#include "memory/vma.h"

/* Forward decl so this header is self-contained when a TU (e.g. scheduler.c)
   includes it without startup/multiboot.h.  `mmap` is the typedef for
   struct _mmap defined in startup/multiboot.h. */
struct _mmap;

#ifdef __x86_64__
#define LINEAR_SEL     0x10
#define SYS_CODE_SEL   0x08
#define SYS_DATA_SEL   0x10
#define SYS_STACK_SEL  0x10
#define SYS_TSS        0x28
#define SCHED_TSS      0x28
#define USER_CODE      (0x18+3)
#define USER_DATA      (0x20+3)
#define USER_STACK     (0x20+3)
#define USER_TSS       0x28
#define SYS_SCHED_SEL  0x08
#define SYS_ERROR_TSS  0x28
#define PF_TSS         0x28
#define APM_CS32       0x08
#define APM_CS16       0x08
#define APM_DS         0x10
#define DEX_SYSCALL    0x08
#define KEYB_TSS       0x28
#define MOUSE_TSS      0x28
#define MACHINE_X86_64
#else
#define LINEAR_SEL 8
#define SYS_CODE_SEL 56
#define SYS_DATA_SEL 0x20
#define SYS_STACK_SEL 0x18
#define SYS_TSS   0x80
#define SCHED_TSS 0x78
#define USER_CODE 0x88+3
#define USER_DATA 0x90+3
#define USER_STACK 0x98+3
#define USER_TSS 0xA0+3
#define SYS_SCHED_SEL 0xA8
#define SYS_ERROR_TSS 0xB0
#define PF_TSS 0xB8
#define APM_CS32 0xC0
#define APM_CS16  0xC8
#define APM_DS 0xD0
#define DEX_SYSCALL 0xD8
#define KEYB_TSS    0xE0
#define MOUSE_TSS    0xE8
#define MACHINE_INTEL386
#endif

//******defines known page attributes********
#define PG_PRESENT 1
#define PG_WR 2
#define PG_USER 4
#define PG_WRITETHROUGH 8
#define PG_PCD 16
#define PG_DIRTY  64
#define PG_PAGESIZE 0x80
#define PG_DEMANDLOAD 0x200
#define PG_COPYWRITE 0x400


//this function returns the total available memory detected by startup.asm
DWORD memamount;

//this structure defines an entry in the gdt table
typedef struct __attribute__((packed)) _GDTentry
   {
      WORD limit;
      WORD lowaddr;
      BYTE lowaddr2;
      BYTE att1,att2;
      BYTE highaddr;
   } gdtentry;


//this tructure defines an 80386/486/Pentium call gate
typedef struct __attribute__((packed)) _CALLGATE
  {
    WORD lowoffset;
    WORD selector;
    BYTE attb1;
    BYTE attb2;
    WORD highoffset;
  } CALLGATE;

//this structure defines an entry in the IDT table
#ifdef __x86_64__
typedef struct __attribute__((packed)) _IDTentry {
    WORD lowphy;
    WORD selector;
    BYTE ist;
    BYTE attr;
    WORD midphy;
    DWORD highphy;
    DWORD reserved;
} idtentry;
#else
typedef struct _IDTentry {
    WORD lowphy ;
    WORD selector;
    BYTE reserved;
    BYTE attr;
    WORD highphy;
} idtentry;
#endif

DWORD totalgdtentries=10;

/*====================The DEX Memory Map==========================================*/

#ifdef __x86_64__
/* Identity-mapped long mode. Values come from memory/memlayout.h. */
char *kbaseheap=(char*)MEM_KHEAP_BASE;
char *kmodeproc=(char*)MEM_KMODE_BASE,
     *kmodeproc_next=(char*)MEM_KMODE_BASE;
char *lmodeproc=(char*)MEM_LMODE_BASE,
     *lmodeproc_next=(char*)MEM_LMODE_BASE;
char *knext=          (char*)MEM_KHEAP_BASE;
char *userstackloc=   (char*)MEM_USER_STACK;
char *userheap=       (char*)MEM_USER_HEAP;
char *syscallstack=   (char*)MEM_SYSCALL_STACK;
char *linux_userspace=(char*)MEM_LINUX_USER_BASE;
char *sharedmemloc=   (char*)MEM_SHARED_BASE;
char *userspace=      (char*)MEM_USER_ELF_BASE;
#else
char *kbaseheap=(char*)0xC0000000;          //marks the location of the kernel heap
char *kmodeproc=(char*)0xD0000000,
     *kmodeproc_next=(char*)0xD0000000;     //marks the location of the kernel mode
                                            //process address space
                                            
char *lmodeproc=(char*)0xE0000000,          //marks the location of the shared library / driver 
     *lmodeproc_next= (char*)0xE0000000;    //address space
char *knext=          (char*)0xC0000000;    //marks the location of the top of the kernel heap

char *userstackloc=   (char*)0xB0000000;    //marks the location of the user stack
char *userheap=       (char*)0xA0000000;    //marks the base of the user heap
char *syscallstack=   (char*)0x90000000;    //marks the base of the system call stack
char *linux_userspace=(char*)0x80000000;    //marks the base where linux executables like to go
char *sharedmemloc=   (char*)0x70000000;    //marks the location of the user shared memory area
char *userspace=      (char*)0x00400000;    //marks the location of the user base address
#endif
DWORD *stackbase=    0;                     /* set in mem_detectmemory() */
DWORD *kernelbase=   (DWORD*)MEM_KERNEL_LOAD;
idtentry *dex_idtbase=(idtentry*)MEM_IDT;
gdtentry *dex_gdtbase=(gdtentry*)MEM_GDT;
/*=================================================================================*/

/*Stores the total number of pages and memory respectively*/                
DWORD totalpages=0;
DWORD totalmemory=0;

//holds physical location of the kernel page directory
DWORD *pagedir1;


//prototypes for page management
extern void enablepaging();
extern void disablepaging();
extern void switchuserprocess(void);
extern inline void storeflags(DWORD *flags);
extern inline void restoreflags(DWORD flags);
extern void setpagedir(DWORD *dir);
inline void startints();
inline void stopints();

/* Global physical frame allocator (x86-64). Every 4KiB frame — kernel and
   user — is handed out by frame_alloc() and returned by frame_release().
   See the implementation and Phase-1/Phase-2 notes in memory/dexmem.c. */
extern u64 frame_alloc(void);
extern int frame_retain(u64 phys);
extern unsigned frame_refcount(u64 phys);
extern void frame_release(u64 phys);
extern u64 frame_free_count(void);
extern u64 frame_total_count(void);
extern void frame_diag_report(const char *tag, u64 phys);
extern unsigned long freed_pml4_count(void);
extern int freed_pml4_contains(u64 pml4);
#ifdef __x86_64__
extern void gpf_probe_store(unsigned long va, unsigned long cr3, unsigned long rip);
#endif

/*=================================Prototype definitions here==============================*/

WORD addgdt(DWORD base,DWORD limit,BYTE attb1,BYTE attb2);
void clearpagetable(DWORD *pagetable);
void *commit(DWORD virtualaddr,DWORD pages);
void *commitb(DWORD virtualaddr,int amt,DWORD *pagecount);
DWORD mem_detectmemory(struct _mmap *grub_meminfo , int size );
void dex32copyblock(DWORD vdest,DWORD vsource,DWORD pages,DWORD *pagedir);
void *dex32_commitblock(DWORD virtualaddr,int amt,
    DWORD *pagecount,DWORD *pagedir,DWORD attb);
void *dex32_commit(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD pattb);
void dex32_copy_on_write(DWORD *directory);
void dex32_copy_pagedir(DWORD *destdir,DWORD *source);
void dex32_copy_pagedirU(DWORD *destdir,DWORD *source);
int dex32_copy_pg(DWORD *destdir, DWORD *source);
void dex32_freeuserpagetable(DWORD *pgd);
DWORD dex32_getfreepages();
void *dex32_reserveblock(DWORD virtualaddr,int amt,
    DWORD *pagecount,DWORD *pagedir,DWORD attb);
void *dex32_reserve(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD attb);
void *dex32_sbrk(unsigned int amt);
/* mmap/munmap/mprotect/msync syscall handlers.  They take the DEX int 0x30
   ABI (five pointer-width api_arg_t values); mmap receives a pointer to a
   struct mmap_args in a0 and returns the mapped base (success) or a negative
   errno (failure) as api_arg_t. */
void *dex32_mmap(unsigned long length, unsigned long flags,
                 unsigned long a3, unsigned long a4, unsigned long a5);
int dex32_munmap(unsigned long addr, unsigned long length,
                 unsigned long a3, unsigned long a4, unsigned long a5);
void freelinearloc(void *linearmemory,DWORD *pagedir);
void freemultiple(void *linearmemory,DWORD *pagedir,DWORD pages);
void freeuserheap(DWORD *pagedir);
DWORD getlinearloc(void *linearmemory,DWORD *pagedir);
DWORD getmultiple(void *linearmemory,DWORD *pagedir,DWORD pages);
DWORD getpagetablephys(DWORD vaddr,DWORD *pagedir);
DWORD getphys(DWORD vaddr,DWORD *pagedir);
DWORD getphys64(unsigned long long vaddr, DWORD *pagedir);
DWORD getvirtaddress(DWORD physicaladdr);
DWORD getvirtaddress2(DWORD physicaladdr,DWORD hdl);
void maplineartophysical(unsigned int *pagedir,unsigned int linearaddr,
      unsigned int physical,unsigned int attribute);
int  maplineartophysical2(unsigned int *pagedir,unsigned int linearaddr,
      unsigned int physical,unsigned int attribute);      
DWORD xmaplineartophysical(const DWORD linearmemory,const DWORD physicalmemory,
   DWORD *pagedir,const DWORD attb);
void mem_init();
void dex32_restore_identity_map(void);
int mmio_mark_uncacheable(u64 phys, u64 len);
void *mmio_map(u64 phys, u64 len);
void mmio_reapply_uncacheable(void);
#ifdef __x86_64__
/* Per-process user page directory (see dexmem.c). */
extern u64 *userpd_create(void);
int userpd_map_region(u64 *pml4, unsigned long long base,
                      unsigned long long size, unsigned long attb);
u64 *userpd_map_page(u64 *pml4, unsigned long long vaddr, unsigned long attb);
int userpd_map_identity_page(u64 *pml4, unsigned long long vaddr, unsigned long attb);
int userpd_unmap_page(u64 *pml4, unsigned long long vaddr);
void *userpd_resolve(u64 *pml4, unsigned long long vaddr);
u64 *userpd_clone_eager(const u64 *parent);
u64 *userpd_clone_cow(u64 *parent, unsigned long long private_vaddr);
int userpd_handle_cow(u64 *pml4, unsigned long long vaddr,
                      unsigned fault_info);
void userpd_cow_fail_next(void);
void userpd_cow_stats(u64 *faults, u64 *copies, u64 *fastpaths,
                      u64 *shootdowns, u64 *oom);
void userpd_free(u64 *pml4);
int userpd_is_private(const void *pml4);
int userpd_used(void);

/* The VMA type (vm_area), VMA_MAX and the mmap_args block live in memory/vma.h
   (included above) so process.h can store the VMA table without pulling this
   header-resident global-state file into every TU. */

struct _PCB386;

/* Find the VMA containing va, or NULL. */
vm_area *vm_area_find(const struct _PCB386 *p, unsigned long va);
/* Reserve a VA range and record a VMA.  Returns the start address or 0.
   When `fixed` is set the map is placed at `hint` (MAP_FIXED, replacing any
   existing mapping there); otherwise the anonymous bump allocator
   (mmap_brk) chooses the address and `hint` is ignored.  `prot`/`flags` are
   the raw mmap() values; `file`/`foff` back a file map (NULL = anonymous). */
unsigned long vm_area_alloc(struct _PCB386 *p, unsigned long length,
                            unsigned long prot, unsigned long flags,
                            void *file, unsigned long foff,
                            unsigned long hint, int fixed);
/* Remove VMAs overlapping [start,end): unmap pages, flush dirty shared
   pages to their files, and drop the file refs.  Returns 0 on success. */
int vm_area_remove(struct _PCB386 *p, unsigned long start, unsigned long end);
/* Change the protection of the present pages in [start,end).  Returns 0 on
   success, -1 if the range is not fully covered by VMAs of this process. */
int vm_area_set_prot(struct _PCB386 *p, unsigned long start, unsigned long end,
                     unsigned long prot);
/* Handle a not-present fault at va inside one of the process's VMAs: load
   the backing page (file data or zero), map it with the VMA's protection.
   Returns 1 handled, 0 if va is not inside a VMA (caller keeps its policy),
   -1 on out-of-memory. */
int vm_area_fault(struct _PCB386 *p, unsigned long va, unsigned fault_info);
/* Copy the VMA table from parent to child at fork, taking a fresh file ref
   for the child on each file-backed VMA.  Returns 0 on success. */
int vm_area_fork(const struct _PCB386 *parent, struct _PCB386 *child);
/* Flush every dirty MAP_SHARED page of the process to its file and drop all
   VMA file refs.  Called at process exit before its PML4 is freed. */
void vm_area_exit(struct _PCB386 *p);
/* Flush the dirty MAP_SHARED pages of one VMA in [start,end) to its file.
   `msync` syscall entry: returns 0 on success, -1 if the range is not a
   mapped shared region. */
int vm_area_msync(struct _PCB386 *p, unsigned long start, unsigned long end);
#endif

/* mmap-adjacent syscall handlers (registered in dexapi/dex32API.c).  All take
   the five-slot api_arg_t ABI and return the mapped address / 0 on success or
   a negative errno as api_arg_t on failure. */
void *dex32_mprotect(unsigned long addr, unsigned long length,
                     unsigned long prot, unsigned long a3, unsigned long a4);
void *dex32_msync(unsigned long addr, unsigned long length,
                  unsigned long flags, unsigned long a3, unsigned long a4);
DWORD *mempop();
void mempush(DWORD mem);
DWORD obtainpage();
extern void refreshpages();
void setattb(WORD sel,BYTE attb1);
void setinterruptvector(DWORD index,idtentry *t,unsigned char attr,
     void (*handler)(int irq), WORD sel);
void dex32_setbase(WORD sel,DWORD addr);
void *sbrk(int amt);
void setcallgate(DWORD sel,DWORD funcsel,void *entry,BYTE params,BYTE access);
void setgdt(WORD sel,DWORD base,DWORD limit,BYTE attb1,BYTE attb2);
void setgdtentry(DWORD index,void *base,gdtentry *t,DWORD limit,
           BYTE attb1,BYTE attb2);

void mem_interpretmemory(struct _mmap *map,int size);
void dex32_stopints(DWORD *flags);
void dex32_restoreints(DWORD flags);
void setpageattb(DWORD *pagedir,DWORD vaddr,DWORD attb);
void *dex32_setpageattb(DWORD virtualaddr,DWORD pages,DWORD *pagedir,DWORD pattb);
void *dex32_setpageattbblock(DWORD virtualaddr,int amt,DWORD *pagecount,DWORD *pagedir,DWORD attb);

#endif /* _DEXMEM_H_ */
