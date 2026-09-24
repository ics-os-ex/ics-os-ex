/*
 * Userland ICS-OS shell. I/O is stdin/stdout (tty fds 0/1).
 * Unknown commands go to sys_kcmd so kernel builtins still work.
 */
#include "../../sdk/dexsdk.h"

int execp(char *fname, unsigned short mode, char *params);
char *strcat(char *s, const char *append);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);

#define FXN_SYSREAD  0xA4
#define FXN_SYSWRITE 0xA5
#define FXN_KCMD     0xA6

static int kcmd(const char *s)
{
   return (int)dexsdk_systemcall(FXN_KCMD, (long)s, 0, 0, 0, 0);
}

static int getline(char *buf, int n)
{
   int i = 0;
   while (i < n - 1) {
      char c;
      long r = dexsdk_systemcall(FXN_SYSREAD, 0, (long)&c, 1, 0, 0);
      if (r <= 0)
         break;
      if (c == 3)
         return 0;
      buf[i++] = c;
      if (c == '\n' || c == '\r')
         break;
   }
   while (i > 0 && (buf[i-1] == '\n' || buf[i-1] == '\r'))
      i--;
   buf[i] = 0;
   return i;
}

static void putstr(const char *s)
{
   dexsdk_systemcall(FXN_SYSWRITE, 1, (long)s, (long)strlen(s), 0, 0);
}

int main(int argc, char **argv)
{
   char line[256];
   (void)argc;
   (void)argv;
   putstr("ICS-OS sh (userland). Type help. Ctrl-C interrupts.\r\n");
   for (;;) {
      char *cmd;
      putstr("sh$ ");
      if (getline(line, sizeof(line)) <= 0)
         break;
      cmd = line;
      while (*cmd == ' ')
         cmd++;
      if (cmd[0] == 0 || cmd[0] == '\n' || cmd[0] == '\r')
         continue;
      if (strncmp(cmd, "exit", 4) == 0)
         break;
      if (strncmp(cmd, "echo ", 5) == 0) {
         putstr(cmd + 5);
         if (!strchr(cmd, '\n'))
            putstr("\r\n");
         continue;
      }
      if (strncmp(cmd, "help", 4) == 0) {
         kcmd("help");
         continue;
      }
      {
         char name[128], path[192];
         int j = 0;
         while (cmd[j] && cmd[j] != ' ' && cmd[j] != '\n' && cmd[j] != '\r' && j < 127) {
            name[j] = cmd[j];
            j++;
         }
         name[j] = 0;
         strcpy(path, "/icsos/apps/");
         strcat(path, name);
         if (!strchr(name, '.') && j < 120)
            strcat(path, ".exe");
         if (execp(path, 0, cmd) == 0)
            kcmd(cmd);
      }
   }
   return 0;
}
