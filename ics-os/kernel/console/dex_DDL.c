/*
  Name: DEX32 Direct Device Layer Management System
  Copyright: 
  Author: Joseph Emmanuel De Luna Dayo
  Date: 23/10/03 02:35
  Description: 
  ==========================================================================
  DEX32 Direct Device Layer Management System
  -This module provides a set of functions for managing output devices
   between multiple processes.
  -It enables each proess to have its own virtual console environment
  ==========================================================================
*/

DEX32_DDL_INFO *ActiveDDL=0;
int totalDDL=0;

static int ddl_legacy_vga(const DEX32_DDL_INFO *dev)
{
   return dev && (unsigned long)dev->hdw_ptr == 0xB8000UL;
}

/* Identity-map kernel / heap pointers live in the low 4GiB. A non-canonical
   or NULL DDL (stale PCB.outdev before process_init) must not be followed:
   Dex32PutC would #GP(0) on the first VGA putc. */
static int ddl_ptr_ok(const void *p)
{
   unsigned long a = (unsigned long)p;
   return a >= 0x100000UL && a < 0x100000000UL;
}

//get the device associated to a process
DEX32_DDL_INFO *Dex32GetProcessDevice(){
   PCB386 *cp;
   DEX32_DDL_INFO *d = 0;

   cp = smp_this_cpu()->current;
   if (ddl_ptr_ok(cp))
      d = cp->outdev;
   if (!ddl_ptr_ok(d))
      d = consoleDDL;
   if (!ddl_ptr_ok(d))
      return 0;
   return d;
};


//Create a new device
DEX32_DDL_INFO *Dex32CreateDDL(){
   DEX32_DDL_INFO *dev=(DEX32_DDL_INFO*)malloc(sizeof(DEX32_DDL_INFO));
   if (!dev)
      return 0;
   memset(dev,0,sizeof(DEX32_DDL_INFO));
   dev->size = sizeof(DEX32_DDL_INFO);
   totalDDL++;
   dev->handle=totalDDL;
   dev->buf_size=80*25*2*sizeof(char);
   dev->mem_ptr=(char*)malloc(80*25*2*sizeof(char));
   if (!dev->mem_ptr)
      return 0; 
   memset(dev->mem_ptr,0,80*25*2*sizeof(char));
    dev->buf_ptr=dev->mem_ptr;
    dev->hdw_ptr=0;
    if (fbconsole_use_legacy_vga(fbconsole_have_tag(),
                                serial_com1_present(),
                                fbconsole_active())) {
       dev->hdw_ptr=(char*)0xB8000;
    } else {
       /* GOP shadow, or a laptop with no VGA: never poke 0xB8000 / 0x3D4. */
       dev->hdw_ptr=(char*)malloc(80*25*2*sizeof(char));
       if (!dev->hdw_ptr) {
          free(dev->mem_ptr);
          return 0;
       }
       memset(dev->hdw_ptr,0,80*25*2*sizeof(char));
    }
    dev->type=DDL_CGA;
   dev->active=0;
   dev->locked=0;
   dev->lines=0;
   dev->scroll = 1;    
   Dex32SetTextColor(dev,WHITE);
   Dex32SetTextBackground(dev,BLACK);
    
   dev->curx=0;dev->cury=0;
   dev->hist=(console_hist_t*)malloc(sizeof(console_hist_t));
   if (dev->hist)
      console_hist_init(dev->hist);
   if (ActiveDDL==0) {
      Dex32SetActiveDDL(dev);
   };
    
   return dev;
};


//associate a console to a process
DEX32_DDL_INFO *Dex32SetProcessDDL(DEX32_DDL_INFO *dev, int pid){
   DEX32_DDL_INFO *ret=0;
   PCB386 *pcb=ps_findprocess(pid);
    
   if (pcb!=-1) {/*pid is valid*/
      ret = pcb->outdev;
      pcb->outdev = dev;
      return ret;
   };
   return 0;  
};

//swap to memory
void dd_swaptomemory(DEX32_DDL_INFO *dev){
   memcpy(dev->mem_ptr,dev->hdw_ptr,dev->buf_size);
   dev->buf_ptr=dev->mem_ptr;
   dev->bufmode = 1;               
};

