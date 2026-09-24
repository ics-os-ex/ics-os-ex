/* 
   ==========================================================================
   Console.c
   Author: Joseph Emmanuel Dayo
   Date updated:December 6, 2002
   Description: A kernel mode console that is used for debugging the kernel
   and testing new kernel features.
                
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
   ==========================================================================
*/

#include "console.h"
#include "klog.h"
#include "../process/sync.h"
#include "../net/wifi.h"

/* COM2 shell (shell2) line-discipline echo flag. Telnet clients typically
   perform local echo themselves, so echo defaults ON but can be disabled with
   'stty noecho' to avoid double-echoing. See shell2_main() below. */
static volatile int shell2_echo = 1;

static int cc1_window_contains(const char *hay, int hlen, const char *needle)
{
   int nlen = (int)strlen(needle);
   int i;
   if (hlen < nlen)
      return 0;
   for (i = 0; i + nlen <= hlen; i++) {
      if (hay[i] == needle[0] && memcmp(hay + i, needle, (size_t)nlen) == 0)
         return 1;
   }
   return 0;
}

void runner(){
   int i=0;
   while(1){
      i++;
      i--;
   }
   //printf("Hello user thread!\n");
}

  
/*A console mode get string function terminates
upon receving \r */
void getstring(char *buf, DEX32_DDL_INFO *dev){
   unsigned int i=0;
   char c;
   if (current_process && current_process->ctty) {
      int n = tty_read(current_process->ctty, buf, 255);
      if (n < 0) {
         buf[0] = 0;
         return;
      }
      if (n > 0 && buf[n-1] == '\n')
         n--;
      buf[n] = 0;
      return;
   }
   do{
      c=getch();
      if (c=='\r' || c=='\n' || c==0xa) 
         break;

      if (c=='\b' || (unsigned char)c == 145){
         if(i>0){
            i--;
            if (Dex32GetX(dev)==0){
               Dex32SetX(dev,79);
               if (Dex32GetY(dev)>0) 
                  Dex32SetY(dev,Dex32GetY(dev)-1);
            }else{
               Dex32SetX(dev,Dex32GetX(dev)-1);
            }     
            Dex32PutChar(dev,Dex32GetX(dev),Dex32GetY(dev),' ',Dex32GetAttb(dev));
         };
      }else{
         if (i<256){  //maximum command line is only 255 characters
            Dex32PutChar(dev,Dex32GetX(dev),Dex32GetY(dev),buf[i]=c,Dex32GetAttb(dev));
            i++;
            Dex32SetX(dev,Dex32GetX(dev)+1);     
            if (Dex32GetX(dev)>79){
               Dex32SetX(dev,0);
               Dex32NextLn(dev);
            };
         };
      };

      Dex32PutChar(dev,Dex32GetX(dev),Dex32GetY(dev),' ',Dex32GetAttb(dev));
      update_cursor(Dex32GetY(dev),Dex32GetX(dev));
   }while (c!='\r');
    
   Dex32SetX(dev,0);
   Dex32NextLn(dev);
   buf[i]=0;
};

/*Show information about memory usage. This function is also useful
  for detecting memory leaks*/
void meminfo(){
#ifdef __x86_64__
   u64 total = frame_total_count();
   u64 free  = frame_free_count();
   u64 used  = total - free;
   printf("=================Memory Information===============\n");
   printf("Pages available     : %10llu pages\n", (unsigned long long)free);
   printf("Total Pages         : %10llu pages\n", (unsigned long long)total);
   printf("Total Memory        : %10llu KB\n", (unsigned long long)(total * 4));
   printf("Free Memory         : %10llu KB\n", (unsigned long long)(free * 4));
   printf("Used pages          : %10llu pages (%10llu KB)\n",
          (unsigned long long)used, (unsigned long long)(used * 4));
#else
   DWORD totalbytes = totalpages * 0x1000;
   DWORD freebytes = totalbytes - (totalpages - stackbase[0])* 0x1000;
   printf("=================Memory Information===============\n");
   printf("Pages available     : %10u pages\n",stackbase[0]);
   printf("Total Pages         : %10d pages\n",totalpages);
   printf("Total Memory        : %10u bytes (%10d KB)\n",totalbytes, totalbytes / 1024);
   printf("Free Memory         : %10u bytes (%10d KB)\n",freebytes, freebytes / 1024);
   printf("Used pages          : %10d pages (%10d KB)\n",totalpages-stackbase[0],
   (totalpages-stackbase[0])*0x1000);
#endif
};


int delfile(char *fname){
   int sectors;
   file_PCB *f=openfilex(fname,FILE_WRITE);
   return fdelete(f);
};


/*
 * Forks a new process
 */
int user_fork(){
   int curval = current_process->processid;

   int childready = 0, retval = 0;
   int hdl;
   int id;
   DWORD flags;

   #ifdef DEBUG_FORK
   printf("user_fork called\n");
   #endif
    
   //enable interrupts since we want the process dispatcher to take control
   storeflags(&flags);
   startints();
   
   //Calls pd_forkmodule() from kernel/process/pdispatch.c 
   hdl = pd_forkmodule(current_process->processid);

   //inform the CPU scheduler, hopefully to schedule process_dispatcher() 
   taskswitch();  

   //id = pd_ok(hdl); 
   id = pd_dispatched(hdl);
   while (!(id = pd_dispatched(hdl))){ //wait for the process to be dispatched before returning
      taskswitch(); //try to wakeup process_dispatcher() 
      ; 
   }

   if (curval != current_process->processid){ //this is the child
      //If this is the child process, the processid when this function
      //was called is not equal to the current processid.
      //pd_ok(hdl);
      retval = 0;
   };
      
   if (curval == current_process->processid){ // this is the parent
      pd_ok(hdl);          //free the createp_queue node used by the child
      retval = id;
   };
      
   restoreflags(flags);
   return retval;
};


#include "selfhost.c"  /* GCC kbuild + optional TinyCC bootstrap drivers */

/**
 * Function that reads an executable and creates a new process for it.
 * ELF64 uses the same VFS stream path as posix_spawn (no whole-file heap
 * map).  mmap+dex32_loader remains for PE/COFF/B32 leftovers.
 */
static DWORD execp_load(char *fname, DWORD mode, char *params)
{
    DWORD id, size;
    char *buf;
    char temp[255];

    id = (DWORD)elf64_stream_load(fname, mode, params, showpath(temp),
                                  current_process);
    if (id && (int)id != -1)
       return id;

    sync_entercrit(&elf_map_crit);
    buf = (char*)vfs_mapfile(fname, &size);
    if (!buf){
       sync_leavecrit(&elf_map_crit);
       return 0;
    }

    printf("execp: loading %s (%u bytes) [mmap]\n", fname, (unsigned)size);
    id = dex32_loader(fname, buf, userspace, mode, params,
                      showpath(temp), current_process);
    free(buf);
    sync_leavecrit(&elf_map_crit);
    if (!id || (int)id == -1)
       return 0;
    return id;
}

int user_execp(char *fname, DWORD mode, char *params){
    DWORD id;
    char alt[260];
    int n = 0, has_dot = 0;

    if (!fname || !fname[0])
       return 0;

    /* Pause CDC before printf/open — CDC OUT holding usb_io_lock blocked MSC. */
    usb_cdc_bulk_io_begin();
    printf("execp: loading %s (Ctrl-C abort, F4 kill fg)\n", fname);
    id = execp_load(fname, mode, params);
    if (!id) {
       while (fname[n] && n < 250) {
          if (fname[n] == '.')
             has_dot = 1;
          n++;
       }
       if (!has_dot && n > 0 && n < 250) {
          memcpy(alt, fname, (unsigned)n);
          memcpy(alt + n, ".exe", 5);
          printf("execp: loading %s (Ctrl-C abort, F4 kill fg)\n", alt);
          id = execp_load(alt, mode, params);
       }
    }
    usb_cdc_bulk_io_end();
    if (!id){
       printf("execp: failed to start %s\n", fname);
       return 0;
    }

    printf("execp: started pid=%d, waiting\n", (int)id);
    fg_setmykeyboard(id);
    {
       int child_status = 0;
       long wr;
       dex32_child_faulted = 0;
       printf("execp: waitpid enter pid=%d\n", (int)id);
       /* Poll with WNOHANG so Ctrl-C can abort. Do NOT call delay():
          delay() spins until ticks advance, and a stalled tick source
          (seen on some no-COM1 laptop boots) freezes every exec forever
          — hello.exe included. WNOHANG already taskswitches once. */
       for (;;) {
          wr = sys_waitpid((int)id, &child_status, 1 /* WNOHANG */);
          if (wr == (long)id)
             break;
          if (wr < 0) {
             child_status = 1;
             break;
          }
          if (current_process && current_process->pending_sig == SIGINT) {
             current_process->pending_sig = 0;
             printf("\nexecp: Ctrl-C — killing pid=%d\n", (int)id);
             kill_process(id);
             wr = sys_waitpid((int)id, &child_status, 0);
             if (wr != (long)id)
                child_status = 1;
             break;
          }
       }
       printf("execp: waitpid exit pid=%d status=%d\n", (int)id,
              child_status);
       dex32_child_faulted = child_status != 0;

       fg_setmykeyboard(getprocessid());
       if (dex32_child_faulted) {
          printf("execp: child faulted\n");
          return 0;
       }
       return id;
    }
}

