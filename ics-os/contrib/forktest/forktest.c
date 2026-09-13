#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile int fork_data=7;
static volatile int oom_data=41;
#define FORK_STRESS_CHILDREN 10

static int fail(const char *reason)
{
   printf("FORK_FAIL %s\n",reason);
   return 1;
}

/* fork() while the syscall's saved-register + iretq frame straddles a page
   boundary.  The child resumes through fork_child_return, which does POP_ALL
   then iretq over 160 bytes at the recorded user RSP; userpd_clone_cow() used
   to make only the page holding the rax slot private to the child, so when the
   frame spanned two pages the tail page -- holding the child's RIP, CS and
   RFLAGS -- stayed COW-shared with a parent that immediately reused that stack.
   The child then iretq'd over reused values and jumped into the low page,
   reported as "UD64 rip=0x207" (RFLAGS popped as RIP) and killed mid-compile
   during the -j4 GCC bootstrap.  Walking RSP across a whole page in 16-byte
   steps guarantees the straddling alignments are hit, so this fails for the
   original cause rather than waiting for a lucky stack layout. */
static int fork_straddle_one(int depth)
{
   volatile char pad[16];
   int status=-1;
   int pid;

   pad[0]=(char)depth;
   if (depth > 0)
      return fork_straddle_one(depth-1);

   pid=fork();
   if (pid<0)
      return -1;
   if (pid==0)
      _exit(77);
   if (waitpid(pid,&status,0)!=pid || status!=77)
      return -2;
   return (int)pad[0];
}

static int fork_straddle_page(void)
{
   int depth;
   int rc;

   for (depth=0; depth<272; depth++) {
      rc=fork_straddle_one(depth);
      if (rc==-1) {
         printf("FORK_FAIL straddle-fork depth=%d\n",depth);
         return -1;
      }
      if (rc==-2) {
         printf("FORK_FAIL straddle-wait depth=%d\n",depth);
         return -1;
      }
   }
   printf("FORK_STRADDLE_PASS depths=%d\n",depth);
   return 0;
}

static int run_oom_test(void)
{
   int status=-1;
   int pid=fork();
   if (pid<0)
      return fail("oom-fork");
   if (pid==0) {
      oom_data=99;
      return fail("oom-survived");
   }
   if (waitpid(pid,&status,0)!=pid || status==0 || oom_data!=41)
      return fail("oom-wait");
   printf("FORK_COW_OOM_PASS status=%d data=%d\n",status,oom_data);
   return 0;
}

int main(int argc,char **argv)
{
   int local=11;
   int status=-1;
   int fd;
   int pid;
   int pids[FORK_STRESS_CHILDREN];
   int index;
   int parent_pid=getpid();
   char buf[8];

   if (argc>1 && strcmp(argv[1],"--oom")==0)
      return run_oom_test();

   fd=open("/work/fork.txt",O_RDWR|O_CREAT|O_TRUNC,0666);
   if (fd<0)
      return fail("open");

   pid=fork();
   if (pid<0)
      return fail("syscall");
   if (pid==0) {
      if (waitpid(parent_pid,&status,0)!=-1)
         return fail("non-child-wait");
      fork_data=19;
      local=23;
      if (write(fd,"child",5)!=5 || fsync(fd)!=0)
         return fail("child-fd");
      printf("FORK_CHILD_PASS data=%d stack=%d\n",fork_data,local);
      _exit(23);
      return 23;
   }

   fork_data=13;
   local=17;
   if (fork_data!=13 || local!=17)
      return fail("isolation");
   if (waitpid(pid,&status,0)!=pid || status!=23)
      return fail("waitpid");
   if (fork_data!=13 || local!=17)
      return fail("post-wait-isolation");
   if (lseek(fd,0,SEEK_SET)!=0)
      return fail("lseek");
   memset(buf,0,sizeof(buf));
   if (read(fd,buf,5)!=5 || memcmp(buf,"child",5)!=0)
      return fail("inherited-fd");
   close(fd);
   printf("FORK_PARENT_PASS child=%d status=%d\n",pid,status);

   pid=fork();
   if (pid<0)
      return fail("text-fork");
   if (pid==0) {
      *(volatile unsigned char *)(unsigned long)&main^=1;
      return fail("text-write");
   }
   status=0;
   if (waitpid(pid,&status,0)!=pid || status==0)
      return fail("text-wait");
   printf("FORK_TEXT_FAULT_PASS status=%d\n",status);

   for (index=0;index<FORK_STRESS_CHILDREN;index++) {
      pids[index]=fork();
      if (pids[index]<0)
         return fail("stress-fork");
      if (pids[index]==0)
         _exit(40+index);
   }
   for (index=0;index<FORK_STRESS_CHILDREN;index++) {
      status=-1;
      if (waitpid(pids[index],&status,0)!=pids[index]
          || status!=40+index)
         return fail("stress-wait");
   }
   fork_data=31;
   if (fork_data!=31)
      return fail("single-owner");
   printf("FORK_COW_PASS shared-write=13 single-owner=%d\n",fork_data);
   printf("FORK_STRESS_PASS count=%d\n",FORK_STRESS_CHILDREN);
   if (fork_straddle_page()<0)
      return 1;
   printf("FORK_PASS\n");
   return 0;
}