//swap to hardware
void dd_swaptohardware(DEX32_DDL_INFO *dev){
    if (dev == ActiveDDL){
       memcpy(dev->hdw_ptr,dev->mem_ptr,dev->buf_size);
       dev->buf_ptr=dev->hdw_ptr;
       dev->bufmode = 0;
       fbconsole_screen_refresh();
    };
};


//Set the active ddl
DEX32_DDL_INFO *Dex32SetActiveDDL(DEX32_DDL_INFO *dev){
   DEX32_DDL_INFO *temp_ptr;
   if (dev != ActiveDDL){ //Is dev is already active?
      temp_ptr = ActiveDDL;
      if (ActiveDDL!=0){ /*If there is already an activeDLL*/
         //obtain the old active DDL and save the contents in the hardware buffer
         //into the memory buffer
         if (!ActiveDDL->bufmode){              
            memcpy(ActiveDDL->mem_ptr,ActiveDDL->hdw_ptr,ActiveDDL->buf_size);
            ActiveDDL->buf_ptr=ActiveDDL->mem_ptr;
         };
         //Deactive current DDL
         ActiveDDL->active=0;
      };
      
      //copy state of dev to the hardware
      memcpy(dev->hdw_ptr,dev->mem_ptr,dev->buf_size);
      dev->buf_ptr=dev->hdw_ptr;
      ActiveDDL = dev;
      
     //set as the active DDL
       dev->active=1;

       if (fbconsole_active())
          fbconsole_screen_refresh();

       if (!dev->bufmode && ddl_legacy_vga(dev))
          move_cursor(dev->cury,dev->curx);
       return temp_ptr;
   };
   return dev;
};

//Clear the device
void Dex32Clear(DEX32_DDL_INFO *dev){
    memset(dev->buf_ptr,0,dev->buf_size);
    dev->curx=0;dev->cury=0;dev->lines=0;
    if (dev==ActiveDDL && dev->active && !dev->bufmode)
       fbconsole_clear_screen();
};

//perform a scroll up
void Dex32ScrollUp(DEX32_DDL_INFO *dev){
    DWORD vidmemloc=dev->buf_ptr;
    if (dev->hist)
       console_hist_push(dev->hist, (unsigned char *)vidmemloc);
    memmove((void*)vidmemloc,(void*)vidmemloc+0x000A0,3840);
    if (dev==ActiveDDL && dev->active && !dev->bufmode)
       fbconsole_screen_refresh();
};

//set the text attribute
void Dex32SetTextAttr(DEX32_DDL_INFO *dev, char attr){
   dev->attb = attr;
};

//set the text color
void Dex32SetTextColor(DEX32_DDL_INFO *dev, char color){
   dev->attb&=0xF0;
   dev->attb|=color;
};

//move the cursor to a new location
void Dex32MoveCursor(DEX32_DDL_INFO *dev, int y, int x){
    if (dev->active && !dev->bufmode) {
       if (ddl_legacy_vga(dev))
          move_cursor(y,x);
       fbconsole_cursor_to(x,y);
    };
};

//Set the background
void Dex32SetTextBackground(DEX32_DDL_INFO *dev, char color){
   dev->attb&=0xF;
   color=color << 4;
   dev->attb|=color;
};

//set the scroll value
void Dex32SetScroll(DEX32_DDL_INFO *dev, int value){
   dev->scroll = value;
};

//next line
void Dex32NextLn(DEX32_DDL_INFO *dev){
   DWORD vidmemloc=dev->buf_ptr;
   dev->cury++;
   dev->lines++;
   if (dev->cury>=25){
      dev->cury=24;
      if (dev->scroll){
         Dex32ScrollUp(dev);
         memset(vidmemloc+0x00F00,0,80*2);
      };
   };
                 
   Dex32PutChar(dev,dev->curx,dev->cury,' ',dev->attb);
    if (dev->active && !dev->bufmode && ddl_legacy_vga(dev))
       move_cursor(dev->cury,dev->curx);
    /* The scroll refresh (above) ran before the bottom line was cleared;
       redraw so the cleared line is not stale on the framebuffer. */
    if (dev->active && !dev->bufmode && dev->cury==24)
       fbconsole_screen_refresh();
};