int exec(char *fname, DWORD mode, char *params){
   DWORD id;
   char *buf;
   file_PCB *f=openfilex(fname,0);

   if (f!=0){
      DWORD size;
      char temp[255];
      vfs_stat fileinfo;
      fstat(f,&fileinfo);
      size = fileinfo.st_size;
     
      buf=(char*)malloc(size+511);
     
      if (fread(buf,size,1,f) == size)
         id=dex32_loader(fname, buf, userspace, mode, params, showpath(temp), getprocessid());
      
      free(buf);
      fclose(f);
      return id;
   };
   return 0;
;};

int user_exec(char *fname,DWORD mode,char *params){
   DWORD id;
   char *buf;
   file_PCB *f = openfilex(fname, FILE_READ);

   if (f != 0){
      int hdl,size;
      char temp[255];
      vfs_stat fileinfo;
      fstat(f, &fileinfo);
      size = fileinfo.st_size;
      buf=(char*)malloc(size+511);
      fread(buf, size, 1, f);
      hdl = addmodule(fname, buf, userspace, mode, params, showpath(temp), getprocessid());
      while (!pd_ok(hdl)) 
         ;
     
      free(buf);
      fclose(f);
      return id;
  };
  return 0;
};


int loadDLL(char *name, char *p){
   file_PCB *handle;
   int fsize; vfs_stat filestat;
   int hdl,libid;
   char *buf;

   handle=openfilex(name,FILE_READ);
   if (!handle) 
      return -1;
   fstat(handle,&filestat);
   vfs_setbuffer(handle,0,filestat.st_size,FILE_IOFBF);
   //get filesize and allocate memory
   fsize= filestat.st_size;
   buf=(char*)malloc(fsize);
 
   //load the file from the disk 
   fread(buf, fsize, 1, handle);
 
   /*Tell the process dispatcher to map the file into memory and
      create data structures necessary for managing dynamic libraries*/
   hdl = addmodule(name, buf, lmodeproc, PE_USERDLL, p, 0, getprocessid());
 
   //wait until the library has been loaded before we continue since addmodule returns immediately
   while (!(libid = pd_ok(hdl)))
      ;
 
   //done!
   free(buf);

   //close the file
   fclose(handle);
   return libid;
};

void loadfile(char *s,int,int);

void loadlib(char *s){
   char *buf;
   DWORD size;
   loadDLL(s,0);
};


int console_showfile(char *s, int wait){
   char *buf;
   DWORD size;
   file_PCB *handle;
   vfs_stat fileinfo;
   int i;
   DEX32_DDL_INFO *myddl;
   handle=openfilex(s, FILE_READ);

   if (!handle) 
      return -1;
   fstat(handle,&fileinfo);
   size = fileinfo.st_size;
   buf=(char*)malloc(size);
   textbackground(BLUE);
   myddl = Dex32GetProcessDevice();
   printf("Name: %s  Size: %d bytes \n", s, size);
   textbackground(BLACK);
   fread(buf, size, 1, handle);
   for (i=0; i<size; i++){
      if (buf[i]!='\r') 
         printf("%c",buf[i]);
      if (myddl->lines%25==0){ 
         char c;
         printf("\nPress any key to continue, 'q' to quit\n");
         c=getch();
         if (c=='q') 
            break;
      };
   };
   fclose(handle);
   free(buf);
   return 1;
};


//creates a virtual console for a process
DWORD alloc_console(){
   dex32_commit(0xB8000, 1, current_process->pagedirloc, PG_WR);
};

void console(){
   console_main();
};


void prompt_parser(const char *promptstr, char *prompt){
   int i, i2 = 0, i3 = 0;
   char command[10], temp[255];
   strcpy(prompt,"");
   for (i=0; promptstr[i] && i<255; i++){
      if (promptstr[i] != '%'){ //add to the prompt
         prompt[i2]=promptstr[i];
         i2++;
         prompt[i2]=0;
      }else{
         if (promptstr[i+1] != 0){
            if (promptstr[i+1] == '%'){
               prompt[i2] = '%';
               i2++;
               prompt[i2] = 0;
               i+=2;
               continue;
            };
         };
         i3=0;
         for (i2=i+1; promptstr[i2]&&i2 < 255; i2++){
            if (promptstr[i2] == '%' || i3 >= 10) 
               break;
            command[i3]=promptstr[i2];
            i3++;
         };
         i=i2;
         command[i3]=0;
         if (strcmp(command,"cdir")==0){
            strcat(prompt, showpath(temp));
            i2=strlen(prompt);
         };
      };
   };
};
  

void kernel_file_io_demo(){
   char msg[10]="Hello!";  
   char buf[10];  
   file_PCB *fp;

   fp=openfilex("sample.txt",FILE_WRITE);
   fwrite(msg,sizeof(msg),1,fp);
   fclose(fp);
   
   fp=openfilex("sample.txt",FILE_READ);
   fread(buf,sizeof(msg),1,fp);
   printf("%s\n",buf);
   fclose(fp);

}



/*An auxillary function for qsort for comparing two elements, based on size*/
int console_ls_sortsize(vfs_node *n1, vfs_node *n2){
   if (n1->size > n2->size) 
      return -1;
   if (n2->size > n1->size) 
      return 1;
   return 0;
};

/*An auxillary function for qsort for comparing two elements, based on name*/
int console_ls_sortname(vfs_node *n1, vfs_node *n2){
   if ( (n1->attb & FILE_DIRECTORY) && !(n2->attb & FILE_DIRECTORY))
      return -1;

   if ( !(n1->attb & FILE_DIRECTORY) && (n2->attb & FILE_DIRECTORY))
      return 1;
        
   return strsort(n1->name,n2->name);
};

/* ==================================================================
   console_ls(int style):
   
   *list the contents of the current directory to the screen
    style = 1      : list format 
    style = others : wide format  
*/
/*lists the files in the current directory to the console screen*/
void console_ls(int style, int sortmethod){
   vfs_node *dptr=current_process->workdir;
   vfs_node *buffer;
   int totalbytes=0, freebytes=0;
   int totalfiles=0, i;
   char cdatestr[20], mdatestr[20], temp[20];
    
   //obtain total number of files
   totalfiles = vfs_listdir(dptr, 0, 0);
    
   buffer = (vfs_node*) malloc( totalfiles * sizeof(vfs_node));

   //Place the list of files obtained from the VFS into a buffer
   totalfiles = vfs_listdir(dptr, buffer, totalfiles * sizeof(vfs_node));     
    
   //Sort the list
   if (sortmethod == SORT_NAME)
      qsort(buffer, totalfiles, sizeof(vfs_node), console_ls_sortname);
   else if (sortmethod == SORT_SIZE)
      qsort(buffer, totalfiles, sizeof(vfs_node), console_ls_sortsize);
        
   textbackground(BLUE);
   textcolor(WHITE);
    
   if (style==1)
      printf("%-25s %10s %14s %14s\n","Filename", "Size(bytes)", "Attribute", "Date Modified");
        
   textbackground(BLACK);

   for (i=0; i < totalfiles; i++){
      char fname[255];   
      if (style == 0){ //wide view style
         if (buffer[i].attb&FILE_MOUNT)
            textcolor(LIGHTBLUE);
         else if (buffer[i].attb&FILE_DIRECTORY)
            textcolor(GREEN);
         else if (buffer[i].attb&FILE_OEXE)
            textcolor(YELLOW);
         else
            textcolor(WHITE);

         strcpy(fname,buffer[i].name);
         fname[24]=0;
         printf("%-25s ",fname);
         totalbytes+=buffer[i].size;
            
         if ( (i+1)%3==0 && (i+1 < totalfiles) ) 
            printf("\n");

      };

      if (style == 1){ //list view style
         if (buffer[i].attb&FILE_MOUNT)
            textcolor(LIGHTBLUE);
         else if (buffer[i].attb&FILE_DIRECTORY)
            textcolor(GREEN);
         else if (buffer[i].attb&FILE_OEXE)
            textcolor(YELLOW);
         else
            textcolor(WHITE);
                    
         strcpy(fname,buffer[i].name);
         fname[24]=0;
         printf("%-25s ",fname);
            
         textcolor(WHITE);
         printf("%10d %14s %14s\n",buffer[i].size,
         vfs_attbstr(&buffer[i],temp), datetostr(&buffer[i].date_modified,
                       mdatestr));
                       
         totalbytes+=buffer[i].size;


         //try to make it fit the screen
         if ((i+1) % 23==0){
            char c;
            printf("Press Q to quit or any other key to continue ...");
            c=getch();
            printf("\n");
            if (c=='q'||c=='Q') 
               break;
         };
      };       
   };
    
   textcolor(WHITE);
   printf("\nTotal Files: %d  Total Size: %d bytes\n", totalfiles, totalbytes);
   free(buffer);
    
};

static void df_find_mount(vfs_node *dir, int devid, char *out)
{
   vfs_node *n;

   if (out[0] || dir == 0 || dir->files == 0 ||
       dir->files == (vfs_node*)VFS_NOT_MOUNTED)
      return;

   for (n = dir->files; n != 0; n = n->next) {
      if ((n->attb & FILE_MOUNT) && n->memid == devid) {
         getpath(n, out);
         return;
      }
      if ((n->attb & FILE_DIRECTORY) && n->files &&
          n->files != (vfs_node*)VFS_NOT_MOUNTED)
         df_find_mount(n, devid, out);
      if (out[0])
         return;
   }
}

