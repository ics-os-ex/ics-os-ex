/*
  Name: foreground.c
  Copyright: 
  Author: Joseph Emmanuel DL Dayo
  Date: 05/01/04 04:39
  Description: Allows the management of multiple screens and controls the use of the
               foreground.
               
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

/*Initialize the virtual console table*/
void fg_init(){
   int i;
    
   for (i=0; i < FG_MAXCONSOLE; i++)
      fg_vconsoles[i] = 0;
    
   memset(&fg_busywait,0,sizeof(fg_busywait));
};

//toggles the state of the virtual console
int fg_toggle(){
  fg_set_state(!fg_state);
};


static int fg_last = 0;
static int fg_prefix = 0;

static void fg_put_status_cells(char *base, const char *msg, unsigned char attr,
                                int render)
{
    int i;
    int ended = 0;
    for (i = 0; i < 80; i++) {
       char ch = ' ';
       if (!ended && msg && msg[i])
          ch = msg[i];
       else
          ended = 1;
       if (base) {
          base[(24 * 80 + i) * 2] = ch;
          base[(24 * 80 + i) * 2 + 1] = (char)attr;
       }
       if (render)
          fbconsole_cell_render(i, 24, (unsigned char)ch, attr);
    }
}

static void fg_vga_status(const char *msg, unsigned char attr)
{
    if (fbconsole_active() && ActiveDDL) {
       char *base = ActiveDDL->bufmode ? ActiveDDL->hdw_ptr : ActiveDDL->buf_ptr;
       fg_put_status_cells(base, msg, attr, 1);
       return;
    }
    fg_put_status_cells((char *)0xB8000, msg, attr, 0);
}

void fg_status(const char *msg)
{
   fg_vga_status(msg, 0x1F); /* white on blue */
}

static void fg_status_windows(const char *hint)
{
   char line[FG_STATUS_COLS + 1];
   const char *names[FG_MAXCONSOLE];
   int ids[FG_MAXCONSOLE];
   int i, nwin = 0;
   for (i = 0; i < FG_MAXCONSOLE; i++) {
      if (fg_vconsoles[i] && !fg_vconsoles[i]->ignore) {
         PCB386 *p = ps_findprocess(fg_vconsoles[i]->pid);
         ids[nwin] = i;
         if (p && p != (PCB386 *)-1)
            names[nwin] = p->name;
         else
            names[nwin] = 0;
         nwin++;
      }
   }
   fg_format_status(line, sizeof line, fg_current, nwin, ids, names, hint);
   fg_status(line);
}

static int fg_copy;
static int fg_copy_took_buf;
static unsigned int fg_view_off;

static void fg_blit_cells(DEX32_DDL_INFO *dev, int y, const unsigned char *cells)
{
   int x;
   char *hdw;
   if (!dev || y < 0 || y >= 24 || !cells)
      return;
   hdw = dev->hdw_ptr;
   for (x = 0; x < 80; x++) {
      unsigned char ch = cells[x * 2];
      unsigned char at = cells[x * 2 + 1];
      if (hdw) {
         hdw[(y * 80 + x) * 2] = (char)ch;
         hdw[(y * 80 + x) * 2 + 1] = (char)at;
      }
      fbconsole_cell_render(x, y, ch, at);
   }
}

static void fg_copy_paint(void)
{
   DEX32_DDL_INFO *dev = ActiveDDL;
   console_hist_t *h;
   unsigned int y, live_row;
   char bar[FG_STATUS_COLS + 1];
   unsigned int n = 0;
   unsigned int i;
   if (!dev || !fg_copy)
      return;
   for (i = 0; i < sizeof bar; i++)
      bar[i] = 0;
   h = dev->hist;
   fg_view_off = console_hist_clamp_off(fg_view_off, h ? h->count : 0);
   for (y = 0; y < CONSOLE_VIEW_ROWS; y++) {
      int slot = -1;
      live_row = y;
      if (h)
         slot = console_hist_view_slot(h, fg_view_off, y, CONSOLE_VIEW_ROWS,
                                       &live_row);
      if (slot >= 0)
         fg_blit_cells(dev, (int)y, h->lines[slot]);
      else if (dev->mem_ptr)
         fg_blit_cells(dev, (int)y,
                       (unsigned char *)dev->mem_ptr + live_row * 160);
   }
   n = fg_status_append(bar, 0, sizeof bar, "[");
   n = fg_status_uint(bar, n, sizeof bar, (unsigned int)fg_current);
   n = fg_status_append(bar, n, sizeof bar, "] COPY ");
   n = fg_status_uint(bar, n, sizeof bar, fg_view_off);
   n = fg_status_append(bar, n, sizeof bar, "/");
   n = fg_status_uint(bar, n, sizeof bar, h ? h->count : 0);
   n = fg_status_append(bar, n, sizeof bar,
                        "  q/Esc quit  Up/Dn  PgUp/PgDn");
   (void)n;
   fg_status(bar);
}