//update the cursor position
void Dex32UpdateCursor(DEX32_DDL_INFO *dev, int y, int x){
    if (dev->active && !dev->bufmode) {
       if (ddl_legacy_vga(dev))
          move_cursor(y,x);
       fbconsole_cursor_to(x,y);
    };
    dev->curx = x;
    dev->cury = y;
};

//Emulates an ANSI compatible display subsystem
void Dex32PutC(DEX32_DDL_INFO *dev, char c){
   /* Serial mirror lives in putcEX to avoid double COM1 output. */
   if (!ddl_ptr_ok(dev))
      return;
   if (c=='\t'){
      int i;
      for (i=0;i<3;i++)
         Dex32PutC(dev,' ');
      return;
   }else if (c=='\b'){
      if (dev->curx>0)
         dev->curx--;
      Dex32UpdateCursor(dev,dev->cury,dev->curx);
   }else if (c=='\r'){
      dev->curx=0;
      Dex32UpdateCursor(dev,dev->cury,dev->curx);
   }else if (c=='\n'){
      dev->curx=0;
      Dex32NextLn(dev);
   }else{
      Dex32PutChar(dev,dev->curx,dev->cury,c,dev->attb);
      dev->curx++;
      Dex32PutChar(dev,dev->curx,dev->cury,' ',dev->attb);
        
      if (dev->active)        
         Dex32UpdateCursor(dev,dev->cury,dev->curx);
   };
        
   if (dev->curx>79){
      dev->curx=0; 
      Dex32NextLn(dev);
   };
};

//return x position
int Dex32GetX(DEX32_DDL_INFO *dev){
   return dev->curx;
};

//return cursor y
int Dex32GetY(DEX32_DDL_INFO *dev){
   return dev->cury;
};

//set the cursor x
void Dex32SetX(DEX32_DDL_INFO *dev,int x){
   dev->curx=x;
};

//set the cursor y
void Dex32SetY(DEX32_DDL_INFO *dev,int y){
   dev->cury=y;
};

//output a character at position x,y
int Dex32PutChar(DEX32_DDL_INFO *dev,int x, int y,char c,char color){
   char *cptr;
   DWORD vidmemloc;
   if (!ddl_ptr_ok(dev))
      return 0;
   vidmemloc=dev->buf_ptr;
   
  if (x>=0&&x<80 &&y>=0&&y <25){
       cptr=(char*)(vidmemloc+ (y * 80 + x) * 2);
       *cptr=c;
       *(cptr+1)=color;
       if (dev==ActiveDDL && dev->active && !dev->bufmode)
          fbconsole_cell_render(x,y,(unsigned char)c,(unsigned char)color);
    };
};

//outoyt text
int Dex32PutText(DEX32_DDL_INFO *dev,int left, int top, int right, 
                  int bottom, char *source){
   
   DWORD vidmemloc=dev->buf_ptr;
   int i,i2,i3=0;
  
  for (i2=top;i2<=bottom;i2++){
       for (i=left;i<=right;i++){
          char *cptr;
          cptr=(char*)(vidmemloc+ (i2 * 80 + i) * 2);
          *cptr = source[i3];
          i3++;
          *(cptr+1) = source[i3];
          i3++;
       };
    }
    if (dev==ActiveDDL && dev->active && !dev->bufmode)
       fbconsole_screen_refresh();
};


//retrieve the text at a given position
int Dex32GetText(DEX32_DDL_INFO *dev,int left, int top, int right, 
                  int bottom, char *destin){

   DWORD vidmemloc=dev->buf_ptr;
   int i,i2,i3=0;

   for (i2=top;i2<=bottom;i2++){
      for (i=left;i<=right;i++){
         char *cptr;
         cptr=(char*)(vidmemloc+ (i2 * 80 + i) * 2);
         destin[i3] = *cptr;
         i3++;
         destin[i3] = *(cptr+1);
         i3++;
      };
   }
}; 

//get the attribute of the device
int Dex32GetAttb(DEX32_DDL_INFO *dev){
   return dev->attb;
};


//destroy the device
int Dex32FreeDDL(DEX32_DDL_INFO *dev){
   if (ActiveDDL!=dev){
      if (dev && dev->hist)
         free(dev->hist);
      free(dev);
      return 0;
   };
   return -1;
};

//set the device for current process
void Dex32SetDDL(DEX32_DDL_INFO *dev){
   if (current_process->outdev!=dev){
      current_process->outdev=dev;
   };
};