void console_iobench(void)
{
   const char *path = "/icsos/apps/tcc.exe";
   DWORD t0, t1, size = 0, i;
   DWORD hits, misses, fills, slots, merged;
   void *buf;
   DWORD cold_ms, warm_ms, mmap_ms;

   printf("iobench: sequential read of %s\n", path);
   blkcache_reset_stats();

   /* Cold read (first pass may still warm FAT/BPB). */
   t0 = getprecisetime();
   buf = vfs_mapfile(path, &size);
   t1 = getprecisetime();
   if (!buf) {
      printf("iobench: FAIL map %s\n", path);
      return;
   }
   cold_ms = t1 - t0;
   free(buf);

   /* Warm read — should hit block cache heavily. */
   t0 = getprecisetime();
   buf = vfs_mapfile(path, &size);
   t1 = getprecisetime();
   if (!buf) {
      printf("iobench: FAIL warm map\n");
      return;
   }
   warm_ms = t1 - t0;
   free(buf);

   /* Third pass through mmap-style path again for stability. */
   t0 = getprecisetime();
   for (i = 0; i < 3; i++) {
      buf = vfs_mapfile(path, &size);
      if (!buf) break;
      free(buf);
   }
   t1 = getprecisetime();
   mmap_ms = (t1 - t0) / (i ? i : 1);

   blkcache_stats(&hits, &misses, &fills, &slots);
   blkcache_mq_stats(&merged);
   printf("iobench: size=%u bytes\n", (unsigned)size);
   printf("iobench: cold_map=%u ms  warm_map=%u ms  avg3=%u ms\n",
          (unsigned)cold_ms, (unsigned)warm_ms, (unsigned)mmap_ms);
   if (warm_ms > 0)
      printf("iobench: speedup=%u.%ux (cold/warm)\n",
             (unsigned)(cold_ms / warm_ms),
             (unsigned)((cold_ms * 10 / warm_ms) % 10));
   else if (cold_ms > warm_ms)
      printf("iobench: warm was <1 timer tick (good)\n");
   printf("iobench: cache hits=%u misses=%u fills=%u slots=%u merged=%u\n",
          (unsigned)hits, (unsigned)misses, (unsigned)fills,
          (unsigned)slots, (unsigned)merged);
   if (hits > 0)
      printf("IOBENCH_CACHE_OK\n");
   if (warm_ms <= cold_ms)
      printf("IOBENCH_PASS\n");
   else
      printf("IOBENCH_WARN warm slower than cold\n");
}

void console_df()
{
   int i;

   printf("%-10s %9s %9s %9s %4s %s\n",
          "Device", "Size(KB)", "Used(KB)", "Free(KB)", "Use%", "Mounted");

   for (i = 0; i < MAXDEVICES; i++) {
      devmgr_block_desc *blk;
       u64 raw_bytes = 0, size_kb;
       DWORD total_bytes = 0, free_bytes = 0;
       DWORD used_kb, free_kb, pct;
      int have_raw = 0, have_fs = 0;
      char mount[256];
      const char *name;

      if (devmgr_devlist[i] == 0)
         continue;
      if (devmgr_devlist[i]->type != DEVMGR_BLOCK)
         continue;

      blk = (devmgr_block_desc*)devmgr_devlist[i];
      name = blk->hdr.name;

      if (blk->total_blocks && blk->get_block_size) {
          u64 nblk = bridges_call64((devmgr_generic*)blk, &blk->total_blocks);
          DWORD bsz = (DWORD)bridges_call((devmgr_generic*)blk, &blk->get_block_size);
          if (bsz > 0 && bsz != (DWORD)-1 && nblk != (u64)-1) {
             raw_bytes = nblk * bsz;
             have_raw = 1;
          }
       }

      /* Probe FAT on any readable  volume. Skip uninitialized floppy
         (FDC waits hang) and CD-ROM (2048-byte sectors). */
      if (blk->read_block) {
         int skip = 0;
         if (name[0] == 'f' && name[1] == 'd' && !devmgr_getlock(i))
            skip = 1;
         if (name[0] == 'c' && name[1] == 'd')
            skip = 1;
         if (strcmp(name, "null") == 0)
            skip = 1;
         if (!skip)
            have_fs = (fat_statfs(i, &total_bytes, &free_bytes) == 0);
      }

      mount[0] = 0;
      if (vfs_root)
         df_find_mount(vfs_root, i, mount);
      if (mount[0] == 0)
         strcpy(mount, "-");

      if (have_fs) {
          size_kb = (u64)total_bytes / 1024;
          free_kb = free_bytes / 1024;
          used_kb = (total_bytes - free_bytes) / 1024;
          pct = (total_bytes == 0) ? 0 : (used_kb * 100) / (size_kb ? size_kb : 1);
          printf("%-10s %9llu %9u %9u %3u%% %s\n",
                 name, size_kb, used_kb, free_kb, pct, mount);
       } else if (have_raw) {
          size_kb = raw_bytes / 1024;
          printf("%-10s %9llu %9s %9s %4s %s\n",
                 name, size_kb, "-", "-", "-", mount);
       } else {
         printf("%-10s %9s %9s %9s %4s %s\n",
                 name, "-", "-", "-", "-", mount);
       }
    }
}

/* lsblk-style partition table for every disk the partdev layer knows about:
   table type, disk GUID, and per-partition index/start/end/size/type/attrs. */
void console_partitions()
{
    int d, i, any;
    char guid[40];

    any = 0;
    for (d = 0; d < partdev_disk_count(); d++) {
        const partdev_disk *disk = partdev_disk_get(d);
        const char *ttype;

        if (disk->table_type == PARTDEV_TABLE_GPT)
            ttype = "GPT";
        else if (disk->table_type == PARTDEV_TABLE_MBR)
            ttype = "MBR";
        else
            ttype = "none";
        any = 1;
        partdev_format_guid(disk->disk_guid, guid, sizeof(guid));
        if (disk->table_type == PARTDEV_TABLE_GPT)
            printf("%s: %s diskguid=%s entries=%d%s\n", disk->disk_name, ttype,
                   guid, disk->gpt_entries,
                   disk->used_backup ? " (backup header)" : "");
        else
            printf("%s: %s\n", disk->disk_name, ttype);

        for (i = 0; i < partdev_count(); i++) {
            const partdev_entry *e = partdev_get(i);
            u64 size;

            if (e->parent_deviceid != disk->parent_deviceid)
                continue;
            size = (e->endlba > e->startlba) ? (e->endlba - e->startlba) : 0;
            if (e->pname[0])
                printf("  p%-3d start=%llu end=%llu size=%llu type=%s%s name=\"%s\"\n",
                       e->entry_index, (unsigned long long)e->startlba,
                       (unsigned long long)e->endlba, (unsigned long long)size,
                       e->type_name, (e->attrs & 2ULL) ? " [boot]" : "", e->pname);
            else
                printf("  p%-3d start=%llu end=%llu size=%llu type=%s%s\n",
                       e->entry_index, (unsigned long long)e->startlba,
                       (unsigned long long)e->endlba, (unsigned long long)size,
                       e->type_name, (e->attrs & 2ULL) ? " [boot]" : "");
        }
    }
    if (!any)
        printf("No partitioned disks.\n");
}

