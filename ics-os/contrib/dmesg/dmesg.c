#include "../../sdk/dexsdk.h"

// Syscall numbers (must match klog.c)
#define FXN_KLOG_READ 0xAC
#define FXN_KLOG_SET  0xAD
#define FXN_KLOG_GET  0xAE

static void usage(){
    printf("Usage: dmesg [watch]\n");
}

int main(int argc, char **argv){
    int watch = (argc>1 && strcmp(argv[1],"watch")==0);
    char buf[160];
    int r, n=0;
    // Drain once
    while( (r=dexsdk_systemcall(FXN_KLOG_READ,(int)buf,sizeof(buf),0,0,0))>0 ){
        printf("%s", buf);
        n++;
        if(!watch) {
            if(n>256) break; // safety
        }
    }
    if(!watch) return 0;
    printf("-- dmesg watch (Ctrl-C to exit) --\n");
    while(1){
        r=dexsdk_systemcall(FXN_KLOG_READ,(int)buf,sizeof(buf),0,0,0);
        if(r>0){ printf("%s", buf); }
        else dexsdk_systemcall(FXN_SLEEP,10,0,0,0,0);
    }
    return 0;
}