static void fg_copy_leave(void)
{
   DEX32_DDL_INFO *dev = ActiveDDL;
   if (!fg_copy)
      return;
   fg_copy = 0;
   fg_view_off = 0;
   if (fg_copy_took_buf && dev)
      dd_swaptohardware(dev);
   else if (dev && fbconsole_active())
      fbconsole_screen_refresh();
   fg_copy_took_buf = 0;
   fg_status_windows(0);
}

static void fg_copy_enter(int delta)
{
   DEX32_DDL_INFO *dev = ActiveDDL;
   unsigned int count;
   if (!dev)
      return;
   if (!fg_copy) {
      if (!dev->bufmode) {
         dd_swaptomemory(dev);
         fg_copy_took_buf = 1;
      } else
         fg_copy_took_buf = 0;
      fg_copy = 1;
      fg_view_off = 0;
   }
   count = (dev->hist) ? dev->hist->count : 0;
   fg_view_off = console_hist_add_off(fg_view_off, delta, count);
   fg_copy_paint();
}

static int fg_copy_key(int c)
{
   if (c == 'q' || c == 27 || c == '\n' || c == 'i') {
      fg_copy_leave();
      return 1;
   }
   if (c == KEY_UP)
      fg_copy_enter(1);
   else if (c == KEY_DN)
      fg_copy_enter(-1);
   else if (c == KEY_PGUP)
      fg_copy_enter((int)CONSOLE_VIEW_ROWS);
   else if (c == KEY_PGDN)
      fg_copy_enter(-(int)CONSOLE_VIEW_ROWS);
   else if (c == KEY_HOME)
      fg_copy_enter(100000);
   else if (c == KEY_END) {
      fg_view_off = 0;
      fg_copy_paint();
   }
   return 1;
}

//Sets the foreground console
int fg_setforeground(int num){
   DWORD cpuflags;
   int ret = -1;

   if (fg_copy)
      fg_copy_leave();
    
   dex32_stopints(&cpuflags);
    
   if (num < FG_MAXCONSOLE){
      if (fg_vconsoles[num] != 0){
         DEX32_DDL_INFO *fgDDL= fg_vconsoles[num]->screen;
         ret = fg_current;
         if (num != fg_current)
            fg_last = fg_current;
         Dex32SetActiveDDL(fgDDL);
         fg_current = num;
         if (fg_vconsoles[num]->tty)
            tty_set_fg(fg_vconsoles[num]->tty);
      };
   };
    
   dex32_restoreints(cpuflags);
   if (ret != -1)
      fg_status_windows(0);
   return ret;
};

static int fg_usable(int i)
{
   return i >= 0 && i < FG_MAXCONSOLE && fg_vconsoles[i]
          && !fg_vconsoles[i]->ignore;
}

//move to previous console (wraps, tmux C-b p)
void fg_prev(){
   int i;
   for (i = 1; i <= FG_MAXCONSOLE; i++) {
      int n = (fg_current - i + FG_MAXCONSOLE) % FG_MAXCONSOLE;
      if (fg_usable(n)) {
         fg_setforeground(n);
         return;
      }
   }
};

//moves to the next virtual console (wraps, tmux C-b n)
void fg_next(){
   int i;
   for (i = 1; i <= FG_MAXCONSOLE; i++) {
      int n = (fg_current + i) % FG_MAXCONSOLE;
      if (fg_usable(n)) {
         fg_setforeground(n);
         return;
      }
   }
};

static void fg_lastwin(void)
{
   if (fg_usable(fg_last))
      fg_setforeground(fg_last);
   else
      fg_next();
}