/* ==================================================================
   console_execute(const char *str):
   * This command is used to execute a console string.

*/
int console_execute(const char *str){
   char temp[512];
   char *u;
   int command_length = 0;
   signed char mouse_x, mouse_y, last_mouse_x=0, last_mouse_y=0;
   int n;
  
   //make a copy so that strtok wouldn't ruin str
   strcpy(temp,str);
   n = (int)strlen(temp);
   while (n > 0 && (temp[n-1] == '\n' || temp[n-1] == '\r' ||
                    temp[n-1] == ' ' || temp[n-1] == '\t' ||
                    temp[n-1] == 3))
      temp[--n] = 0;
  
   u=strtok(temp," ");
  
   if (u == 0) 
      return;
  
   command_length = strlen(u);    
    
   //check if a pathcut command was executed
   if (u[command_length - 1] == ':'){
      char temp[512];
      sprintf(temp,"cd %s",u);            
      console_execute(temp); 
   }else 
   if (strcmp(u,"fgman") == 0){  //--  Foreground manager
      fg_set_state(1);
   }else 
   if (strcmp(u,"mouse") == 0){  //--  Activate the mouse
      while (!kb_ready()){
         get_mouse_pos(&mouse_x,&mouse_y);
         printf("Mouse (x,y): %d %d\n",mouse_x, mouse_y);
         while ((last_mouse_x == mouse_x) && (last_mouse_y==mouse_y)){
            get_mouse_pos(&mouse_x,&mouse_y);
         }
         last_mouse_x=mouse_x;
         last_mouse_y=mouse_y; 
      }
   }else 
   if (strcmp(u,"shutdown") == 0){  //-- Shuts down the system.
      sendmessage(0,MES_SHUTDOWN,0);
   }else
   if (strcmp(u,"procinfo") == 0){  //-- Show process information. Args: <pid>
      int pid;             
      u=strtok(0," ");
      if (u!=0){
         pid = atoi(u);
         show_process_stat(pid);
      };
   }else
   if (strcmp(u,"meminfo") == 0){   //-- Show memory map information.
       mem_interpretmemory(memory_map,map_length);
    }else
    if (strcmp(u,"pf64stats") == 0){  //-- Demand-paging (PF64-DIAG) counters.
       extern void pf64_stats(unsigned long*,unsigned long*,unsigned long*,
                              unsigned long*,unsigned long*);
       unsigned long t,r,e,h,p;
       pf64_stats(&t,&r,&e,&h,&p);
       printf("pf64stats: demand_total=%lu recovered=%lu elf-region=%lu committed-heap=%lu past-brk=%lu\n",
              (unsigned long)t,(unsigned long)r,(unsigned long)e,(unsigned long)h,(unsigned long)p);
    }else
    if (strcmp(u,"brk") == 0){        //-- Show the break/heap pointers of a process. Args: <pid>
       extern PCB386 *ps_findprocess(DWORD);
       int pid;
       u = strtok(0," ");
       if (u != 0) {
          pid = atoi(u);
          {
             PCB386 *pp = ps_findprocess((DWORD)pid);
             if (pp && pp != (PCB386 *)-1) {
                printf("brk pid=%d name=%s\n", pid, pp->name);
                printf("  knext (heap end) = %p\n", (void *)pp->knext);
                printf("  mmap_brk         = %p\n", (void *)pp->mmap_brk);
                printf("  pagedirloc       = %p\n", (void *)pp->pagedirloc);
             } else {
                printf("brk: no process %d\n", pid);
             }
          }
       } else {
          printf("brk: usage: brk <pid>\n");
       }
    }else
    if (strcmp(u,"heap") == 0){       //-- Show the heap layout of a process. Args: <pid>
       extern PCB386 *ps_findprocess(DWORD);
       int pid;
       u = strtok(0," ");
       if (u != 0) {
          pid = atoi(u);
          {
             PCB386 *pp = ps_findprocess((DWORD)pid);
             if (pp && pp != (PCB386 *)-1) {
                printf("heap pid=%d name=%s\n", pid, pp->name);
                printf("  knext (heap end) = %p\n", (void *)pp->knext);
                printf("  mmap_brk         = %p\n", (void *)pp->mmap_brk);
                printf("  pagedirloc       = %p\n", (void *)pp->pagedirloc);
             } else {
                printf("heap: no process %d\n", pid);
             }
          }
       } else {
          printf("heap: usage: heap <pid>\n");
       }
    }else
    if (strcmp(u,"stty") == 0){       //-- COM2 shell line discipline: stty echo | noecho
       u = strtok(0," ");
       if (u && strcmp(u,"echo") == 0) {
          shell2_echo = 1;
          printf("stty: com2 shell echo on\n");
       } else if (u && strcmp(u,"noecho") == 0) {
          shell2_echo = 0;
          printf("stty: com2 shell echo off\n");
       } else {
          printf("stty: com2 shell echo=%d  (usage: stty echo | stty noecho)\n", shell2_echo);
       }
    }else
   if (strcmp(u,"pause") == 0){     //-- Waits for a key press
      printf("press any key to continue or 'q' to quit..\n");
      if (getch() == 'q') 
         return -1;
   }else
   if (strcmp(u,"lspcut") == 0){    //-- Shows a list of path aliases. 
      vfs_showpathcuts();
   }else
   if (strcmp(u,"pcut") == 0){      //-- Creates a path alias. Args: <alias:> [path]
      char *u2,*u3;
      u2 = strtok(0," ");
      u3 = strtok(0," ");
      if (u2 != 0){
         if (vfs_addpathcut(u2,u3) == -1){
            printf("Invalid pathcut specified.\n");
         }else{
            printf("Pathcut added.\n"); 
         }                               
      }else{
         printf("Wrong number of parameters specified.\n");
      }
   }else
   if (strcmp(u,"rmdir") == 0){     //-- Removes a directory and all its subdirectories. Args: <dirname>
      char *u2 = strtok(0," ");
      if (u2 != 0){
         char c;                
         printf("*Warning!* This will delete all files and subdirectories!\n");
         printf("Do you wish to continue? (y/n):");
         c = getch();
         if (c == 'y'){
            printf("Please wait..\n");                        
            if (rmdir(u2) != -1)
               printf("Remove directory successful!\n");
            else
               printf("Error while removing directory.\n");
         }else{
            printf("Remove directory cancelled.\n");
         }
      }else{
         printf("Invalid parameter.\n"); 
      }
   }else
   if (strcmp(u,"rempcut") == 0){   //-- Removes a path alias. Args: <alias:>
      char *u2;
      u2 = strtok(0," ");
      if (u2 != 0){
         if (vfs_removepathcut(u2) == -1)
            printf("Invalid Pathcut or pathcut not found\n");
         else
            printf("Pathcut removed.\n");   
      }else{
         printf("Wrong number of parameters specified\n");
      }               
   }else
   if (strcmp(u,"newconsole") == 0){   //-- Creates a new console.  
      //create a new console         
      console_new();
      printf("New console thread created.\n");                   
   }else  
   if (strcmp(u,"ver") == 0) {         //-- Shows version information.
       printf("%s\n",dex32_versionstring);
       printf("%s\n",OS_VERSION);
       printf("release=%s build=%s%s %s\n",ICSOS_RELEASE,ICSOS_GIT_HASH,
              (ICSOS_GIT_DIRTY[0]=='1')?"-dirty":"",ICSOS_BUILD_TS);
    }else
    if (strcmp(u,"version") == 0) {     //-- Full version banner.
       printf("%s %s\n",OS_NAME,OS_VERSION);
       printf("release=%s build=%s%s %s\n",ICSOS_RELEASE,ICSOS_GIT_HASH,
              (ICSOS_GIT_DIRTY[0]=='1')?"-dirty":"",ICSOS_BUILD_TS);
    }else
    if (strcmp(u,"uname") == 0) {       //-- Kernel build info: -a -r -m -v (no arg = name+release)
       char *a = strtok(0," ");
       if (a != 0 && a[0]=='-' && a[1]=='r') {
          printf("%s\n",ICSOS_RELEASE);
       } else if (a != 0 && a[1]=='m') {
          printf("x86_64\n");
       } else if (a != 0 && a[1]=='v') {
          printf("%s %s build=%s%s %s\n",OS_NAME,OS_VERSION,ICSOS_GIT_HASH,
                 (ICSOS_GIT_DIRTY[0]=='1')?"-dirty":"",ICSOS_BUILD_TS);
       } else if (a != 0 && a[1]=='a') {
          printf("%s %s %s %s %s x86_64 build=%s%s\n",OS_NAME,OS_VERSION,
                 ICSOS_RELEASE,KERNEL_NAME,ICSOS_BUILD_TS,ICSOS_GIT_HASH,
                 (ICSOS_GIT_DIRTY[0]=='1')?"-dirty":"");
       } else {
          printf("%s %s %s x86_64\n",OS_NAME,OS_VERSION,ICSOS_RELEASE);
       }
    }else
    if (strcmp(u,"dmesg") == 0) {       //-- Dump kernel log: -c | -n <0-7> | -l <0-7> (no arg = all)
       char *a = strtok(0," ");
       if (a != 0 && a[0]=='-' && a[1]=='c') {
          klog_clear();
          printf("dmesg: log cleared\n");
       } else if (a != 0 && a[0]=='-' && a[1]=='n') {
          char *v = strtok(0," ");
          if (v != 0) {
             klog_console_max_set((unsigned char)atoi(v));
             printf("dmesg: console log level set to %d\n",klog_console_max_get());
          } else
             printf("dmesg: usage: dmesg -n <0-7>\n");
       } else if (a != 0 && a[0]=='-' && a[1]=='l') {
          char *v = strtok(0," ");
          if (v != 0)
             klog_dump(atoi(v));
          else
             printf("dmesg: usage: dmesg -l <0-7>\n");
       } else {
          klog_dump(-1);
       }
    }else
    if (strcmp(u,"cpuid") == 0){        //-- Displays CPU information.
      hardware_cpuinfo mycpu;
      hardware_getcpuinfo(&mycpu);
      hardware_printinfo(&mycpu);
   }else
   if (strcmp(u,"pciwifi") == 0 || strcmp(u,"wifi") == 0){ //-- Dump PCI network/wireless devices.
      /* N150 bring-up: dump PCI network/wireless for driver work. */
      extern void pci_dump_network(void);
      pci_dump_network();
   }else
   if (strcmp(u,"wifistat") == 0){     //-- Dump RTL8821CE / wlan0 status (FW, MAC, RX).
      extern void rtw8821ce_status(void);
      rtw8821ce_status();
   }else
   if (strcmp(u,"wifidbg") == 0){      //-- RTL8821CE RX-path diagnostic (HW write-pointer dwell).
      extern void rtw8821ce_wifidbg(void);
      rtw8821ce_wifidbg();
   }else
   if (strcmp(u,"wifiscan") == 0){     //-- Poll RX for beacons; print SSIDs.
      struct wifi_dev *w = wifi_default();
      if (!w || !w->ops || !w->ops->scan) {
         printf("wifiscan: no wifi device\n");
      } else {
         (void)w->ops->scan(w);
      }
   }else
   if (strcmp(u,"pci") == 0){          //-- Dump all PCI devices.
      extern void pci_dump_all(void);
      pci_dump_all();
   }else
   if (strcmp(u,"exit") == 0){         //-- Exits a console session.
      fg_exit();
      exit(0);              
   }else  
   if (strcmp(u,"echo") == 0){         //-- Displays a string. Args: <string>  
      u=strtok(0,"\n");
      if (u!=0)              
         printf("%s\n",u);
   }else  
   if (strcmp(u,"use") == 0){          //-- Tells the extension manager to use the extension: Args: <extension>  
      u=strtok(0," ");
      if (extension_override(devmgr_getdevicebyname(u),0) == -1){
         printf("Unable to install extension %s.\n",u);                
      };            
   }else        
   if (strcmp(u,"off") == 0){          //-- Power off the machine.
      dex32apm_off();
   }else
   if (strcmp(u,"files") == 0){        //-- Shows list of currently open files.
      file_showopenfiles();
   }else
   if (strcmp(u,"find") == 0){         //-- Finds a file.
      u=strtok(0," ");
      if (u != 0)
         findfile(u);
   }else
   if (strcmp(u,"dkill") == 0){         //-- Dirty kill a user process/thread. No cleanup is done. Args: <pid>
      u=strtok(0," ");
      if (u!=0){
         ps_user_kill(atoi(u));
      }
   }else
   if (strcmp(u,"kill") == 0){         //-- Kills a thread/process. Performs cleanup.  Args: <thread name>
      //kernel_file_io_demo();
      u=strtok(0," ");
      if (u!=0){
         dex32_killkthread_name(u);
      }
   }else
   if (strcmp(u,"procs") == 0 || strcmp(u,"ps") == 0){  //-- List the running processes. "ps" can also be used.
      show_process();
   }else
   if (strcmp(u,"cls") == 0 || strcmp(u,"clear") == 0){          //-- Clears the screen. 
      clrscr();
      unsigned char stk[10240];
      //createkthread((void *)runner,"runner",10240);
      //createthread((void*)runner,stk,10240);
   }else
   if (strcmp(u,"help") == 0){         //-- Displays this help screen.
      console_execute("type /icsos/icsos.hlp");
   }else
   if (strcmp(u,"df") == 0){           //-- Shows free space on block devices.
       console_df();
    }else
    if (strcmp(u,"partitions") == 0){   //-- Shows partition tables (MBR/GPT) and partitions.
       console_partitions();
    }else
    if (strcmp(u,"iobench") == 0){      //-- Benchmark file I/O / block cache.
      console_iobench();
   }else
   if (strcmp(u,"umount") == 0){       //-- Unmounts a mounted device. Args: <mount point>
      char *u =strtok(0," ");
      if (u!=0){
         if (vfs_unmount_device(u)==-1)
            printf("umount failed.\n");
         else
            printf("%s umounted.\n",u);
      }else{
         printf("Missing parameter.\n");
      }                    
   }else
   if (strcmp(u,"mount") == 0){        //-- Mounts a device. Args: fat/cdfs <partition/device> <mount point> 
      char *fsname,*devname,*location;
      fsname=strtok(0," ");
      devname=strtok(0," ");
      location=strtok(0," ");
               
      if (vfs_mount_device(fsname, devname, location) == -1)
         printf("mount not successful.\n");
      else
         printf("mount successful.\n");  
         //fat12_mount_root(root,floppy_deviceid);
   }else
   if (strcmp(u,"pwd") == 0){         //-- Shows the current working directory.
      char temp[255];
      printf("%s\n",showpath(temp));
   }else
   if (strcmp(u,"lsmod") == 0){        //-- Shows the list of loaded libraries and modules. 
      showlibinfo();
   }else
   if (strcmp(u,"mem") == 0){          //-- Shows memory information.
      meminfo();
   }else
   if (strcmp(u,"mkdir") == 0){        //-- Creates a directory. Args: <directory name> 
      u=strtok(0," ");
      if (u!=0){
         if (mkdir(u) == -1)
            printf("mkdir failed.\n");
      }
   }else       
   if (strcmp(u,"run") == 0){          //-- Executes a batch file or script. Args: <script>
      u=strtok(0," ");
      if (u!=0){
         if (script_load(u) == -1){
            printf("console: Error loading script file.\n");
         };            
      }
   }else    
   if (strcmp(u,"ls") == 0||strcmp(u,"dir") == 0){ //-- Shows directory listing. Args: [-l | -osize | -oname] 
      int style=0, ordering = 0;
      char v[20];
   
      u=strtok(0," ");
      if (u != 0){
         do {
            strcpy(v,u);
            if (strcmp(v,"-l") == 0) 
               style=1;
            if (strcmp(v,"-oname") == 0) 
               ordering  = 0;
            if (strcmp(v,"-osize") == 0) 
               ordering  = 1;
            u=strtok(0," ");
         } while (u!=0);
      };
      console_ls(style, ordering);
   }else
   if (strcmp(u,"del") == 0){             //-- Deletes a files or directory. Args: <filename/dirname>
      int res;
      u=strtok(0," ");
      if (u!=0){
         char *u3=strtok(0," ");
         if (u3==0){
            delfile(u);
            printf("File deleted.\n");
         }else{
            printf("Invalid parameter.\n");
         }
      }else{
         printf("Missing parameter.\n");
      }
   }else
   if (strcmp(u,"ren") == 0){            //-- Renames a file. Args: <oldname> <newname>
      char *u2,*u3;
      u2=strtok(0," ");
      u3=strtok(0," ");               
      if (u2!=0 && u3!=0){
         if (rename(u2, u3)) 
            printf("File renamed.\n");
         else
            printf("Error renaming file.\n");
      }else{
         printf("Missing parameter.\n"); 
      }   
   }else
   if (strcmp(u,"type") == 0 || strcmp(u,"cat") == 0 ){ //-- Displays the contents of a file. Args: <filename> [-p]
      u=strtok(0," ");
      if (u!=0){
         if (console_showfile(u,0)==-1)
            printf("error opening file.\n");
      }else{
         printf("missing parameter.\n");
      }
   }else
   if (strcmp(u,"copy") == 0 || strcmp(u,"cp") == 0){ //-- Copy source to destination: Args: <source> <destination>
      u=strtok(0," ");
      if (u!=0){
         char *u2 = strtok(0," ");
         if (u2!=0){
            if (fcopy(u,u2) == -1){
               printf("Copy failed. Error while copying.\n");
               printf("Destination directory might not be present.\n");
            };
         };  
      };
   }else              
   if (strcmp(u,"cd") == 0){     //-- Changes working directory. Args: <directory>
      u=strtok(0," ");
      if (u!=0){
         if (!changedirectory(u))
            printf("cd: Cannot find directory\n");
      }else{
         changedirectory(0); //go to working directory
      } 
   }else
   if (strcmp(u,"loadmod") == 0){   //-- Loads a shared library (.dll or .so). Args: <module filename> 
      u=strtok(0," ");
      if (u!=0){
         if (loadDLL(u,str) == -1)
            printf("Unable to load %s.\n",u);
         else
            printf("Load module Successful.\n");  
      }else{
            printf("missing parameter.\n");
      }
   }else
   if (strcmp(u,"lsdev") == 0){  //-- Lists all modules currently installed and available. 
      devmgr_showdevices();
   }else
   if (strcmp(u,"lsext") == 0){  //-- Lists all extensions.
      extension_list();
   }else
   if (strcmp(u,"libinfo") == 0){ //-- Shows library information.
      u=strtok(0," ");
      module_listfxn(u);
   }else
   if (strcmp(u,"time") == 0){   //-- Displays date and time.
      printf("%d/%d/%d %d:%d.%d (%d total seconds since 1970)\n",time_systime.day,
               time_systime.month, time_systime.year,
               time_systime.hour, time_systime.min,
               time_systime.sec,time());
   }else
   if (strcmp(u,"set") == 0){    //-- Sets an environment variable. Args: <key>=<value>
      u=strtok(0," ");
      if (u==0){
         env_showenv();
      }else{
         char *name  = strtok(u,"=");
         char *value = strtok(0,"\n");
         env_setenv(name, value, 1);
      }; 
   }else         
   if (strcmp(u,"unload") == 0){ //-- Unloads a library. Args: <library name>
      u=strtok(0," ");
      if (u!=0){
         if (module_unload_library(u) == -1)
            printf("Error unloading library");
   	};
   }else
   if (strcmp(u,"demo_graphics") == 0){   //-- Runs the graphics demonstration.
      demo_graphics();
   }else
   if (strcmp(u,"cc") == 0){   //-- Builds a C program (invokes tcc.exe). Args: <name.exe> <name.c>
      char src[30],exe[30],cmdline[256],path[256];
      char sdk_home[128]="";
      env_getenv("SDK_HOME",sdk_home);
      env_getenv("PATH",path);
      if ( (strcmp(sdk_home,"")==0) || strcmp(path,"")==0 ){
         printf("Please set the SDK_HOME and PATH environment variables first.\n");
      }else{
         u=strtok(0," ");
         if (u!=0){
            strcpy(exe,u);
            u=strtok(0," ");
            if (u!=0){
               strcpy(src,u);
               sprintf(cmdline,"%s/tcc.exe -nostdlib -static -o%s %s -B%s %s/tccsdk.c %s/posix.c %s/libtcc1.c %s/crt1.c %s/setjmp.c",
                        path,exe,src,sdk_home,sdk_home,sdk_home,sdk_home,sdk_home,sdk_home);
               user_execp("/icsos/apps/tcc.exe",0,cmdline);
            }else{
               printf("Usage: cc <name.exe> <name.c>\n");
            }
         }else{
               printf("Usage: cc <name.exe> <name.c>\n");
         }
      }
   }else
   if (strcmp(u,"reboot") == 0){  //-- Reboot the machine (keyboard + QEMU ports).
      machine_reboot();
   }else
   if (strcmp(u,"kbuild") == 0){  //-- Supported path: GNU make + GCC + binutils.
         gmake_kbuild_run("/icsos/apps/make.exe", "/work/kernel",
                     "/icsos/apps/gcc.exe",
                        "/icsos/apps/ld.exe", "", "host-seeded");
    }else
    if (strcmp(u,"gkbuild") == 0){  //-- Diagnostic direct GCC build (without make).
       gkbuild_run();
    }else
    if (strcmp(u,"gccselfhost") == 0){  //-- Rebuild GCC, then kernel, entirely in-OS.
       gccselfhost_run();
    }else
    if (strcmp(u,"tcckbuild") == 0){  //-- Optional TinyCC kernel-build experiment.
       kbuild_run(0);
    }else
   if (strcmp(u,"kexec") == 0){  //-- Boot a kernel ELF from ramdisk/path.
      u=strtok(0," ");
      if (!u)
         u="/ramdisk/Kernel64.bin";
      if (kexec_load(u) == 0)
         kexec_reboot();
   }else
   if (strcmp(u,"kexeccert") == 0){  //-- Final generated-kernel capability marker.
      if (!kernel_kexeced) {
         printf("KEXEC_CAPABILITY_FAIL not-kexeced\n");
      } else if (cpu_count < 2) {
         printf("KEXEC_CAPABILITY_FAIL smp cpus=%d\n", cpu_count);
      } else {
         printf("KEXEC_SMP_OK cpus=%d\n", cpu_count);
         printf("KEXEC_CAPABILITY_PASS\n");
      }
   }else
   if (strcmp(u,"fullhost") == 0){  //-- Generic self-host alias uses GCC.
      gkbuild_run();
   }else
   if (strcmp(u,"tccfullhost") == 0){  //-- Optional TinyCC bootstrap.
      fullhost_run();
   }else
   if (strcmp(u,"exectest") == 0){  //-- Run the host-built hello.exe (ELF loader smoke test).
      printf("exectest: running /icsos/apps/hello.exe\n");
      if (!user_execp("/icsos/apps/hello.exe", 0, "/icsos/apps/hello.exe"))
         printf("EXEC_TEST_FAIL\n");
      else
         printf("EXEC_TEST_PASS\n");
   }else
   if (strcmp(u,"posixio") == 0){
      printf("posixio: running /icsos/apps/posixio.exe\n");
      if (!user_execp("/icsos/apps/posixio.exe", 0, "/icsos/apps/posixio.exe"))
         printf("POSIXIO_FAIL\n");
   }else
   if (strcmp(u,"spawntest") == 0){
        printf("spawntest: running /icsos/apps/spawn.exe\n");
        if (!user_execp("/icsos/apps/spawn.exe", 0, "/icsos/apps/spawn.exe"))
           printf("SPAWN_FAIL\n");
     }else
   if (strcmp(u,"forktest") == 0){
        printf("forktest: running /icsos/apps/forktest.exe\n");
        if (!user_execp("/icsos/apps/forktest.exe", 0, "/icsos/apps/forktest.exe"))
           printf("FORK_RUN_FAIL\n");
     }else
      if (strcmp(u,"forkoomtest") == 0){
        printf("forkoomtest: injecting one COW allocation failure\n");
        userpd_cow_fail_next();
        if (!user_execp("/icsos/apps/forktest.exe", 0,
               "/icsos/apps/forktest.exe --oom"))
           printf("FORK_OOM_RUN_FAIL\n");
        }else
     if (strcmp(u,"ext4test") == 0){
         printf("ext4test: running /icsos/apps/ext4test.exe\n");
         if (!user_execp("/icsos/apps/ext4test.exe", 0, "/icsos/apps/ext4test.exe"))
            printf("EXT4_RUN_FAIL\n");
      }else
      if (strcmp(u,"termtest") == 0){
         printf("termtest: running /icsos/apps/termtest.exe\n");
         if (!user_execp("/icsos/apps/termtest.exe", 0, "/icsos/apps/termtest.exe"))
            printf("TERMTEST_RUN_FAIL\n");
      }else
    if (strcmp(u,"vimtest") == 0){  //-- Run FEAT_TINY vim --version (ELF TUI loader smoke test).
       printf("vimtest: running /icsos/apps/vim.exe --version\n");
       if (!user_execp("/icsos/apps/vim.exe", 0, "/icsos/apps/vim.exe --version"))
           printf("VIM_RUN_FAIL\n");
     }else
     if (strcmp(u,"duptest") == 0){  //-- Run the dup(2) syscall self-test (tty fd clone + write).
        printf("duptest: running /icsos/apps/duptest.exe\n");
        if (!user_execp("/icsos/apps/duptest.exe", 0, "/icsos/apps/duptest.exe"))
           printf("DUPT_RUN_FAIL\n");
     }else
     if (strcmp(u,"cc1test") == 0){  //-- Run host-built GCC cc1 to compile a trivial file in-OS.
       char cmd[512];
       int ok = 1;
       file_PCB *f;
      printf("cc1test: /icsos/apps/cc1.exe /icsos/apps/cc1probe.c -o /ramdisk/cc1probe.s\n");
        if (ok) {
           /* cc1 is the C frontend: it emits assembly directly. '-c' is a
              gcc *driver* option (compile+assemble, no link) and is rejected
              by cc1, so it must not appear on the cc1 command line. */
           sprintf(cmd, "/icsos/apps/cc1.exe /icsos/apps/cc1probe.c -o /ramdisk/cc1probe.s");
          if (!user_execp("/icsos/apps/cc1.exe", 0, cmd)) {
             printf("CC1_TEST_FAIL compile\n");
             ok = 0;
          }
       }
       if (ok) {
           f = openfilex("/ramdisk/cc1probe.s", FILE_READ);
           if (f) {
              vfs_stat info;
              static char cc1win[4104];
              char cc1prev[8];
              int cc1prevlen = 0;
              int has_text = 0, has_func = 0;
              int nr;
              fstat(f, &info);
              memset(cc1prev, 0, sizeof(cc1prev));
              if (info.st_size < 4096) {
                 printf("CC1_TEST_FAIL output size %lu\n", (unsigned long)info.st_size);
                 ok = 0;
              } else {
                 while (!(has_text && has_func)) {
                    int wlen;
                    nr = (int)fread(cc1win + cc1prevlen, 1, 4096, f);
                    if (nr <= 0)
                       break;
                    wlen = cc1prevlen + nr;
                    if (cc1_window_contains(cc1win, wlen, ".text"))
                       has_text = 1;
                    if (cc1_window_contains(cc1win, wlen, "main:") ||
                        cc1_window_contains(cc1win, wlen, "fn_"))
                       has_func = 1;
                    if (wlen >= 7) {
                       memcpy(cc1prev, cc1win + wlen - 7, 7);
                       cc1prevlen = 7;
                    } else {
                       memcpy(cc1prev, cc1win, (unsigned int)wlen);
                       cc1prevlen = wlen;
                    }
                 }
                 if (!has_text || !has_func) {
                    printf("CC1_TEST_FAIL missing markers text=%d func=%d size=%lu\n",
                           has_text, has_func, (unsigned long)info.st_size);
                    ok = 0;
                 } else {
                    printf("cc1test: /ramdisk/cc1probe.s is %lu bytes\n",
                           (unsigned long)info.st_size);
                 }
              }
              fclose(f);
           } else {
              printf("CC1_TEST_FAIL no output file\n");
              ok = 0;
           }
        }
       if (ok)
           printf("CC1_TEST_PASS\n");
     }else
    if (strcmp(u,"gctest") == 0){  //-- Full in-OS GCC toolchain: cc1 -> as -> ld -> exec.
       char cmd[512];
       int ok = 1;
       file_PCB *f;
       /* Stage the small inputs to /ramdisk so the spawned tool children never
          read from the CD mid-run (the proven bintest/selfhost pattern). The
          tools themselves (cc1/as/ld) and the linker scripts stay on the CD. */
       if (ok) {
          printf("gctest: staging probe + SDK runtime objects onto /ramdisk\n");
          if (fcopy("/icsos/apps/gccprobe.c", "/ramdisk/gccprobe.c") == -1 ||
              fcopy("/icsos/apps/crt1.o", "/ramdisk/crt1.o") == -1 ||
              fcopy("/icsos/apps/tccsdk.o", "/ramdisk/tccsdk.o") == -1 ||
              fcopy("/icsos/apps/libtcc1.o", "/ramdisk/libtcc1.o") == -1 ||
              fcopy("/icsos/apps/posix.o", "/ramdisk/posix.o") == -1 ||
              fcopy("/icsos/apps/setjmp.o", "/ramdisk/setjmp.o") == -1) {
             printf("GCC_E2E_FAIL stage\n");
             ok = 0;
          }
       }
       /* 1. cc1 (C frontend) emits assembly for the test program. */
       if (ok) {
          printf("gctest: cc1 /ramdisk/gccprobe.c -o /ramdisk/gccprobe.s\n");
          sprintf(cmd, "/icsos/apps/cc1.exe /ramdisk/gccprobe.c -o /ramdisk/gccprobe.s");
          if (!user_execp("/icsos/apps/cc1.exe", 0, cmd)) {
             printf("GCC_E2E_FAIL compile\n");
             ok = 0;
          }
       }
       /* 2. as (GAS) assembles the .s into an ELF64 object. */
       if (ok) {
          printf("gctest: as --64 /ramdisk/gccprobe.s -o /ramdisk/gccprobe.o\n");
          sprintf(cmd, "/icsos/apps/as.exe --64 /ramdisk/gccprobe.s -o /ramdisk/gccprobe.o");
          if (!user_execp("/icsos/apps/as.exe", 0, cmd)) {
             printf("GCC_E2E_FAIL assemble\n");
             ok = 0;
          }
       }
       /* 3. ld (GNU ld) links the object + SDK runtime into a runnable ELF64. */
       if (ok) {
          printf("gctest: ld /ramdisk/gccprobe.o <sdk runtime> -o /ramdisk/gccprobe.exe\n");
          sprintf(cmd, "/icsos/apps/ld.exe -T /icsos/apps/ldscripts/elf_x86_64.xc /ramdisk/gccprobe.o /ramdisk/crt1.o /ramdisk/tccsdk.o /ramdisk/libtcc1.o /ramdisk/posix.o /ramdisk/setjmp.o -o /ramdisk/gccprobe.exe");
          if (!user_execp("/icsos/apps/ld.exe", 0, cmd)) {
             printf("GCC_E2E_FAIL link\n");
             ok = 0;
          }
       }
       /* Verify the linked exe exists and is a plausible ELF. */
       if (ok) {
          f = openfilex("/ramdisk/gccprobe.exe", FILE_READ);
          if (f) {
             vfs_stat info;
             fstat(f, &info);
             fclose(f);
             if (info.st_size < 32) {
                printf("GCC_E2E_FAIL link size %lu\n", (unsigned long)info.st_size);
                ok = 0;
             } else {
                printf("gctest: /ramdisk/gccprobe.exe is %lu bytes\n", (unsigned long)info.st_size);
             }
          } else {
             printf("GCC_E2E_FAIL no gccprobe.exe\n");
             ok = 0;
          }
       }
       /* 4. Run: the kernel loads the ld-built ELF64 and runs _start -> main -> puts. */
        if (ok) {
           printf("gctest: exec /ramdisk/gccprobe.exe\n");
           if (!user_execp("/ramdisk/gccprobe.exe", 0, "/ramdisk/gccprobe.exe")) {
              printf("GCC_E2E_FAIL exec\n");
              ok = 0;
           }
           if (ok)
              printf("GCC_E2E_RUN_OK\n");
        }
     }else
     if (strcmp(u,"gccclosed") == 0){  //-- Verify the rebuilt GCC after kexec.
        char cmd[512];
        int ok = 1;
        file_PCB *f;
        printf("gccclosed: staging source and SDK runtime\n");
        if (fcopy("/icsos/apps/gccprobe.c", "/ramdisk/gccprobe.c") == -1 ||
            fcopy("/icsos/apps/crt1.o", "/ramdisk/crt1.o") == -1 ||
            fcopy("/icsos/apps/tccsdk.o", "/ramdisk/tccsdk.o") == -1 ||
            fcopy("/icsos/apps/libtcc1.o", "/ramdisk/libtcc1.o") == -1 ||
            fcopy("/icsos/apps/posix.o", "/ramdisk/posix.o") == -1 ||
            fcopy("/icsos/apps/setjmp.o", "/ramdisk/setjmp.o") == -1) {
           printf("GCC_CLOSED_TOOLCHAIN_FAIL stage\n");
           ok = 0;
        }
        if (ok) {
           sprintf(cmd, "/work/gcc.exe -B/work /ramdisk/gccprobe.c -o /ramdisk/gccclosed.exe");
           if (!user_execp("/work/gcc.exe", 0, cmd)) {
              printf("GCC_CLOSED_TOOLCHAIN_FAIL driver\n");
              ok = 0;
           }
        }
        if (ok) {
           f = openfilex("/ramdisk/gccclosed.exe", FILE_READ);
           if (!f) {
              printf("GCC_CLOSED_TOOLCHAIN_FAIL output\n");
              ok = 0;
           } else
              fclose(f);
        }
        if (ok && !user_execp("/ramdisk/gccclosed.exe", 0,
                              "/ramdisk/gccclosed.exe")) {
           printf("GCC_CLOSED_TOOLCHAIN_FAIL exec\n");
           ok = 0;
        }
        if (ok)
           printf("GCC_CLOSED_TOOLCHAIN_RUN_OK\n");
     }else
     if (strcmp(u,"gccdrv") == 0){  //-- Run the standalone in-OS gcc driver (gcc.exe):
                                     //-- it spawns cc1/as/ld itself via posix_spawn, then exec.
        char cmd[512];
        int ok = 1;
        file_PCB *f;
        /* Stage the source + SDK runtime .o's onto /ramdisk so the children the
           driver spawns never read the CD mid-run. The driver + cc1/as/ld ELFs
           stay on the CD (loaded by the kernel exec path). */
        if (ok) {
           printf("gccdrv: staging probe + SDK runtime objects onto /ramdisk\n");
           if (fcopy("/icsos/apps/drvprobe.c", "/ramdisk/drvprobe.c") == -1 ||
               fcopy("/icsos/apps/crt1.o", "/ramdisk/crt1.o") == -1 ||
               fcopy("/icsos/apps/tccsdk.o", "/ramdisk/tccsdk.o") == -1 ||
               fcopy("/icsos/apps/libtcc1.o", "/ramdisk/libtcc1.o") == -1 ||
               fcopy("/icsos/apps/posix.o", "/ramdisk/posix.o") == -1 ||
               fcopy("/icsos/apps/setjmp.o", "/ramdisk/setjmp.o") == -1) {
              printf("GCC_DRV_FAIL stage\n");
              ok = 0;
           }
        }
        /* 1. The gcc driver does the whole compile+link (cc1 -> as -> ld). */
        if (ok) {
           printf("gccdrv: gcc.exe /ramdisk/drvprobe.c -o /ramdisk/drvprobe.exe\n");
           sprintf(cmd, "/icsos/apps/gcc.exe /ramdisk/drvprobe.c -o /ramdisk/drvprobe.exe");
           if (!user_execp("/icsos/apps/gcc.exe", 0, cmd)) {
              printf("GCC_DRV_FAIL driver\n");
              ok = 0;
           }
        }
        /* The driver's success is proven by its OUTPUT: a linked, valid ELF.
           (The kernel does not propagate a child's exit status via waitpid.) */
        if (ok) {
           f = openfilex("/ramdisk/drvprobe.exe", FILE_READ);
           if (f) {
              vfs_stat info;
              fstat(f, &info);
              fclose(f);
              if (info.st_size < 32) {
                 printf("GCC_DRV_FAIL link size %lu\n", (unsigned long)info.st_size);
                 ok = 0;
              } else {
                 printf("gccdrv: /ramdisk/drvprobe.exe is %lu bytes\n", (unsigned long)info.st_size);
              }
           } else {
              printf("GCC_DRV_FAIL no drvprobe.exe\n");
              ok = 0;
           }
        }
        /* 2. Run the driver's output: the kernel loads the ELF64 and runs main. */
        if (ok) {
           printf("gccdrv: exec /ramdisk/drvprobe.exe\n");
           if (!user_execp("/ramdisk/drvprobe.exe", 0, "/ramdisk/drvprobe.exe")) {
              printf("GCC_DRV_FAIL exec\n");
              ok = 0;
           }
           if (ok)
              printf("GCC_DRV_RUN_OK\n");
        }
     }else
     if (strcmp(u,"selfhost") == 0){  //-- Compile a test program with in-OS tcc and run it.
      char cmd[512];
      int ok = 1;
      /* Stage onto ramdisk so compiles do not re-hit ATAPI mid-run. */
      if (ok) {
         printf("selfhost: staging tcc + sources onto /ramdisk\n");
         if (fcopy("/icsos/apps/tcc.exe", "/ramdisk/tcc.exe") == -1 ||
             fcopy("/icsos/apps/min.c", "/ramdisk/min.c") == -1 ||
             fcopy("/icsos/apps/hello.c", "/ramdisk/hello.c") == -1) {
            printf("SELFHOST_TEST_FAIL stage\n");
            ok = 0;
         }
      }
      if (ok) {
         printf("selfhost: compiling /ramdisk/min.c (no includes)\n");
         sprintf(cmd,
                 "/ramdisk/tcc.exe -nostdlib -static -o/ramdisk/min.exe /ramdisk/min.c");
         if (!user_execp("/ramdisk/tcc.exe", 0, cmd)) {
            printf("SELFHOST_TEST_FAIL min.c\n");
            ok = 0;
         }
      }
      if (ok) {
         printf("selfhost: running /ramdisk/min.exe\n");
         if (!user_execp("/ramdisk/min.exe", 0, "/ramdisk/min.exe")) {
            printf("SELFHOST_TEST_FAIL run min.exe\n");
            ok = 0;
         }
      }
      if (ok) {
         printf("selfhost: compiling /ramdisk/hello.c\n");
         sprintf(cmd,
                 "/ramdisk/tcc.exe -nostdlib -static -o/ramdisk/hello.exe /ramdisk/hello.c");
         if (!user_execp("/ramdisk/tcc.exe", 0, cmd)) {
            printf("SELFHOST_TEST_FAIL hello.c\n");
            ok = 0;
         }
      }
      if (ok) {
         printf("selfhost: running /ramdisk/hello.exe\n");
         if (!user_execp("/ramdisk/hello.exe", 0, "/ramdisk/hello.exe")) {
            printf("SELFHOST_TEST_FAIL run hello.exe\n");
            ok = 0;
         }
      }
      if (ok)
         printf("SELFHOST_TEST_PASS\n");
   }else
   if (strcmp(u,"tccboot") == 0){  //-- Rebuild TinyCC with the in-OS tcc.
      tccboot_run();
   }else
   if (strcmp(u,"makeboot") == 0){  //-- TinyCC builds GNU make onto /work.
      makeboot_run();
   }else
   if (u[0] == '$'){                      //-- Sends message to a device.
      int i, devid;
      char devicename[255],*cmd;

      for (i=0;i<20 && u[i+1];i++){
         devicename[i] = u[i+1];
      };
      devicename[i] = 0;
      printf("Sending command to %s\n",devicename);
      devid = devmgr_finddevice(devicename);
               
      if (devid != -1){
         if (devmgr_sendmessage(devid,DEVMGR_MESSAGESTR,str)==-1)
            printf("console: send_message() failed or not supported.\n");
      }else{
         printf("console: cannot find device.\n");
      }   

   }else{         //ok it is not a command, maybe it's an executable?
      if (u!=0){
         char path[256]="", tmp[256];
         env_getenv("PATH",path);     
         if (strcmp(path,"")==0){
            strcpy(path,"/icsos/apps");
            sprintf(tmp,"%s/%s",path,u);
            if (!user_execp(tmp, 0, str)){
               printf("Command or executable not found.\n");
            }
         }else{
            sprintf(tmp,"%s/%s",path,u);
            if (!user_execp(tmp, 0, str)){
               printf("Command or executable not found.\n");
            }
         }
      }
   }
   //normal termination
   return 1;
};

