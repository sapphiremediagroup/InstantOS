// /bin/login of the NetSurf suite ISO (tools/run-netsurf-suite.sh): brings up
// the desktop the way login does, after giving NetSurf a homepage on the
// host, then idles. The host (drive.py) does the rest over QMP: clicks the
// taskbar's Web slot, watches the page load, clicks a link, types a URL.
#include "../os-testsuite/ostest.h"

#ifndef HTTPS_PORT
#define HTTPS_PORT 18443
#endif
#define STR_(x) #x
#define STR(x) STR_(x)

enum { N_NETGETCONFIG = 141 };

struct NetConfig {
    u32 flags, address, netmask, gateway, dns[3], dnsCount, dhcpServer;
    u32 leaseSeconds, leaseRemaining, ntpServer;
    u64 generation;
    char domain[64];
    u32 linkUp;
    char device[16];
};

// /bin/netsurf runs NetSurf with HOME=/tmp, which reads ~/.netsurf/Choices.
static void write_choices(void){
    SC2(N_MKDIR,"/tmp/.netsurf",0755);
    const char* choices = "homepage_url:https://10.0.2.2:" STR(HTTPS_PORT) "/ns/page.html\n";
    i64 h=t_open("/tmp/.netsurf/Choices",O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(h<0){ sw("[nstest] FAIL cannot write Choices\n"); return; }
    t_write(h,choices,slen(choices));
    t_close(h);
}

// The kernel enters _start with rsp 16-aligned, not 8 past it like a call,
// so realign: clang stores the arrays below with aligned SSE moves.
__attribute__((force_align_arg_pointer)) void _start(void){
    sw("\n[nstest] BEGIN\n");
    write_choices();

    // The browser's first fetch needs an address; DHCP takes a moment.
    struct NetConfig cfg; u64 gen=0;
    for(int i=0;i<60;i++){
        memset(&cfg,0,sizeof cfg);
        if(SC3(N_NETGETCONFIG,&cfg,gen,1000)==0){
            gen=cfg.generation;
            if(cfg.flags&1) break;
        }
    }
    sw((cfg.flags&1) ? "[nstest] network up\n" : "[nstest] FAIL no address\n");

    const char* av[]={"/bin/graphics-compositor",0};
    const char* ev[]={"HOME=/","PATH=/bin",0};
    i64 pid=SC3(N_SPAWN,"/bin/graphics-compositor",av,ev);
    if(pid<=0){ sw("[nstest] FAIL cannot start the compositor\n"); }
    else sw("[nstest] compositor started\n");
    for(;;) msleep(1000);
}
