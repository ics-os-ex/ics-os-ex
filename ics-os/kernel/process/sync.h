/*
  Name: sync.c
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 18/01/04 06:27
  Description: Provides kernel synchornization functions
*/
#ifndef SYNC_H
#define SYNC_H

//a synchronized shared variable for 
//synchronization
typedef struct _sync_sharedvar {
  volatile int busy;
   int ready;
   int wait;
   /* One-shot latch for the CRITHANG owner report (sync.c). */
   volatile int diag_reported;
} sync_sharedvar;

void sync_entercrit(sync_sharedvar *var);
void sync_leavecrit(sync_sharedvar *var);
unsigned long sync_entercrit_irqsave(sync_sharedvar *var);
void sync_leavecrit_irqrestore(sync_sharedvar *var,unsigned long flags);
void sync_release_process_crits(void *pcb, int owner);
int  sync_cpu_holds(const sync_sharedvar *var);

extern sync_sharedvar elf_map_crit;

#endif