int console_new(){
   //create a new console         
   char consolename[255];
   sprintf(consolename,"console(%d)", console_first);    
   return createkthread_on_cpu((void*)console, consolename, 200000,0);
};

void console_main(){
   DEX32_DDL_INFO *myddl=0;
   fg_processinfo *myfg;
   char s[256]="";
   char temp[256]="";
   char last[256]="";
   char console_fmt[256]="%cdir% %% ";
   char console_prompt[256]="cmd >";
    
   DWORD ptr;
    
   myddl =Dex32CreateDDL();    

    
   Dex32SetProcessDDL(myddl, getprocessid());
   myfg = fg_register(myddl, getprocessid());
   fg_setforeground( myfg->id );
   {
      int tflags = TTY_ECHO | TTY_ICANON | TTY_ISIG;
      struct _dex32_direct_device_hdl *tdl = (struct _dex32_direct_device_hdl *)myddl;
      /* Headless oracle: when there is no real VGA framebuffer (a QEMU
         -nographic boot, or a laptop with no VGA), the DDL's hardware
         pointer is a malloc'd shadow buffer, not 0xB8000.  In that case the
         console tty must be serial-backed so full-screen applications
         (e.g. NetHack, which writes to the tty rather than the serial log)
         are visible on the serial console.  vt_feed() routes TTY_SERIAL
         bytes straight to COM1; a real VGA console keeps the DDL path. */
      if (myddl && (myddl->hdw_ptr == 0 || (void *)myddl->hdw_ptr != (void *)0xB8000))
         tflags |= TTY_SERIAL;
      tty_t *t = tty_alloc(tdl, tflags);
      tty_set_fg(t);
      tty_attach_proc(current_process, t);
      if (myfg && myfg != (fg_processinfo *)-1)
         myfg->tty = t;
   }

   clrscr();
   printf("console: tmux C-b [/c/n/p/l/0-9/w/x/?  F2 new  F4 kill fg\n");
   strcpy(last,"");

   /* First window: run autoexec while MSC is still quiet (hotplug/CDC pump
      starts only after CONSOLE_READY). Do not auto-exec sh.exe — loading it
      from USB-root races the same pump and leaves a blank hung tty. Extra
      C-b c windows always drop straight to the kernel prompt. */
   {
      int is_first = (console_first == 0);

      if (is_first) {
         if (kernel_kexeced && strcmp(kernel_cmdline, "kexeced") == 0) {
            printf("KEXEC_BOOT_OK\n");
            serial_puts("KEXEC_BOOT_OK\n");
            if (script_load("/icsos/postkexec.bat") == -1)
               machine_reboot();
         } else
            script_load("/icsos/autoexec.bat");
      }

      console_first++;
      printf("CONSOLE_READY window=%d\n", console_first - 1);
      serial_puts("CONSOLE_READY\n");
      if (is_first)
         printf("kernel prompt (type sh for POSIX shell, help for commands)\n");
   }
   do{
      textcolor(WHITE);
      textbackground(BLACK);
      prompt_parser(console_fmt,console_prompt);
    
      textcolor(LIGHTBLUE);
      printf("%s",console_prompt);
      textcolor(WHITE);
    
      if (strcmp(s,"@@")!=0 && strcmp(s,"!!")!=0)
         strcpy(last,s);
    
      getstring(s, myddl);
   
      if (strcmp(s,"!")==0){
         sendtokeyb(last,&_q);
      }
      else if (strcmp(s,"!!")==0){
         sendtokeyb(last,&_q);
         sendtokeyb("\r",&_q);
      }
      else
          console_execute(s);
    } while (1);
};

