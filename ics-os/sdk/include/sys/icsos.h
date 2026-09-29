#ifndef _SYS_ICSOS_H
#define _SYS_ICSOS_H

/*
 * ICS-OS process and system statistics interface.
 *
 * NOTE: the struct layouts must stay byte-identical to the kernel copies in
 * kernel/process/process.c (search for `struct icsos_procinfo`).
 */

#define ICSOS_PROC_NAMELEN 32
#define ICSOS_MAX_CPUS 8

#define ICSOS_ST_RUNNING  1
#define ICSOS_ST_BLOCKED  2
#define ICSOS_ST_DYING    4
#define ICSOS_ST_THREAD   8
#define ICSOS_ST_KERNEL   16
#define ICSOS_ST_DRIVER   32

struct icsos_procinfo {
    unsigned int pid;
    unsigned int ppid;
    char name[ICSOS_PROC_NAMELEN];
    unsigned int state;
    unsigned int priority;
    unsigned int cpu_affinity;
    unsigned int on_cpu;
    unsigned long long totalcputime;
    unsigned long long arrivaltime;
    unsigned long long rss_pages;
    unsigned int pad;
};

struct icsos_sysinfo {
    unsigned int uptime_ticks;
    unsigned int hz;
    unsigned int ncpu;
    unsigned int total_procs;
    unsigned long long total_pages;
    unsigned long long free_pages;
    unsigned long long used_pages;
    unsigned long long total_cpu_ticks;
    unsigned long long cpu_ticks[ICSOS_MAX_CPUS];
};

int icsos_proc_list(struct icsos_procinfo *buf, int max);
int icsos_sysinfo(struct icsos_sysinfo *info);
int icsos_kill(int pid, int sig);

#endif