static void fg_kill_current(void)
{
   int pid, i, others = 0;
   if (!fg_usable(fg_current))
      return;
   for (i = 0; i < FG_MAXCONSOLE; i++)
      if (fg_usable(i) && i != fg_current)
         others++;
   pid = fg_vconsoles[fg_current]->pid;
   if (!others) {
      /* Last window: kill and respawn so a wedged `ls`/VFS mount can be
         recovered without power-cycling (F4 does the same). */
      if (pid)
         dex32_killkthread(pid);
      console_new();
      fg_status("restarted");
      return;
   }
   fg_next();
   if (pid)
      dex32_killkthread(pid);
}

/* tmux-style prefix C-b (keyboard encodes Ctrl-A=0, Ctrl-B=1, Ctrl-C=2). */
#define FG_PREFIX  ('b' - 'a')

int fg_mux_key(int c)
{
   tty_t *t;
   if (fg_copy)
      return fg_copy_key(c);
   t = tty_fg();
   /* Shell (no alternate screen): PageUp opens scrollback. vim/less keep PgUp. */
   if (c == KEY_PGUP && (!t || !t->vt.alt)) {
      fg_copy_enter((int)CONSOLE_VIEW_ROWS);
      return 1;
   }
   if (!fg_prefix) {
      if (c == FG_PREFIX) {
         fg_prefix = 1;
         fg_status_windows("C-b [ scroll  c n p l w x ?");
         return 1;
      }
      return 0;
   }
   fg_prefix = 0;
   if (c == FG_PREFIX) {
      /* C-b C-b: send literal Ctrl-B to the tty. */
      tty_input_fg(2);
      return 1;
   }
   if (c == '[' || c == KEY_PGUP) {
      fg_copy_enter((int)CONSOLE_VIEW_ROWS);
      return 1;
   }
   c = fg_mux_letter(c);
   if (c == 'c') {
      console_new();
      fg_status_windows("new");
      return 1;
   }
   if (c == 'n') {
      fg_next();
      return 1;
   }
   if (c == 'p') {
      fg_prev();
      return 1;
   }
   if (c == 'l') {
      fg_lastwin();
      return 1;
   }
   if (c == 'w' || c == 's') {
      fg_set_state(1);
      return 1;
   }
   if (c == 'x') {
      fg_kill_current();
      return 1;
   }
   if (c == '?' || c == '/') {
      fg_status("C-b [ scroll | c new | n/p | l last | 0-9 | w list | x kill | C-b C-b send");
      return 1;
   }
   if (c >= '0' && c <= '9') {
      int n = c - '0';
      if (fg_usable(n))
         fg_setforeground(n);
      else
         fg_status("no such window");
      return 1;
   }
   fg_status_windows(0);
   return 1;
}


//grab the keyboard
void fg_setmykeyboard(int pid){
   fg_processinfo *ptr = fg_getinfo(getprocessid());
   if (ptr!=0){
      ptr->keyboardfocus = pid;
   };
};

//get console for the process
fg_processinfo *fg_getinfo(int pid){
   int i;
   for (i=0;i<FG_MAXCONSOLE; i++){
      if (fg_vconsoles[i]!=0){
         if (fg_vconsoles[i]->pid == pid) 
            return fg_vconsoles[i];
      };
   };
   return 0;
};


//get my console
fg_processinfo *fg_getmyinfo(){
   return fg_getinfo(getprocessid());
};


//regsiter a new console
fg_processinfo *fg_register(DEX32_DDL_INFO *scr, int keyboard){
   //first check if there is any slot for another virtual console
   int i, slot = -1;
   DWORD cpuflags;
   fg_processinfo *new_vconsole;

   for (i=0; i < FG_MAXCONSOLE; i++){
      if (fg_vconsoles[i] == 0) {
         slot = i;
         break;
      };
   };

   //oops! no more slots left, return with error
   if (slot == -1) 
      return -1; 

   new_vconsole = (fg_processinfo*) malloc(sizeof(fg_processinfo));
   memset(new_vconsole, 0, sizeof(fg_processinfo));

   /*Lock when entering a mutual exclusion section*/
   dex32_stopints(&cpuflags);

   new_vconsole->id = slot;
   new_vconsole->size = sizeof(fg_processinfo);
   new_vconsole->screen = scr;
   new_vconsole->keyboardfocus = keyboard;
   new_vconsole->pid = getprocessid();
   new_vconsole->tty = 0;
   fg_vconsoles[slot] = new_vconsole;

   dex32_restoreints(cpuflags);

   return new_vconsole;
};


