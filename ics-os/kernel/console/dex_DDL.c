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


//get the device associated to a process
DEX32_DDL_INFO *Dex32GetProcessDevice(){
   if (current_process->outdev == 0 || current_process == 0 ) 
      return consoleDDL;
   return current_process->outdev;
};


//Create a new device
DEX32_DDL_INFO *Dex32CreateDDL(){
   DEX32_DDL_INFO *dev=(DEX32_DDL_INFO*)malloc(sizeof(DEX32_DDL_INFO));
   memset(dev,0,sizeof(DEX32_DDL_INFO));
   dev->size = sizeof(DEX32_DDL_INFO);
   totalDDL++;
   dev->handle=totalDDL;
   dev->buf_size=80*25*2*sizeof(char);
   dev->mem_ptr=(char*)malloc(80*25*2*sizeof(char));    
   memset(dev->mem_ptr,0,80*25*2*sizeof(char));
   dev->buf_ptr=dev->mem_ptr; 
   dev->hdw_ptr=(char*)0xB8000;
   dev->type=DDL_CGA;
   dev->active=0;
   dev->locked=0;
   dev->lines=0;
   dev->scroll = 1;    
   Dex32SetTextColor(dev,WHITE);
   Dex32SetTextBackground(dev,BLACK);
    
   dev->curx=0;dev->cury=0;
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
      
      if (!dev->bufmode)
         move_cursor(dev->cury,dev->curx);
      return temp_ptr;
   };
   return dev;
};

//Clear the device
void Dex32Clear(DEX32_DDL_INFO *dev){
   memset(dev->buf_ptr,0,dev->buf_size);
   dev->curx=0;dev->cury=0;dev->lines=0;
};

//perform a scroll up
void Dex32ScrollUp(DEX32_DDL_INFO *dev){
   DWORD vidmemloc=dev->buf_ptr;
   memmove((void*)vidmemloc,(void*)vidmemloc+0x000A0,3840);
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
   if (dev->active && !dev->bufmode) 
      move_cursor(y,x);
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
   if (dev->active && !dev->bufmode) 
      move_cursor(dev->cury,dev->curx);
};

static int ansi_parse_int(const char **s, int defval){
   int val = 0;
   int got = 0;
   while (**s >= '0' && **s <= '9') {
      val = val * 10 + (**s - '0');
      (*s)++;
      got = 1;
   }
   if (!got)
      return defval;
   return val;
}

static void ansi_apply_sgr(DEX32_DDL_INFO *dev, const char *params){
   const char *p = params;
   int any = 0;
   while (*p) {
      int v = ansi_parse_int(&p, 0);
      any = 1;
      if (v == 0) {
         Dex32SetTextAttr(dev, 0x07);
      } else if (v == 1) {
         Dex32SetTextColor(dev, (dev->attb & 0x0F) | 0x08);
      } else if (v >= 30 && v <= 37) {
         Dex32SetTextColor(dev, v - 30);
      } else if (v >= 40 && v <= 47) {
         Dex32SetTextBackground(dev, v - 40);
      } else if (v >= 90 && v <= 97) {
         Dex32SetTextColor(dev, (v - 90) + 8);
      } else if (v >= 100 && v <= 107) {
         Dex32SetTextBackground(dev, (v - 100) + 8);
      }
      if (*p == ';')
         p++;
   }
   if (!any)
      Dex32SetTextAttr(dev, 0x07);
}

static void ansi_clear_to_eol(DEX32_DDL_INFO *dev){
   int x;
   for (x = dev->curx; x < 80; x++)
      Dex32PutChar(dev, x, dev->cury, ' ', dev->attb);
}

static void ansi_clear_screen(DEX32_DDL_INFO *dev){
   Dex32Clear(dev);
   dev->curx = 0;
   dev->cury = 0;
}

//update the cursor position
void Dex32UpdateCursor(DEX32_DDL_INFO *dev, int y, int x){
   if (dev->active && !dev->bufmode) 
      move_cursor(y,x);
   dev->curx = x;
   dev->cury = y;
};

//Emulates an ANSI compatible display subsystem
void Dex32PutC(DEX32_DDL_INFO *dev, char c){
   if (dev->ansi_command_ptr) {
      if (dev->ansi_command_ptr == 1) {
         if (c == '[') {
            dev->ansi_command_ptr = 2;
            dev->ansi_cmd[0] = 0;
            return;
         }
         dev->ansi_command_ptr = 0;
      } else {
         int idx = dev->ansi_command_ptr - 2;
         if ((c >= '0' && c <= '9') || c == ';' || c == '?' ) {
            if (idx < (int)sizeof(dev->ansi_cmd) - 2) {
               dev->ansi_cmd[idx] = c;
               dev->ansi_cmd[idx + 1] = 0;
               dev->ansi_command_ptr++;
            }
            return;
         }

         if (dev->ansi_cmd[0] == '?') {
            dev->ansi_command_ptr = 0;
            return;
         }

         switch (c) {
            case 'm':
               ansi_apply_sgr(dev, dev->ansi_cmd);
               break;
            case 'H':
            case 'f': {
               const char *p = dev->ansi_cmd;
               int row = ansi_parse_int(&p, 1);
               if (*p == ';') p++;
               int col = ansi_parse_int(&p, 1);
               if (row < 1) row = 1;
               if (col < 1) col = 1;
               if (row > 25) row = 25;
               if (col > 80) col = 80;
               dev->cury = row - 1;
               dev->curx = col - 1;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            }
            case 'A': {
               const char *p = dev->ansi_cmd;
               int n = ansi_parse_int(&p, 1);
               dev->cury = (dev->cury >= (DWORD)n) ? (dev->cury - n) : 0;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            }
            case 'B': {
               const char *p = dev->ansi_cmd;
               int n = ansi_parse_int(&p, 1);
               dev->cury += n;
               if (dev->cury > 24) dev->cury = 24;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            }
            case 'C': {
               const char *p = dev->ansi_cmd;
               int n = ansi_parse_int(&p, 1);
               dev->curx += n;
               if (dev->curx > 79) dev->curx = 79;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            }
            case 'D': {
               const char *p = dev->ansi_cmd;
               int n = ansi_parse_int(&p, 1);
               dev->curx = (dev->curx >= (DWORD)n) ? (dev->curx - n) : 0;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            }
            case 'J': {
               const char *p = dev->ansi_cmd;
               int n = ansi_parse_int(&p, 0);
               if (n == 2 || n == 0)
                  ansi_clear_screen(dev);
               break;
            }
            case 'K': {
               ansi_clear_to_eol(dev);
               break;
            }
            case 's':
               dev->ansi_x = dev->curx;
               dev->ansi_y = dev->cury;
               break;
            case 'u':
               dev->curx = dev->ansi_x;
               dev->cury = dev->ansi_y;
               Dex32UpdateCursor(dev, dev->cury, dev->curx);
               break;
            default:
               break;
         }

         dev->ansi_command_ptr = 0;
         return;
      }
   }

   if (c == 27) {
      dev->ansi_command_ptr = 1;
      dev->ansi_cmd[0] = 0;
      return;
   }
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
   DWORD vidmemloc=dev->buf_ptr;
   
   if (x>=0&&x<80 &&y>=0&&y <25){
      cptr=(char*)(vidmemloc+ (y * 80 + x) * 2);
      *cptr=c;
      *(cptr+1)=color;
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