/* shell2: an interactive terminal on COM2 (0x2F8) for live command execution
   and kernel introspection without a QEMU/OS reboot. Reads a line from COM2,
   echoes it (unless echo is disabled), and dispatches it through the same
   console_execute() interpreter the built-in console uses. Command output
   (printf) is mirrored to COM2 via serial2_mirror so the shell sees results
   on the same port it types on.

   The line discipline is telnet-compatible:
     * CR (0x0d) is the Enter key (telnet clients send CR, not LF); a trailing
       LF (CRLF) is swallowed so it does not submit an empty line.
     * Both DEL (0x7f) and BS (0x08) act as backspace.
     * Ctrl-C (0x03) aborts the current line.
   Echo is controlled by the 'stty echo' / 'stty noecho' console commands
   (see shell2_echo above), because telnet clients typically do local echo
   themselves and would otherwise double-echo. */
void shell2_main(void)
{
    static char line[512];
    int len = 0;
    int saw_cr = 0;
    serial2_mirror_set(1);
    serial2_puts("\r\n[ICS-OS com2 shell] type 'help' for commands, 'exit' to quit\r\n");
    serial2_puts("com2> ");
    while (1) {
        int c = serial2_getc();
        if (c < 0) {
            taskswitch();
            continue;
        }
        if (c == '\r') {
            /* telnet Enter: submit the line. */
            line[len] = 0;
            if (len > 0) {
                if (shell2_echo) serial2_puts("\r\n");
                console_execute(line);
            }
            len = 0;
            saw_cr = 1;
            serial2_puts("com2> ");
        } else if (c == '\n') {
            /* LF: if it trails a CR (CRLF) swallow it, else treat as Enter. */
            if (saw_cr) {
                saw_cr = 0;
            } else {
                line[len] = 0;
                if (len > 0) {
                    if (shell2_echo) serial2_puts("\r\n");
                    console_execute(line);
                }
                len = 0;
                serial2_puts("com2> ");
            }
        } else if (c == 127 || c == 8) {
            /* DEL or BS backspace. */
            if (len > 0) {
                len--;
                if (shell2_echo) serial2_puts("\b \b");
            }
            saw_cr = 0;
        } else if (c == 3) {
            /* Ctrl-C: abort the current line. */
            len = 0;
            saw_cr = 0;
            if (shell2_echo) serial2_puts("^C\r\n");
            serial2_puts("com2> ");
        } else if (c >= 32 && c < 127) {
            if (len < 511) {
                line[len++] = (char)c;
                if (shell2_echo) serial2_putc((char)c);
            }
            saw_cr = 0;
        } else {
            saw_cr = 0;
        }
    }
}

DWORD shell2_start(void)
{
    extern DWORD createkthread_on_cpu(void *ptr, char *name, DWORD stacksize, int cpu);
    return createkthread_on_cpu((void*)shell2_main, "shell2", 32768, 0);
}