//exit the console
int fg_exit(){
   DWORD cpuflags;
   fg_processinfo *ptr;    

   dex32_stopints(&cpuflags);
    
   ptr = fg_getmyinfo();
    
   if (ptr!=0){
      fg_vconsoles[ptr->id] = 0;
      free(ptr);
      fg_prev();
   };
        
   dex32_restoreints(cpuflags);
        
};

//get the keyboard owner
int fg_getkeyboardowner(){
   if (fg_vconsoles[fg_current]!=0)
      return fg_vconsoles[fg_current]->keyboardfocus;
   else
      return 0; 
};

//show the menu
void fg_showmenu(int choice){
   int i;
   DWORD cpuflags;

   fg_processinfo screens[FG_MAXCONSOLE];

   dex32_stopints(&cpuflags);

   for (i=0; i < FG_MAXCONSOLE; i++){
      if (fg_vconsoles[i])
         memcpy(&screens[i], fg_vconsoles[i], sizeof(fg_processinfo));
      else
         screens[i].id = -1;
   };

   dex32_restoreints(cpuflags);

   for (i=0; i < FG_MAXCONSOLE; i++){
      char pname[255];
      
      if (choice == i) 
         textbackground(BLUE);
      else 
         textbackground(BLACK);
                  
      if (screens[i].id != -1){
         PCB386 *process=ps_findprocess(screens[i].pid);
         if (process != -1){
            PCB386 *subprocess = ps_findprocess(screens[i].keyboardfocus);
            strcpy(pname,process->name);
                  
            if (screens[i].keyboardfocus != screens[i].pid && subprocess!=0)
               sprintf(pname,"%s (%s)",process->name,subprocess->name);   
                  
            if (screens[i].ignore)
                textcolor(RED);
            else
               textcolor(WHITE);
                  
            printf("[%2d] %-73s\n",i, pname);
            
            textbackground(BLACK);
         }else{
            printf("[%2d] unknown\n",i);
         }
      }else{
         printf("[%2d] unused\n", i+1);
      }
   };
};

//set the state
void fg_set_state(int state){
   static int fg_before;

   if (fg_started){
      fg_state = state;
      if (state) 
         fg_before=fg_setforeground(fg_myslot);
      else
         fg_setforeground(fg_before);
   };
};

#define KEY_UP    151
#define KEY_DOWN  152

extern volatile int selfhost_cooperative_ready;

//
void fg_updateinfo(){
    int refreshrate=100; //determines how long in milliseconds until the task manager refreshes its display
    int loop;
    int choice = 0;
    
   //Create a screen buffer that we can use   
   fg_out = Dex32CreateDDL();
 
   //register myself
   fg_fginfo = fg_register(fg_out,getprocessid());
 
   fg_myslot = fg_fginfo->id;
 
   //prevent F5 and F6 from setting this process as the foreground
   fg_fginfo->ignore = 1; 
 
   Dex32SetProcessDDL(fg_out,getprocessid());
   fg_started = 1;

   while(1){
      if (fg_state){
         dd_swaptomemory(fg_out);
         clrscr();
         textbackground(BLUE);
         textcolor(WHITE);
         printf("%-79s\n","Dex32- Virtual Console Management");
         textbackground(BLACK);
        
         //Display processes in memory
         fg_showmenu(choice);
        
         dd_swaptohardware(fg_out);
         
         if (kb_keypressed()){
            unsigned char c = getch();
            
            if (c-'0' >= 0 && c-'0' <= 9){
               int choice = c - '0';
               if (fg_vconsoles[choice]!=0)
                  if (!fg_vconsoles[choice]->ignore)
                     fg_setforeground(choice);
                 
            }else if (c == KEY_UP){
               if (choice>0) 
                  choice-- ;
            }else if (c == KEY_DOWN){
               if (choice<FG_MAXCONSOLE -1) 
                  choice++;
            }else if (c== '\n'){
               if (fg_vconsoles[choice]!=0)
                  if (!fg_vconsoles[choice]->ignore)
                     fg_setforeground(choice);
            };      
        };
      } else {
          /* In cooperative self-host mode a bare hlt never yields; if a
             higher-priority user tool was just enqueued on this CPU it would
             remain stuck behind the foreground kernel thread. */
          if (selfhost_cooperative_ready)
             taskswitch();
          cpu_idle();
       }
    };
};


