// /bin/login of tools/run-thread-suite.sh: runs the thread test programs one
// after the other, copies what they print to the serial port, and counts their
// exit statuses. Freestanding, like the feature suite's runner.
#include "../os-testsuite/ostest.h"

static i64 run(const char* path){   // exit status, 1000 + signal is 128+signal anyway; -1: too long
    u64 fds[2];
    if(SC1(N_PIPE,fds)!=0) return -2;
    i64 pid=SC0(N_FORK);
    if(pid==0){
        SC2(N_DUP2,fds[1],1); SC2(N_DUP2,fds[1],2);
        SC1(N_CLOSE,fds[0]);
        const char* av[]={path,0}; const char* ev[]={"PATH=/bin","HOME=/tmp",0};
        SC3(N_EXEC,path,av,ev);
        SC1(N_EXIT,99); for(;;){}
    }
    SC1(N_CLOSE,fds[1]);
    static char buf[512];
    for(;;){
        i64 n=t_read(fds[0],buf,sizeof buf);
        if(n<=0) break;
        swn(buf,(u64)n);
    }
    SC1(N_CLOSE,fds[0]);
    int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
    if(w!=pid) return -1;
    return (st&0x7f) ? 1000+(st&0x7f) : (st>>8)&0xff;   // died of a signal: 1000 + it
}

__attribute__((force_align_arg_pointer))
void _start(void){
    SC2(N_SIGNAL,SIG_PIPE,1);
    sw("\n[threads] BEGIN\n");
    static const char* programs[]={ PROGRAMS };
    for(u64 i=0;i<sizeof programs/sizeof programs[0];i++){
        section(programs[i]);
        i64 r=run(programs[i]);
        checkv(r==0,"exit status 0",r);
    }
    section("");
    sw("\n[threads] SCORE pass="); sdec(g_pass); sw(" total="); sdec(g_total);
    sw(" fail="); sdec(g_total-g_pass); sw(" \n[threads] END\n");
    for(;;){ SC1(N_SLEEP,1000); }
}
