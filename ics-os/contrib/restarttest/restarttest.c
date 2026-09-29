#include "../../sdk/dexsdk.h"

/*
 * restarttest: targeted test for the "process restart from entry point" bug.
 *
 * Bug: A user process is switched in, executes some code, then the current
 * process is reset to idle WITHOUT going through ps_switchto (with the
 * process as prev). When the process is switched in again, its ctx still
 * has the entry point, so it restarts from the beginning.
 *
 * Detection: If the process is restarted from the entry point, the startup
 * marker is printed multiple times. If the process is resumed correctly,
 * the startup marker is printed exactly once.
 *
 * This version launches a child process (like the shell does for NetHack)
 * and waits for it to exit. The child does the same sequence of syscalls
 * as NetHack: getpid, getenv, chdir, open.
 */

#define STARTUP_MARKER "RESTART_TEST_CHILD_STARTUP"
#define COUNTER_INTERVAL 100
#define TOTAL_ITERATIONS 5000

static void child_main() {
    long counter = 0;
    
    printf(STARTUP_MARKER "\n");
    
    while (counter < TOTAL_ITERATIONS) {
        counter++;
        
        /* Do the same sequence of syscalls as NetHack */
        getpid();                    /* getpid */
        getenv("NETHACKDIR");        /* getenv */
        getenv("HACKDIR");           /* getenv */
        chdir("/icsos/nethack");     /* chdir */
        open(NULL, 0);               /* open(NULL) */
        
        if (counter % COUNTER_INTERVAL == 0) {
            printf("RESTART_TEST_COUNTER %ld\n", counter);
        }
    }
    
    printf("RESTART_TEST_CHILD_PASS %ld\n", counter);
    exit(0);
}

int main() {
    pid_t pid = fork();
    
    if (pid < 0) {
        printf("RESTART_TEST_FAIL fork failed\n");
        return 1;
    }
    
    if (pid == 0) {
        /* Child process */
        child_main();
        /* Should not reach here */
        exit(0);
    }
    
    /* Parent process: wait for child to exit */
    int status;
    waitpid(pid, &status, 0);
    
    printf("RESTART_TEST_PASS\n");
    return 0;
}
