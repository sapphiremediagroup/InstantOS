// InstantOS network suite (freestanding, raw syscalls). Runs as /bin/login on a
// headless boot with virtio-net on QEMU user networking (slirp) and talks to
// tools/net-suite/host_server.py, which slirp exposes at 10.0.2.2:
//   NIC presence, ping to the gateway, UDP echo, TCP echo, HTTP downloads.
// Built with -DNET_INTERNET=1 it also reaches the real internet: raw DNS via
// slirp's forwarder (10.0.2.3) and HTTP to example.com, plus the mlibc
// dns-smoke/http-smoke apps when they are in the initrd.
//
// "GAP:" lines record known shortcomings that a later step of the internet plan
// fixes; they are listed, not scored. Prints "[netsuite] SCORE ..." and
// "[netsuite] END". Every wait is bounded (non-blocking sockets with deadlines,
// or SO_RCVTIMEO/SO_SNDTIMEO on blocking ones), so a broken stack fails checks
// instead of hanging the run.

#include "../os-testsuite/ostest.h"

#ifndef TCP_ECHO_PORT
#define TCP_ECHO_PORT 18007
#endif
#ifndef UDP_ECHO_PORT
#define UDP_ECHO_PORT 18008
#endif
#ifndef HTTP_PORT
#define HTTP_PORT 18080
#endif
#ifndef CLOSED_PORT
#define CLOSED_PORT 18009
#endif
#ifndef NET_INTERNET
#define NET_INTERNET 0
#endif
#ifndef NET_LINK
#define NET_LINK 0
#endif
#ifndef HAVE_QMP
#define HAVE_QMP 0
#endif
#ifndef NET_TOOLS
#define NET_TOOLS 0
#endif
#ifndef HAVE_CURL
#define HAVE_CURL 0
#endif
#ifndef SINK_PORT
#define SINK_PORT 18006
#endif
#ifndef HTTPS_PORT
#define HTTPS_PORT 18443
#endif
#ifndef HAVE_TLS
#define HAVE_TLS 0
#endif
#ifndef HAVE_CA_BUNDLE
#define HAVE_CA_BUNDLE 0
#endif

enum { N_NETPING=71, N_NETGETPINGREPLY=73 };
enum { E_CONNREFUSED=111, E_INPROGRESS=115, E_ALREADY=114, E_NETUNREACH=101, E_HOSTUNREACH=113 };
enum { N_NETGETCONFIG=141, N_NETSETCONFIG=142 };
enum { IPPROTO_TCP_=6, TCP_NODELAY_=1, TCP_MAXSEG_=2, TCP_INFO_=11 };
// struct tcp_info: 8 bytes of u8 fields, then u32s from tcpi_rto on.
struct TcpInfo { u8 state,ca_state,retransmits,probes,backoff,options,wscale,flags;
                 u32 rto,ato,snd_mss,rcv_mss,unacked,sacked,lost,retrans,fackets,
                     last_data_sent,last_ack_sent,last_data_recv,last_ack_recv,
                     pmtu,rcv_ssthresh,rtt,rttvar,snd_ssthresh,snd_cwnd,advmss,
                     reordering,rcv_rtt,rcv_space,total_retrans; };
enum { CFG_UP=1, CFG_DHCP=2 };
enum { CFG_STATIC=0, CFG_DHCP_START=1, CFG_DHCP_RENEW=2, CFG_DOWN=3 };
struct NetConfig { u32 flags,address,netmask,gateway,dns[3],dnsCount,dhcpServer,leaseSeconds,leaseRemaining,ntpServer; u64 generation; char domain[64]; u32 linkUp; char device[16]; };
enum { F_GETFL_=3, F_SETFL_=4, SO_RCVTIMEO_=20, SO_SNDTIMEO_=21, MSG_DONTWAIT_=0x40 };
struct Timeval { i64 sec, usec; };
enum { POLLHUP_=0x10 };

#define STR_(x) #x
#define STR(x) STR_(x)
#define IP4(a,b,c,d) ((u32)(a)|((u32)(b)<<8)|((u32)(c)<<16)|((u32)(d)<<24))   // network order
static const u32 kHost = IP4(10,0,2,2);
static const u32 kGuest = IP4(10,0,2,15);
static const u32 kSlirpDns = IP4(10,0,2,3);
#if NET_LINK
static const u32 kFake = IP4(10,0,2,77);   // host_server.py --link
#endif

static int g_gaps=0;
static void gap(const char* what,i64 got){ g_gaps++; sw("  GAP: "); sw(what); sw(" got="); sdec(got); sw("\n"); }

static i64 now_ms(void){ return SC0(N_GETTIME); }
static u8 pat(u64 i){ return (u8)((i*7+(i>>8))&0xff); }

static struct sockaddr_in addr4(u32 ip,u16 port){
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr=ip; return a;
}

// Non-blocking TCP connect; 0 when established, else -errno (or -E_AGAIN on timeout).
static i64 tcp_connect(u32 ip,u16 port,i64 timeout_ms,i64* fd_out){
    i64 fd=SC3(N_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
    *fd_out=fd;
    if(fd<0) return fd;
    struct sockaddr_in a=addr4(ip,port);
    i64 deadline=now_ms()+timeout_ms, r;
    for(;;){
        r=SC3(N_CONNECT,fd,&a,sizeof a);
        if(r==0) return 0;
        if(r!=-E_AGAIN && r!=-E_INPROGRESS && r!=-E_ALREADY) return r;
        if(now_ms()>=deadline) return -E_AGAIN;
        msleep(1);
    }
}

// Sends all of buf; returns bytes sent (short on error/timeout).
static i64 send_all(i64 fd,const void* buf,u64 n,i64 timeout_ms){
    u64 off=0; i64 deadline=now_ms()+timeout_ms;
    while(off<n){
        i64 s=SC4(N_SEND,fd,(const u8*)buf+off,n-off,0);
        if(s>0){ off+=(u64)s; continue; }
        if(s!=-E_AGAIN || now_ms()>=deadline) break;
        msleep(1);
    }
    return (i64)off;
}

// One recv with a timeout: >0 bytes, 0 EOF, -errno, or -E_AGAIN on timeout.
static i64 recv_wait(i64 fd,void* buf,u64 n,i64 timeout_ms){
    i64 deadline=now_ms()+timeout_ms;
    for(;;){
        i64 r=SC4(N_RECV,fd,buf,n,0);
        if(r!=-E_AGAIN || now_ms()>=deadline) return r;
        msleep(1);
    }
}

// Bound every blocking call on fd: recv/accept by rcv_ms, send/connect by snd_ms.
static void set_timeouts(i64 fd,i64 rcv_ms,i64 snd_ms){
    struct Timeval r={rcv_ms/1000,(rcv_ms%1000)*1000}, s={snd_ms/1000,(snd_ms%1000)*1000};
    SC5(N_SETSOCKOPT,fd,SOL_SOCKET,SO_RCVTIMEO_,&r,sizeof r);
    SC5(N_SETSOCKOPT,fd,SOL_SOCKET,SO_SNDTIMEO_,&s,sizeof s);
}

// Blocking TCP connect (bounded by SO_SNDTIMEO); 0 or -errno, *fd_out always set.
static i64 tcp_connect_blocking(u32 ip,u16 port,i64 timeout_ms,i64* fd_out){
    i64 fd=SC3(N_SOCKET,AF_INET,SOCK_STREAM,0);
    *fd_out=fd;
    if(fd<0) return fd;
    set_timeouts(fd,timeout_ms,timeout_ms);
    struct sockaddr_in a=addr4(ip,port);
    return SC3(N_CONNECT,fd,&a,sizeof a);
}

// Device interrupts served so far, per CPU (CpuInfo). Fills up to 16 entries.
struct CpuInfoEntry { u32 index, lapicId; u64 deviceIrqs; u32 currentPid, reserved; };
static i64 device_irqs(u64* per_cpu){
    i64 n=SC2(N_CPUINFO,0,0); if(n>16) n=16;
    for(i64 i=0;i<n;i++){ struct CpuInfoEntry e; memset(&e,0,sizeof e); SC2(N_CPUINFO,i,&e); per_cpu[i]=e.deviceIrqs; }
    return n;
}

static void print_rate(const char* what,u64 bytes,i64 ms){
    sw("  ("); sw(what); sw(": "); sdec((i64)bytes); sw(" bytes in "); sdec(ms); sw(" ms");
    if(ms>0){ sw(", "); sdec((i64)(bytes/(u64)ms)); sw(" KB/s"); }
    sw(")\n");
}

static int tcp_echo_stream(i64 fd,u64 total,i64 timeout_ms,u64* got_out);

static i64 get_config(struct NetConfig* c,u64 known,i64 timeout_ms){
    memset(c,0,sizeof *c); return SC3(N_NETGETCONFIG,c,known,timeout_ms);
}

// Waits up to timeout_ms for the interface flags to become `want`.
static int wait_flags(struct NetConfig* c,u32 want,i64 timeout_ms){
    i64 deadline=now_ms()+timeout_ms;
    get_config(c,0,0);
    while(c->flags!=want){
        i64 left=deadline-now_ms(); if(left<=0) return 0;
        get_config(c,c->generation,left);
    }
    return 1;
}

// Waits up to timeout_ms for `path` to contain `needle` (network-manager
// rewrites it after each change).
static int wait_file_contains(const char* path,const char* needle,i64 timeout_ms){
    static char buf[512]; i64 deadline=now_ms()+timeout_ms;
    for(;;){
        i64 h=t_open(path,O_RDONLY,0), n=0;
        if(h>=0){ n=t_read(h,buf,sizeof buf-1); t_close(h); }
        if(n>0){ buf[n]=0; if(contains(buf,(u64)n,needle)) return 1; }
        if(now_ms()>=deadline) return 0;
        msleep(20);
    }
}

// A short echo over a fresh connection to the host: 1 when it all came back.
static int quick_echo(void){
    i64 fd; int good=0;
    if(tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd)==0){ u64 g=0; good=tcp_echo_stream(fd,1000,5000,&g); }
    if(fd>0) t_close(fd);
    return good;
}

// ============================ SECTIONS ============================

static int g_have_nic=0;
static void test_nic(void){
    section("NIC");
    u8 mac[6]; memset(mac,0,6);
    i64 r=SC1(N_NETGETMAC,mac); ok(r,"net_get_mac (a NIC is present)");
    int nz=0; for(int i=0;i<6;i++) if(mac[i]) nz=1;
    check(r==0&&nz,"MAC is not all-zero");
    // A real PHY (and the rtl8168 model) needs a while to autonegotiate.
    i64 t0=now_ms(), link=SC0(N_NETLINK);
    while(link!=1&&now_ms()-t0<10000){ msleep(50); link=SC0(N_NETLINK); }
    checkv(link==1,"link is up (ms)",now_ms()-t0);
    struct NetConfig c; get_config(&c,0,0);
    c.device[sizeof c.device-1]=0;
    check(c.device[0]!=0,"NetGetConfig names the NIC driver");
    sw("  (driver: "); sw(c.device); sw(")\n");
    checkv(c.linkUp==1,"NetGetConfig reports the link up",c.linkUp);
    g_have_nic = r==0;
}

// The interface comes up by DHCP from slirp, and network-manager writes the
// resolver configuration from the lease.
static void test_config(void){
    section("interface config (DHCP)");
    struct NetConfig c; i64 t0=now_ms();
    int up=wait_flags(&c,CFG_UP|CFG_DHCP,10000);
    checkv(up,"interface up from a DHCP lease (flags)",c.flags);
    sw("  (lease after "); sdec(now_ms()-t0); sw(" ms of the suite)\n");
    checkv(c.address==kGuest,"leased address is 10.0.2.15",c.address);
    checkv(c.netmask==IP4(255,255,255,0),"netmask /24",c.netmask);
    checkv(c.gateway==kHost,"gateway 10.0.2.2",c.gateway);
    checkv(c.dnsCount>=1&&c.dns[0]==kSlirpDns,"name server 10.0.2.3",c.dnsCount);
    checkv(c.dhcpServer==kHost,"DHCP server 10.0.2.2",c.dhcpServer);
    checkv(c.leaseSeconds>0&&c.leaseRemaining<=c.leaseSeconds&&c.leaseRemaining+60>=c.leaseSeconds,"lease granted and running (s)",c.leaseSeconds);
    check(wait_file_contains("/etc/resolv.conf","nameserver 10.0.2.3",5000),"network-manager writes resolv.conf from the lease");
    check(wait_file_contains("/etc/hosts","10.0.2.15\tinstantos",5000),"network-manager maps the host name to the leased address");

    // A wait on an unchanged configuration ends at its timeout.
    struct NetConfig d; t0=now_ms();
    i64 r=get_config(&d,c.generation,300); i64 took=now_ms()-t0;
    checkv(r==0&&d.generation==c.generation&&took>=250&&took<3000,"NetGetConfig wait times out when nothing changes (ms)",took);

    // Renewal: a unicast REQUEST to the server, answered with a fresh lease.
    r=SC2(N_NETSETCONFIG,0,CFG_DHCP_RENEW); ok(r,"NetSetConfig renew");
    t0=now_ms(); r=get_config(&d,c.generation,5000); took=now_ms()-t0;
    checkv(r==0&&d.generation!=c.generation,"the renewal ACK arrives (ms)",took);
    check(d.flags==(CFG_UP|CFG_DHCP)&&d.address==kGuest,"renewed lease keeps 10.0.2.15");

    // A connection bound to any address reports the interface address.
    i64 fd;
    if(tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd)==0){
        struct sockaddr_in sn; memset(&sn,0,sizeof sn); u32 sl=sizeof sn;
        i64 g=SC3(N_GETSOCKNAME,fd,&sn,&sl);
        checkv(g==0&&sn.sin_addr==kGuest,"getsockname of a TCP connection is 10.0.2.15",(i64)sn.sin_addr);
    } else check(0,"getsockname of a TCP connection is 10.0.2.15 (connect failed)");
    if(fd>0) t_close(fd);
}

static void test_ping(void){
    section("ping");
    struct { u32 srcIp; u16 id,seq,payloadSize,reserved; } reply;
    // host-order address for NetPing
    const u32 gw=(10u<<24)|(0u<<16)|(2u<<8)|2u;
    int got=0; i64 t0=now_ms(), sent=0;
    for(int attempt=0;attempt<20&&!got;attempt++){
        i64 r=SC3(N_NETPING,gw,0x1234,attempt);
        if(r==0||r==-E_AGAIN) sent++;
        for(int i=0;i<100&&!got;i++){
            memset(&reply,0,sizeof reply);
            if(SC1(N_NETGETPINGREPLY,&reply)==0 && reply.id==0x1234) got=1; else msleep(1);
        }
    }
    checkv(sent>0,"ping 10.0.2.2 sent",sent);
    check(got,"echo reply from 10.0.2.2");
    check(got&&reply.srcIp==gw,"reply comes from the gateway");
    if(got){ sw("  (ping rtt+arp: "); sdec(now_ms()-t0); sw(" ms)\n"); }
}

static void test_udp(void){
    section("UDP echo (host)");
    i64 fd=SC3(N_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0); checkv(fd>0,"udp socket",fd);
    if(fd<=0) return;
    struct sockaddr_in a=addr4(kHost,UDP_ECHO_PORT);
    ok(SC3(N_CONNECT,fd,&a,sizeof a),"udp connect to host echo");
    static u8 out[1472], in[2048];
    static const u64 sizes[]={1,16,100,512,1000,1400,1472};
    int good=0; i64 badsz=-1;
    for(u64 k=0;k<sizeof sizes/sizeof sizes[0];k++){
        u64 n=sizes[k];
        for(u64 i=0;i<n;i++) out[i]=pat(i+k);
        i64 s=SC4(N_SEND,fd,out,n,0);
        i64 r=recv_wait(fd,in,sizeof in,2000);
        if(s==(i64)n && r==(i64)n && smemcmp(in,out,n)==0) good++; else if(badsz<0) badsz=(i64)n;
    }
    checkv(good==(int)(sizeof sizes/sizeof sizes[0]),"echo of 1..1472-byte datagrams (first bad size)",badsz);
    // 50 small datagrams back to back: count how many echoes come back.
    int sent=0,back=0;
    for(int i=0;i<50;i++){ u8 b[8]={(u8)i,1,2,3,4,5,6,7}; if(SC4(N_SEND,fd,b,8,0)==8) sent++; }
    i64 deadline=now_ms()+3000;
    while(back<sent && now_ms()<deadline){ i64 r=SC4(N_RECV,fd,in,sizeof in,0); if(r==8) back++; else msleep(1); }
    checkv(sent==50,"50 back-to-back datagrams sent",sent);
    if(back==sent) check(1,"all 50 echoes received");
    else gap("burst of 50 UDP echoes: some lost",back);
    t_close(fd);
}

// Streams `total` pattern bytes to the echo server while reading them back.
static int tcp_echo_stream(i64 fd,u64 total,i64 timeout_ms,u64* got_out){
    static u8 out[4096], in[4096];
    u64 sent=0,got=0; int bad=0; i64 deadline=now_ms()+timeout_ms;
    while(got<total && now_ms()<deadline){
        int progress=0;
        if(sent<total){
            u64 n=total-sent; if(n>sizeof out) n=sizeof out;
            for(u64 i=0;i<n;i++) out[i]=pat(sent+i);
            i64 s=SC4(N_SEND,fd,out,n,0);
            if(s>0){ sent+=(u64)s; progress=1; }
            else if(s!=-E_AGAIN) break;
        }
        i64 r=SC4(N_RECV,fd,in,sizeof in,0);
        if(r>0){ for(i64 i=0;i<r;i++) if(in[i]!=pat(got+(u64)i)) bad=1; got+=(u64)r; progress=1; }
        else if(r==0||(r<0&&r!=-E_AGAIN)) break;
        if(!progress) msleep(1);
    }
    *got_out=got;
    return got==total && !bad;
}

static void test_tcp(void){
    section("TCP echo (host)");
    i64 fd; i64 t0=now_ms();
    i64 r=tcp_connect(kHost,TCP_ECHO_PORT,5000,&fd);
    checkv(r==0,"connect to host echo",r);
    if(r==0){ sw("  (handshake: "); sdec(now_ms()-t0); sw(" ms)\n"); }
    if(r==0){
        struct sockaddr_in pn; memset(&pn,0,sizeof pn); u32 pl=sizeof pn;
        i64 g=SC3(N_GETPEERNAME,fd,&pn,&pl);
        check(g==0&&pn.sin_addr==kHost&&pn.sin_port==htons(TCP_ECHO_PORT),"getpeername is the host echo");
        u64 got=0; i64 t1=now_ms();
        int okk=tcp_echo_stream(fd,256*1024,30000,&got);
        checkv(okk,"256 KiB echoed back intact",(i64)got);
        print_rate("echo round trip",got,now_ms()-t1);
    }
    if(fd>0) t_close(fd);

    // A process parked in poll() must be woken by the SYN-ACK (and by data),
    // not by its timeout: that is how mlibc's blocking connect() waits.
    {
        i64 f=SC3(N_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
        struct sockaddr_in a=addr4(kHost,TCP_ECHO_PORT);
        i64 c=SC3(N_CONNECT,f,&a,sizeof a);
        struct PollFD p={f,POLLOUT,0}; i64 t1=now_ms();
        i64 pr=(c==-E_INPROGRESS)?SC3(N_POLL,&p,1,5000):1;
        checkv(c==-E_INPROGRESS,"non-blocking connect -> EINPROGRESS",c);
        i64 waited=now_ms()-t1;
        checkv(pr==1&&(p.revents&POLLOUT)&&waited<2000,"poll(POLLOUT) wakes when the handshake completes (ms)",waited);
        send_all(f,"wake",4,2000);
        struct PollFD q={f,POLLIN,0}; t1=now_ms();
        pr=SC3(N_POLL,&q,1,5000); waited=now_ms()-t1;
        checkv(pr==1&&(q.revents&POLLIN)&&waited<2000,"poll(POLLIN) wakes when the echo arrives (ms)",waited);
        t_close(f);
    }

    // Sequential reconnects: fresh ports, clean teardown.
    int seq_ok=0;
    for(int i=0;i<5;i++){
        i64 f; if(tcp_connect(kHost,TCP_ECHO_PORT,5000,&f)==0){ u64 g=0; if(tcp_echo_stream(f,1000,5000,&g)) seq_ok++; }
        if(f>0) t_close(f);
    }
    checkv(seq_ok==5,"5 sequential connect/echo/close",seq_ok);

    // Two connections in flight at once.
    i64 f1,f2;
    i64 r1=tcp_connect(kHost,TCP_ECHO_PORT,5000,&f1), r2=tcp_connect(kHost,TCP_ECHO_PORT,5000,&f2);
    check(r1==0&&r2==0,"two concurrent connections");
    if(r1==0&&r2==0){
        static u8 b1[2000],b2[2000],in[4096];
        for(int i=0;i<2000;i++){ b1[i]=(u8)i; b2[i]=(u8)(255-i); }
        send_all(f1,b1,2000,3000); send_all(f2,b2,2000,3000);
        u64 g1=0,g2=0; int bad=0; i64 deadline=now_ms()+5000;
        while((g1<2000||g2<2000)&&now_ms()<deadline){
            i64 a=SC4(N_RECV,f1,in,sizeof in,0); if(a>0){ for(i64 i=0;i<a;i++) if(in[i]!=b1[g1+(u64)i]) bad=1; g1+=(u64)a; }
            i64 b=SC4(N_RECV,f2,in,sizeof in,0); if(b>0){ for(i64 i=0;i<b;i++) if(in[i]!=b2[g2+(u64)i]) bad=1; g2+=(u64)b; }
            if(a<=0&&b<=0) msleep(1);
        }
        check(g1==2000&&g2==2000&&!bad,"each connection gets its own bytes back");
    }
    if(f1>0) t_close(f1);
    if(f2>0) t_close(f2);

    // Nothing listens on CLOSED_PORT: slirp answers the SYN with a RST.
    i64 fc; i64 rc=tcp_connect(kHost,CLOSED_PORT,3000,&fc);
    checkv(rc==-E_CONNREFUSED,"connect to a closed port -> ECONNREFUSED",rc);
    if(fc>0) t_close(fc);
}

// Sockets without O_NONBLOCK wait in the kernel: connect until the handshake,
// recv until data, send until the window has room.
static void test_blocking(void){
    section("blocking sockets");
    i64 fd; i64 t0=now_ms();
    i64 r=tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd);
    i64 took=now_ms()-t0;
    checkv(r==0,"blocking connect returns 0 once established",r);
    checkv(r==0&&took<2000,"blocking connect takes one handshake (ms)",took);
    if(r==0){
        i64 fl=SC3(N_FCNTL,fd,F_GETFL_,0);
        checkv(fl>=0&&!(fl&O_NONBLOCK),"F_GETFL: no O_NONBLOCK",fl);
        // MSG_DONTWAIT makes one call non-blocking.
        u8 b[64]; t0=now_ms();
        i64 d=SC4(N_RECV,fd,b,sizeof b,MSG_DONTWAIT_);
        checkv(d==-E_AGAIN&&now_ms()-t0<500,"recv(MSG_DONTWAIT) with nothing queued -> EAGAIN",d);
        // SO_RCVTIMEO bounds a blocking recv.
        set_timeouts(fd,300,5000); t0=now_ms();
        d=SC4(N_RECV,fd,b,sizeof b,0); took=now_ms()-t0;
        checkv(d==-E_AGAIN&&took>=250&&took<3000,"idle blocking recv times out after SO_RCVTIMEO=300 ms (ms)",took);
        // 64 KiB in one blocking send (more than the peer's window), then
        // blocking recvs until all of it is back: no EAGAIN along the way.
        set_timeouts(fd,10000,10000);
        static u8 out[65536], in[8192];
        for(u64 i=0;i<sizeof out;i++) out[i]=pat(i);
        t0=now_ms();
        i64 s=SC4(N_SEND,fd,out,sizeof out,0);
        checkv(s==(i64)sizeof out,"one blocking send queues all 64 KiB",s);
        u64 got=0; int bad=0; i64 last=0;
        while(got<sizeof out){
            last=SC4(N_RECV,fd,in,sizeof in,0);
            if(last<=0) break;
            for(i64 i=0;i<last;i++) if(in[i]!=pat(got+(u64)i)) bad=1;
            got+=(u64)last;
        }
        checkv(got==sizeof out&&!bad,"blocking recvs return the 64 KiB echo intact (last rc)",last);
        print_rate("blocking echo round trip",got,now_ms()-t0);
        // fcntl(F_SETFL, O_NONBLOCK) switches the same socket to EAGAIN.
        SC3(N_FCNTL,fd,F_SETFL_,O_NONBLOCK);
        fl=SC3(N_FCNTL,fd,F_GETFL_,0);
        checkv(fl>=0&&(fl&O_NONBLOCK),"F_SETFL O_NONBLOCK is reported back",fl);
        t0=now_ms(); d=SC4(N_RECV,fd,b,sizeof b,0);
        checkv(d==-E_AGAIN&&now_ms()-t0<500,"recv after F_SETFL O_NONBLOCK -> EAGAIN",d);
    }
    if(fd>0) t_close(fd);

    i64 fc; r=tcp_connect_blocking(kHost,CLOSED_PORT,3000,&fc);
    checkv(r==-E_CONNREFUSED,"blocking connect to a closed port -> ECONNREFUSED",r);
    if(fc>0) t_close(fc);

    // Blocking UDP: the echo is waited for, not polled.
    i64 u=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0);
    if(u>0){
        set_timeouts(u,2000,2000);
        struct sockaddr_in a=addr4(kHost,UDP_ECHO_PORT);
        SC3(N_CONNECT,u,&a,sizeof a);
        u8 q[32], back[64]; for(int i=0;i<32;i++) q[i]=(u8)(i*3);
        SC4(N_SEND,u,q,32,0);
        i64 n=SC4(N_RECV,u,back,sizeof back,0);
        checkv(n==32&&smemcmp(q,back,32)==0,"blocking UDP recv returns the echo",n);
        t_close(u);
    }
}

#if NET_LINK
// The fake host 10.0.2.77 answers with ICMP errors, and probes us in turn.
static void test_icmp(void){
    section("ICMP (fake host 10.0.2.77)");
    // UDP to a closed port: the port unreachable fails the next call.
    i64 u=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0);
    if(u>0){
        set_timeouts(u,2000,2000);
        struct sockaddr_in a=addr4(kFake,5000);
        SC3(N_CONNECT,u,&a,sizeof a);
        u8 b[16]; i64 t0=now_ms();
        SC4(N_SEND,u,"x",1,0);
        i64 r=SC4(N_RECV,u,b,sizeof b,0);
        checkv(r==-E_CONNREFUSED&&now_ms()-t0<1500,"connected UDP: ICMP port unreachable -> ECONNREFUSED",r);
        r=SC4(N_RECV,u,b,sizeof b,MSG_DONTWAIT_);
        checkv(r==-E_AGAIN,"the error is reported once",r);
        t_close(u);
    }
    // TCP: a host unreachable ends the handshake now, not after the retries.
    i64 fd; i64 t0=now_ms();
    i64 r=tcp_connect_blocking(kFake,5001,10000,&fd); i64 took=now_ms()-t0;
    checkv(r==-E_HOSTUNREACH,"connect answered by ICMP host unreachable -> EHOSTUNREACH",r);
    checkv(took<2000,"... at once (ms)",took);
    if(fd>0) t_close(fd);

    // The fake host pings us, sends UDP and a SYN to closed port 9.
    i64 p=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0), q=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0);
    static char status[64]; i64 n=0;
    if(p>0&&q>0){
        struct sockaddr_in pa=addr4(kFake,7777), qa=addr4(kFake,7778);
        SC3(N_CONNECT,p,&pa,sizeof pa); SC3(N_CONNECT,q,&qa,sizeof qa);
        set_timeouts(q,500,500);
        SC4(N_SEND,p,"probe",5,0);
        i64 deadline=now_ms()+3000;
        while(now_ms()<deadline){
            msleep(50);
            SC4(N_SEND,q,"status",6,0);
            n=SC4(N_RECV,q,status,sizeof status-1,0);
            if(n>0){ status[n]=0; if(contains(status,(u64)n,"echo=1 unreach=1 rst=1")) break; }
        }
    }
    if(p>0) t_close(p);
    if(q>0) t_close(q);
    if(n<0) n=0;
    status[n]=0;
    sw("  (fake host saw: "); sw(n>0?status:"nothing"); sw(")\n");
    check(contains(status,(u64)n,"echo=1"),"we answer a ping from another host");
    check(contains(status,(u64)n,"unreach=1"),"UDP to a closed port -> ICMP port unreachable");
    check(contains(status,(u64)n,"rst=1"),"TCP SYN to a closed port -> RST");
}
#endif

// Taking the interface down and configuring it by hand, then DHCP again.
static void test_down_up(void){
    section("interface down / static / DHCP");
    struct NetConfig c;
    i64 r=SC2(N_NETSETCONFIG,0,CFG_DOWN); ok(r,"NetSetConfig down");
    get_config(&c,0,0);
    checkv(c.flags==0&&c.address==0,"no address while down (flags)",c.flags);
    i64 fd; r=tcp_connect_blocking(kHost,TCP_ECHO_PORT,2000,&fd);
    checkv(r==-E_NETUNREACH,"TCP connect while down -> ENETUNREACH",r);
    if(fd>0) t_close(fd);
    i64 u=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in a=addr4(kHost,UDP_ECHO_PORT);
    r=u>0?SC3(N_CONNECT,u,&a,sizeof a):u;
    checkv(r==-E_NETUNREACH,"UDP connect while down -> ENETUNREACH",r);
    if(u>0) t_close(u);
    r=SC3(N_NETPING,(10u<<24)|(2u<<8)|2u,1,1);
    checkv(r==-E_NETUNREACH,"ping while down -> ENETUNREACH",r);
    check(wait_file_contains("/etc/hosts","127.0.1.1\tinstantos",5000),"hosts falls back to loopback while down");

    struct NetConfig st; memset(&st,0,sizeof st);
    st.address=kGuest; st.netmask=IP4(255,255,255,0); st.gateway=kHost;
    st.dns[0]=kSlirpDns; st.dns[1]=IP4(1,1,1,1); st.dnsCount=2;
    r=SC2(N_NETSETCONFIG,&st,CFG_STATIC); ok(r,"NetSetConfig static 10.0.2.15/24");
    get_config(&c,0,0);
    checkv(c.flags==CFG_UP&&c.address==kGuest&&c.dnsCount==2,"static configuration in effect (flags)",c.flags);
    check(quick_echo(),"TCP echo works on the static address");
    check(wait_file_contains("/etc/resolv.conf","nameserver 1.1.1.1",5000),"resolv.conf follows the static name servers");

    r=SC2(N_NETSETCONFIG,0,CFG_DHCP_START); ok(r,"NetSetConfig DHCP");
    i64 t0=now_ms();
    int up=wait_flags(&c,CFG_UP|CFG_DHCP,15000);
    checkv(up&&c.address==kGuest,"a new DHCP lease (ms)",now_ms()-t0);
    check(quick_echo(),"TCP echo works again on the lease");
    int back=0; i64 deadline=now_ms()+5000;
    while(!back&&now_ms()<deadline){
        back=wait_file_contains("/etc/resolv.conf","nameserver 10.0.2.3\n",0)&&!wait_file_contains("/etc/resolv.conf","1.1.1.1",0);
        if(!back) msleep(20);
    }
    check(back,"resolv.conf is back to the leased name server");
}

static i64 http_get(u32 ip,u16 port,const char* host,const char* path,int verify_pattern,
                    char* head,u64 headcap,i64 timeout_ms,int* bad_out);

static i64 tcp_info(i64 fd,struct TcpInfo* t){
    memset(t,0,sizeof *t); u32 len=sizeof *t;
    return SC5(N_GETSOCKOPT,fd,IPPROTO_TCP_,TCP_INFO_,t,&len);
}

// GET path into body (NUL-terminated); returns the body length or -1.
static i64 http_text(const char* path,char* body,u64 cap){
    i64 fd;
    if(tcp_connect_blocking(kHost,HTTP_PORT,5000,&fd)!=0){ if(fd>0) t_close(fd); return -1; }
    set_timeouts(fd,5000,5000);
    char req[128]; u64 n=0;
    const char* parts[]={"GET ",path," HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n"};
    for(int p=0;p<3;p++) for(u64 i=0;parts[p][i]&&n<sizeof req;i++) req[n++]=parts[p][i];
    send_all(fd,req,n,5000);
    static char all[2048]; u64 got=0;
    for(;;){ i64 r=SC4(N_RECV,fd,all+got,sizeof all-1-got,0); if(r<=0) break; got+=(u64)r; if(got>=sizeof all-1) break; }
    t_close(fd);
    all[got]=0;
    for(u64 i=0;i+3<got;i++) if(all[i]=='\r'&&all[i+1]=='\n'&&all[i+2]=='\r'&&all[i+3]=='\n'){
        u64 b=0; for(u64 j=i+4;j<got&&b+1<cap;j++) body[b++]=all[j]; body[b]=0; return (i64)b;
    }
    return -1;
}

// The number after "key=" in text, or -1.
static i64 field(const char* text,const char* key){
    u64 kl=slen(key), tl=slen(text);
    for(u64 i=0;i+kl<=tl;i++) if(smemcmp(text+i,key,kl)==0&&text[i+kl]=='='){
        i64 v=0; u64 j=i+kl+1; int neg=0; if(text[j]=='-'){ neg=1; j++; }
        while(text[j]>='0'&&text[j]<='9') v=v*10+(text[j++]-'0');
        return neg?-v:v;
    }
    return -1;
}

static i64 sink_count(void){ static char b[128]; return http_text("/sinkstat",b,sizeof b)>0?field(b,"n"):-1; }

// Sends n pattern bytes to the sink and closes without shutdown(); returns the
// sink's report once it has one (empty on timeout). *ms_out: until then.
static const char* sink_upload(u64 n,int wait_acked,i64* ms_out,struct TcpInfo* info_out){
    static char report[128]; report[0]=0;
    i64 before=sink_count(), fd, t0=now_ms();
    if(tcp_connect_blocking(kHost,SINK_PORT,5000,&fd)==0){
        set_timeouts(fd,30000,30000);
        static u8 out[16384];
        for(u64 off=0;off<n;){
            u64 c=n-off<sizeof out?n-off:sizeof out;
            for(u64 i=0;i<c;i++) out[i]=pat(off+i);
            i64 s=SC4(N_SEND,fd,out,c,0); if(s<=0) break; off+=(u64)s;
        }
        if(wait_acked){   // all of it acknowledged: the numbers below are final
            i64 deadline=now_ms()+30000; struct TcpInfo t;
            while(now_ms()<deadline){ tcp_info(fd,&t); if(t.unacked==0) break; msleep(20); }
        }
        if(info_out) tcp_info(fd,info_out);
    }
    if(fd>0) t_close(fd);
    i64 deadline=now_ms()+20000;
    while(now_ms()<deadline){
        static char b[128];
        if(http_text("/sinkstat",b,sizeof b)>0 && field(b,"n")>before){ memcpy(report,b,sizeof report); break; }
        msleep(50);
    }
    *ms_out=now_ms()-t0;
    return report;
}

// UDP echo of one datagram of n bytes: 1 when it came back intact.
static int udp_echo_size(u64 n){
    static u8 out[65507], in[65536];
    i64 u=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0); if(u<0) return 0;
    set_timeouts(u,3000,3000);
    struct sockaddr_in a=addr4(kHost,UDP_ECHO_PORT); SC3(N_CONNECT,u,&a,sizeof a);
    for(u64 i=0;i<n;i++) out[i]=pat(i*3);
    int good=0;
    for(int attempt=0;attempt<3&&!good;attempt++){
        if(SC4(N_SEND,u,out,n,0)!=(i64)n) continue;
        i64 r=SC4(N_RECV,u,in,sizeof in,0);
        good=r==(i64)n&&smemcmp(in,out,n)==0;
    }
    t_close(u);
    return good;
}

// TCP options, socket options and the bigger buffers.
static void test_tcp_options(void){
    section("TCP options and info");
    i64 fd;
    i64 r=tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd);
    check(r==0,"connect to host echo");
    if(r==0){
        u64 got=0; tcp_echo_stream(fd,65536,10000,&got);
        struct TcpInfo t; i64 g=tcp_info(fd,&t);
        checkv(g==0&&t.state==1,"TCP_INFO: ESTABLISHED (state)",t.state);
        checkv(t.snd_mss==1460,"MSS learnt from slirp's SYN-ACK (snd_mss)",t.snd_mss);
        checkv(t.rtt>0&&t.rtt<1000000,"an RTT was measured (us)",t.rtt);
        checkv(t.rto>=200000,"RTO at least 200 ms (us)",t.rto);
        checkv(t.snd_cwnd>=10,"congestion window started at 10 segments or more",t.snd_cwnd);
        check((t.wscale&0xf)==0,"no window scaling with slirp, which offers none");
        sw("  (rtt "); sdec(t.rtt); sw(" us, rto "); sdec(t.rto/1000); sw(" ms, cwnd "); sdec(t.snd_cwnd); sw(" segments)\n");
        int one=1, v=0; u32 vl=sizeof v;
        ok(SC5(N_SETSOCKOPT,fd,IPPROTO_TCP_,TCP_NODELAY_,&one,sizeof one),"setsockopt TCP_NODELAY");
        g=SC5(N_GETSOCKOPT,fd,IPPROTO_TCP_,TCP_NODELAY_,&v,&vl);
        checkv(g==0&&v==1,"TCP_NODELAY reads back 1",v);
        vl=sizeof v; g=SC5(N_GETSOCKOPT,fd,IPPROTO_TCP_,TCP_MAXSEG_,&v,&vl);
        checkv(g==0&&v==1460,"TCP_MAXSEG is 1460",v);
    }
    if(fd>0) t_close(fd);

    sw("  (datagrams above one frame leave and come back as IP fragments)\n");
    static const u64 big[]={1473,3000,8000,30000,65507};
    for(int k=0;k<5;k++){
        char name[48]="large UDP echo: "; iname(name,"large UDP echo: ",(int)big[k]);
        check(udp_echo_size(big[k]),name);
    }

    // close() right after a large send: the queue still drains, then the FIN.
    i64 ms; const char* rep=sink_upload(1048576,0,&ms,0);
    checkv(field(rep,"bytes")==1048576&&field(rep,"ok")==1,"1 MiB send + close without shutdown: all of it arrives (bytes)",field(rep,"bytes"));
    print_rate("upload incl. close",(u64)(field(rep,"bytes")>0?field(rep,"bytes"):0),ms);
}

#if NET_LINK
static i64 link_control(const char* text,char* reply,u64 cap){
    i64 u=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0); if(u<0) return u;
    set_timeouts(u,1000,1000);
    struct sockaddr_in a=addr4(kFake,7779); SC3(N_CONNECT,u,&a,sizeof a);
    i64 r=-1;
    for(int attempt=0;attempt<3&&r<=0;attempt++){ SC4(N_SEND,u,text,slen(text),0); r=SC4(N_RECV,u,reply,cap-1,0); }
    if(r>0) reply[r]=0;
    t_close(u);
    return r;
}

static void fake_status(char* out,u64 cap){
    i64 q=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0); out[0]=0; if(q<0) return;
    set_timeouts(q,1000,1000);
    struct sockaddr_in qa=addr4(kFake,7778); SC3(N_CONNECT,q,&qa,sizeof qa);
    for(int attempt=0;attempt<3;attempt++){ SC4(N_SEND,q,"status",6,0); i64 n=SC4(N_RECV,q,out,cap-1,0); if(n>0){ out[n]=0; break; } }
    t_close(q);
}

// The link between the NIC and slirp drops, duplicates, reorders and delays
// TCP frames (host_server.py --link): the stream must still arrive intact.
static void test_tcp_impaired(void){
    section("TCP over a bad link");
    static char st[160], reply[32];
    // Our SYN, as the fake host saw it.
    i64 fd; i64 r=tcp_connect_blocking(kFake,7792,3000,&fd);
    checkv(r==-E_CONNREFUSED,"SYN to the fake host answered by RST",r);
    if(fd>0) t_close(fd);
    fake_status(st,sizeof st);
    checkv(field(st,"syn_mss")==1460,"our SYN offers MSS 1460",field(st,"syn_mss"));
    checkv(field(st,"syn_ws")==3,"our SYN offers window scale 3",field(st,"syn_ws"));

    // slirp sends the downloads: a plain Reno without SACK that waits out a
    // 1 s minimum RTO for most losses, so its link loses less.
    i64 c=link_control("impair loss=1 dup=1 reorder=1",reply,sizeof reply);
    check(c>0&&contains(reply,(u64)c,"ok"),"link: 1% loss, 1% duplicates, 1% reordered");
    static char head[1024]; int bad; i64 t0=now_ms();
    i64 n=http_get(kHost,HTTP_PORT,"10.0.2.2","/pattern/1048576",1,head,sizeof head,90000,&bad);
    checkv(n==1048576&&!bad,"GET 1 MiB over the lossy link, intact",n);
    print_rate("lossy download",(u64)(n>0?n:0),now_ms()-t0);
    if(tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd)==0){
        u64 got=0; int good=tcp_echo_stream(fd,262144,60000,&got);
        checkv(good,"256 KiB echoed intact over the lossy link",(i64)got);
    } else check(0,"256 KiB echoed intact over the lossy link (connect failed)");
    if(fd>0) t_close(fd);

    // Our sender gets the worse link.
    c=link_control("impair loss=3 dup=1 reorder=3",reply,sizeof reply);
    check(c>0&&contains(reply,(u64)c,"ok"),"link: 3% loss, 1% duplicates, 3% reordered");
    struct TcpInfo t; i64 ms;
    const char* rep=sink_upload(524288,1,&ms,&t);
    checkv(field(rep,"bytes")==524288&&field(rep,"ok")==1,"512 KiB upload over the lossy link, intact (bytes)",field(rep,"bytes"));
    checkv(t.total_retrans>0,"... with retransmissions (total_retrans)",t.total_retrans);
    print_rate("lossy upload",(u64)(field(rep,"bytes")>0?field(rep,"bytes"):0),ms);
    sw("  (upload: "); sdec(t.total_retrans); sw(" retransmissions, cwnd "); sdec(t.snd_cwnd); sw(", ssthresh "); sdec(t.snd_ssthresh); sw(")\n");
    fake_status(st,sizeof st);
    checkv(field(st,"dropped")>0,"the link really dropped frames",field(st,"dropped"));

    c=link_control("impair delay=40",reply,sizeof reply);
    check(c>0,"link: 40 ms each way");
    if(tcp_connect_blocking(kHost,TCP_ECHO_PORT,5000,&fd)==0){
        u64 got=0; int good=tcp_echo_stream(fd,65536,30000,&got);
        tcp_info(fd,&t);
        check(good,"64 KiB echo over the slow link");
        checkv(t.rtt>=60000&&t.rtt<500000,"measured RTT near 80 ms (us)",t.rtt);
        checkv(t.rto>=t.rtt/1000*1000&&t.rto<2000000,"RTO follows it (us)",t.rto);
    } else check(0,"64 KiB echo over the slow link (connect failed)");
    if(fd>0) t_close(fd);
    t0=now_ms();
    n=http_get(kHost,HTTP_PORT,"10.0.2.2","/pattern/1048576",1,head,sizeof head,60000,&bad);
    checkv(n==1048576&&!bad,"GET 1 MiB over the slow link, intact",n);
    print_rate("download at 80 ms RTT",(u64)(n>0?n:0),now_ms()-t0);

    c=link_control("impair off",reply,sizeof reply);
    check(c>0,"link back to normal");
}
#endif

#if NET_TOOLS
// Runs `cmd` under bash with its output (stdout and stderr) captured into
// out; returns the exit status, or -1 if bash could not be run.
static i64 sh(const char* cmd,char* out,u64 cap){
    static char script[1024];
    // stdin from /dev/null: the suite runs without one, and curl, finding fd
    // 0 closed, would open pipes until one lands there.
    u64 n=0; const char* parts[]={"{ ",cmd,"; } < /dev/null > /tmp/sh.out 2>&1; rc=$?; echo >> /tmp/sh.out; echo \"rc=$rc\" >> /tmp/sh.out"};
    for(int p=0;p<3;p++) for(u64 i=0;parts[p][i]&&n+1<sizeof script;i++) script[n++]=parts[p][i];
    script[n]=0;
    const char* av[]={"/bin/bash","-c",script,0}; const char* ev[]={"PATH=/bin","HOME=/",0};
    i64 pid=SC3(N_SPAWN,"/bin/bash",av,ev);
    if(pid<=0){ out[0]=0; return -1; }
    int st=0; SC3(N_WAIT,pid,&st,0);
    i64 h=t_open("/tmp/sh.out",O_RDONLY,0), len=0;
    if(h>=0){ len=t_read(h,out,cap-1); t_close(h); }
    if(len<0) len=0;
    out[len]=0;
    // The status is the last "rc=" line.
    i64 rc=-1;
    for(i64 i=len-3;i>=0;i--) if(out[i]=='r'&&out[i+1]=='c'&&out[i+2]=='='&&(i==0||out[i-1]=='\n')){ rc=field(out+i,"rc"); out[i]=0; break; }
    return rc;
}

// Runs cmd and checks its status and that its output has each needle (a
// NULL-terminated list); prints the output when not.
static void tool(const char* cmd,i64 want_rc,const char* const* needles,const char* what){
    static char out[8192];
    i64 rc=sh(cmd,out,sizeof out);
    int good=rc==want_rc;
    for(int i=0;needles&&needles[i]&&good;i++) if(!contains(out,slen(out),needles[i])) good=0;
    checkv(good,what,rc);
    if(!good){ sw("    $ "); sw(cmd); sw("\n"); sw(out); sw("\n"); }
}
#define NEED(...) ((const char* const[]){__VA_ARGS__,0})

// Runs a curl command that saves /pattern/1048576 to /tmp/pattern, then checks
// the file byte by byte and prints the rate.
static void curl_pattern(const char* cmd,const char* what,const char* rate_what){
    static char out[8192];
    i64 t0=now_ms();
    i64 rc=sh(cmd,out,sizeof out);
    i64 ms=now_ms()-t0;
    static u8 buf[65536]; u64 total=0; int bad=0;
    i64 h=t_open("/tmp/pattern",O_RDONLY,0);
    while(h>=0){ i64 r=t_read(h,buf,sizeof buf); if(r<=0) break; for(i64 i=0;i<r;i++) if(buf[i]!=pat(total+(u64)i)) bad=1; total+=(u64)r; }
    if(h>=0) t_close(h);
    SC1(N_UNLINK,"/tmp/pattern");
    checkv(rc==0&&total==1048576&&!bad,what,(i64)total);
    if(rc!=0){ sw("    $ "); sw(cmd); sw("\n"); sw(out); sw("\n"); }
    print_rate(rate_what,total,ms);
}

static void test_tools(void){
    section("network tools");
    tool("netcfg",0,NEED("inet     10.0.2.15/24 (dhcp)","gateway  10.0.2.2","dns      10.0.2.3","lease    "),
         "netcfg shows the DHCP lease");
    tool("netcfg renew",0,NEED("(dhcp)"),"netcfg renew");
    tool("ping -c 3 -i 0.2 10.0.2.2",0,NEED("icmp_seq=1 time=","icmp_seq=3","3 packets transmitted, 3 received, 0% packet loss","rtt min/avg/max/mdev"),
         "ping -c 3 the gateway");
    tool("ping -c 2 -i 0.2 -s 3000 10.0.2.2",0,NEED("3008 bytes from 10.0.2.2","2 received"),"ping -s 3000 (fragmented both ways)");
    tool("ping -c 2 -i 0.2 127.0.0.1",0,NEED("64 bytes from 127.0.0.1","2 received"),"ping localhost");
    tool("ping -c 2 -i 0.2 -W 1 10.0.2.99",1,NEED("2 packets transmitted, 0 received, 100% packet loss"),"ping a host that is not there: 100% loss, status 1");
    tool("ping -c 1 no-such-host.invalid",2,NEED("ping: no-such-host.invalid"),"ping an unknown name: status 2");
#if NET_LINK
    tool("ping -c 2 -i 0.2 10.0.2.77",0,NEED("from 10.0.2.77","2 received"),"ping the fake host");
    tool("nslookup web.test 10.0.2.77",0,NEED("Server:\t\t10.0.2.77","Name:\tweb.test","Address: 10.0.2.2"),"nslookup web.test at the fake host");
    tool("host alias.test 10.0.2.77",0,NEED("alias.test is an alias for web.test.","web.test has address 10.0.2.2"),"host follows a CNAME");
    tool("host -t MX mail.test 10.0.2.77",0,NEED("mail.test mail is handled by 10 web.test."),"host -t MX");
    tool("nslookup -type=TXT txt.test 10.0.2.77",0,NEED("text = \"hello world\""),"nslookup -type=TXT");
    tool("nslookup 10.0.2.2 10.0.2.77",0,NEED("2.2.0.10.in-addr.arpa\tname = web.test."),"nslookup of an address asks for its PTR");
    tool("host nothere.test 10.0.2.77",1,NEED("Host nothere.test not found: 3(NXDOMAIN)"),"host: NXDOMAIN, status 1");
    tool("host -W 1 web.test 10.0.2.99",1,NEED("timed out"),"host: a server that never answers times out");
#endif
#if NET_INTERNET
    tool("host example.com",0,NEED("example.com has address"),"host example.com (server from resolv.conf)");
    tool("nslookup example.com",0,NEED("Server:\t\t10.0.2.3","Address: "),"nslookup example.com");
#endif
#if HAVE_CURL
    tool("curl -sS http://10.0.2.2:" STR(HTTP_PORT) "/hello",0,NEED("hello from the host"),"curl GET /hello");
    curl_pattern("curl -sS -o /tmp/pattern http://10.0.2.2:" STR(HTTP_PORT) "/pattern/1048576",
                 "curl downloads 1 MiB intact (bytes)","curl download incl. start-up");
    tool("curl -s -o /tmp/x -w 'code=%{http_code}' http://10.0.2.2:" STR(HTTP_PORT) "/nope",0,NEED("code=404"),"curl reports a 404");
    tool("curl -sS http://10.0.2.2:" STR(CLOSED_PORT) "/",7,NEED("Failed to connect"),"curl to a closed port: status 7");
#if NET_LINK
    tool("curl -sS --connect-timeout 5 http://10.0.2.77:80/",7,NEED("Failed to connect"),"curl answered by ICMP host unreachable: status 7");
#endif
#if NET_INTERNET
    tool("curl -sS http://example.com/",0,NEED("Example Domain"),"curl http://example.com/");
#endif
#endif
}

#if HAVE_TLS
#define HTTPS "https://10.0.2.2:" STR(HTTPS_PORT)
#define TEST_CA "--cacert /etc/ssl/test-ca.pem "
static void test_tls(void){
    section("HTTPS (curl + mbedTLS)");
    tool("curl -sS " TEST_CA HTTPS "/hello",0,NEED("hello from the host"),"https:// verified against the test CA (IP in the certificate)");
    tool("curl -sS " TEST_CA "--resolve web.test:" STR(HTTPS_PORT) ":10.0.2.2 https://web.test:" STR(HTTPS_PORT) "/hello",0,
         NEED("hello from the host"),"https:// to a DNS name in the certificate");
    tool("curl -sS " TEST_CA "--resolve other.test:" STR(HTTPS_PORT) ":10.0.2.2 https://other.test:" STR(HTTPS_PORT) "/hello",60,
         0,"https:// to a name the certificate does not cover: status 60");
#if HAVE_CA_BUNDLE
    tool("curl -sS " HTTPS "/hello",60,0,"https:// with only the system CAs: unknown issuer, status 60");
#else
    tool("curl -sS " HTTPS "/hello",77,0,"https:// without a CA bundle: status 77");
#endif
    tool("curl -sSk " HTTPS "/hello",0,NEED("hello from the host"),"curl -k skips verification");
    tool("curl -sS -v --tlsv1.3 " TEST_CA HTTPS "/hello",0,NEED("TLSv1.3","hello from the host"),"TLS 1.3");
    tool("curl -sS -v --tlsv1.2 --tls-max 1.2 " TEST_CA HTTPS "/hello",0,NEED("TLSv1.2","hello from the host"),"TLS 1.2");
    curl_pattern("curl -sS " TEST_CA "-o /tmp/pattern " HTTPS "/pattern/1048576",
                 "https:// downloads 1 MiB intact (bytes)","https download incl. start-up and handshake");
#if NET_INTERNET && HAVE_CA_BUNDLE
    tool("curl -sS https://example.com/",0,NEED("Example Domain"),"curl https://example.com/ (system CA bundle)");
    tool("curl -sS -o /dev/null -w 'code=%{http_code}' https://curl.se/",0,NEED("code=200"),"curl https://curl.se/");
#endif
}
#endif

// Host Unix time in ms from GET /time ("S.US"), -1 on failure.
static i64 host_time_ms(void){
    char body[64];
    if(http_text("/time",body,sizeof body)<=0) return -1;
    i64 s=0,frac=0,digits=0; const char* p=body;
    while(*p>='0'&&*p<='9') s=s*10+(*p++-'0');
    if(*p=='.') p++;
    while(*p>='0'&&*p<='9'&&digits<3){ frac=frac*10+(*p++-'0'); digits++; }
    while(digits++<3) frac*=10;
    return s*1000+frac;
}
static i64 guest_time_ms(void){ return SC0(N_GETUNIXTIMENS)/1000000; }
static i64 iabs(i64 v){ return v<0?-v:v; }

static void test_clock(void){
    section("wall clock and NTP");
    i64 ns=SC0(N_GETUNIXTIMENS), s=SC0(N_GETUNIXTIME);
    checkv(ns>0&&(s==ns/1000000000||s==ns/1000000000+1),"GetUnixTimeNs agrees with GetUnixTime",ns);
    i64 h=host_time_ms(), g=guest_time_ms();
    checkv(h>0&&iabs(g-h)<5000,"wall clock within 5 s of the host (ms off)",g-h);
    err(SC1(N_SETUNIXTIME,1000),22,"SetUnixTime before boot: EINVAL");
#if NET_LINK
    tool("sntp 10.0.2.77",0,NEED("10.0.2.77 (10.0.2.77): stratum 2, offset "),"sntp asks the fake host");
    tool("sntp -W 1 -r 1 10.0.2.99",1,NEED("no answer from 10.0.2.99"),"sntp: a server that never answers, status 1");
    tool("sntp -W 1 -r 1 10.0.2.2",1,NEED("sntp:"),"sntp: port unreachable, status 1");
    // 2001-01-01: every certificate in use is not valid yet.
    i64 r=SC1(N_SETUNIXTIME,978307200LL*1000000000LL);
    s=SC0(N_GETUNIXTIME);
    checkv(r==0&&s>=978307200&&s<978307200+5,"SetUnixTime puts the clock in 2001",s);
#if HAVE_TLS
    tool("curl -sS " TEST_CA HTTPS "/hello",60,0,"https:// with the clock in 2001: certificate not yet valid, status 60");
#endif
    tool("sntp -s 10.0.2.77",0,NEED("clock stepped by +"),"sntp -s steps the clock back to now");
    h=host_time_ms(); g=guest_time_ms();
    checkv(h>0&&iabs(g-h)<1000,"after sntp -s the clock is within 1 s of the host (ms off)",g-h);
    if(h>0&&iabs(g-h)>=1000) SC1(N_SETUNIXTIME,h*1000000);   // keep the rest of the run sane
#if HAVE_TLS
    tool("curl -sS " TEST_CA HTTPS "/hello",0,NEED("hello from the host"),"https:// works again");
#endif
#endif
#if NET_INTERNET
    tool("sntp pool.ntp.org",0,NEED("pool.ntp.org ("," offset "),"sntp pool.ntp.org");
#endif
}
#endif

// GET path; returns the body length received. *bad_out: 1 = a /pattern byte
// was wrong, 2 = timed out or failed before EOF (length is what arrived).
static i64 http_get(u32 ip,u16 port,const char* host,const char* path,int verify_pattern,
                    char* head,u64 headcap,i64 timeout_ms,int* bad_out){
    i64 fd; *bad_out=0;
    if(tcp_connect_blocking(ip,port,5000,&fd)!=0){ if(fd>0) t_close(fd); return -1; }
    set_timeouts(fd,timeout_ms,5000);
    char req[256]; u64 n=0;
    const char* parts[]={"GET ",path," HTTP/1.1\r\nHost: ",host,"\r\nConnection: close\r\n\r\n"};
    for(int p=0;p<5;p++) for(u64 i=0;parts[p][i]&&n<sizeof req;i++) req[n++]=parts[p][i];
    if(send_all(fd,req,n,5000)!=(i64)n){ t_close(fd); return -2; }
    static u8 buf[8192];
    u64 hl=0; int in_body=0; u64 body=0; i64 deadline=now_ms()+timeout_ms;
    for(;;){
        i64 r=SC4(N_RECV,fd,buf,sizeof buf,0);
        if(r<=0||now_ms()>deadline){ if(r!=0) *bad_out|=2; break; }
        i64 i=0;
        while(!in_body && i<r){
            if(hl+1<headcap) head[hl++]=(char)buf[i];
            i++;
            if(hl>=4&&head[hl-4]=='\r'&&head[hl-3]=='\n'&&head[hl-2]=='\r'&&head[hl-1]=='\n') in_body=1;
        }
        if(in_body){
            if(verify_pattern) for(i64 j=i;j<r;j++) if(buf[j]!=pat(body+(u64)(j-i))) *bad_out|=1;
            body+=(u64)(r-i);
        }
    }
    head[hl<headcap?hl:headcap-1]=0;
    t_close(fd);
    return (i64)body;
}

static void test_http(void){
    section("HTTP (host)");
    static char head[1024]; int bad;
    i64 n=http_get(kHost,HTTP_PORT,"10.0.2.2","/hello",0,head,sizeof head,10000,&bad);
    checkv(n==20,"GET /hello: 20-byte body until EOF",n);
    check(contains(head,slen(head),"HTTP/1.1 200"),"GET /hello: status 200");
    static const u64 sizes[]={100000,1048576};
    static u64 before[16], after[16];
    for(int k=0;k<2;k++){
        char path[32]="/pattern/"; iname(path,"/pattern/",(int)sizes[k]);
        i64 cpus=device_irqs(before);
        i64 t0=now_ms();
        n=http_get(kHost,HTTP_PORT,"10.0.2.2",path,1,head,sizeof head,60000,&bad);
        const char* name=k==0?"GET 100 KB pattern, intact":"GET 1 MiB pattern, intact";
        if(bad==2 && k==1) gap("1 MiB download did not finish in 60 s (bytes received)",n);   // throughput
        else checkv(n==(i64)sizes[k]&&!bad,name,n);
        print_rate("download",(u64)(n>0?n:0),now_ms()-t0);
        if(k==1){
            // ~730 segments: a receive interrupt per batch of them, not none.
            device_irqs(after);
            u64 total=0; i64 busiest=0;
            for(i64 i=0;i<cpus;i++){ total+=after[i]-before[i]; if(after[i]-before[i]>after[busiest]-before[busiest]) busiest=i; }
            // A polled NIC (e1000, rtl8139) raises none; run-net-suite.sh
            // requires the MSI-X line for the default virtio-net.
            if(total==0) gap("no NIC receive interrupts: the NIC is polled",0);
            else checkv(total>=20,"the 1 MiB download raises NIC receive interrupts",(i64)total);
            sw("  (device interrupts during the download: "); sdec((i64)total); sw(", most on cpu "); sdec(busiest); sw(")\n");
        }
    }
}

#if NET_INTERNET
// Minimal DNS A query over UDP to slirp's forwarder; returns the first address.
static u32 dns_a(const char* name,i64* rcode){
    *rcode=-1;
    i64 fd=SC3(N_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0); if(fd<0) return 0;
    struct sockaddr_in a=addr4(kSlirpDns,53); SC3(N_CONNECT,fd,&a,sizeof a);
    u8 q[300]; u64 n=0;
    u8 hdr[12]={0x4e,0x53,0x01,0x00,0,1,0,0,0,0,0,0}; memcpy(q,hdr,12); n=12;
    for(const char* p=name;*p;){ const char* e=p; while(*e&&*e!='.') e++; q[n++]=(u8)(e-p); while(p<e) q[n++]=(u8)*p++; if(*p=='.') p++; }
    q[n++]=0; q[n++]=0; q[n++]=1; q[n++]=0; q[n++]=1;
    u32 ip=0; static u8 r[1500];
    for(int attempt=0;attempt<3&&!ip;attempt++){
        SC4(N_SEND,fd,q,n,0);
        i64 len=recv_wait(fd,r,sizeof r,2000);
        if(len<12||r[0]!=0x4e||r[1]!=0x53) continue;
        *rcode=r[3]&0xf;
        u64 an=((u64)r[6]<<8)|r[7], off=n;    // answers follow the echoed question
        for(u64 i=0;i<an&&off+12<=(u64)len;i++){
            if(r[off]&0xc0) off+=2; else { while(off<(u64)len&&r[off]) off+=r[off]+1; off++; }
            u16 type=(u16)((r[off]<<8)|r[off+1]); u16 rdlen=(u16)((r[off+8]<<8)|r[off+9]); off+=10;
            if(type==1&&rdlen==4&&off+4<=(u64)len){ memcpy(&ip,r+off,4); break; }
            off+=rdlen;
        }
    }
    t_close(fd);
    return ip;
}

static void spawn_wait_app(const char* path){
    i64 h=t_open(path,O_RDONLY,0);
    if(h<0){ sw("  ("); sw(path); sw(" not in initrd, skipped)\n"); return; }
    t_close(h);
    const char* av[]={path,0}; const char* ev[]={"PATH=/bin",0};
    i64 pid=SC3(N_SPAWN,path,av,ev);
    checkv(pid>0,path,pid);
    if(pid>0){ int st=0; SC3(N_WAIT,pid,&st,0); }   // markers are checked on the serial log
}

static void test_internet(void){
    section("internet (DNS + example.com)");
    i64 rc; u32 ip=dns_a("example.com",&rc);
    checkv(ip!=0,"DNS A example.com via 10.0.2.3",rc);
    if(ip){ sw("  (example.com -> "); sdec(ip&0xff); sw("."); sdec((ip>>8)&0xff); sw("."); sdec((ip>>16)&0xff); sw("."); sdec(ip>>24); sw(")\n"); }
    if(ip){
        static char head[1024]; int bad;
        i64 n=http_get(ip,80,"example.com","/",0,head,sizeof head,20000,&bad);
        checkv(n>0,"GET http://example.com/ returns a body",n);
        check(contains(head,slen(head),"HTTP/1.1 200"),"example.com: status 200");
        // HTTP/1.0: the whole response tends to arrive in one segment with FIN.
        i64 fd; static u8 buf[4096]; u64 got=0;
        if(tcp_connect(ip,80,5000,&fd)==0){
            const char* req="GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
            send_all(fd,req,slen(req),5000);
            for(;;){ i64 r=recv_wait(fd,buf,sizeof buf,10000); if(r<=0) break; got+=(u64)r; }
        }
        if(fd>0) t_close(fd);
        checkv(got>0,"HTTP/1.0 GET example.com returns a body",(i64)got);
    }
    spawn_wait_app("/bin/dns-smoke");
    spawn_wait_app("/bin/http-smoke");
}
#endif

// The kernel keeps its log for dmesg, serial port or not.
static void test_kernel_log(void){
    section("kernel log (KernelLog, dmesg)");
    static char log[131072];
    i64 n=SC2(N_KERNELLOG,log,sizeof log);
    checkv(n>0,"KernelLog returns the log (bytes)",n);
    check(n>0&&contains(log,(u64)n,"[net] link up"),"... with the link coming up");
    check(n>0&&contains(log,(u64)n,"[net] dhcp: 10.0.2.15"),"... and the DHCP lease");
#if NET_TOOLS
    tool("dmesg > /tmp/klog; echo rc=$?",0,NEED("rc=0"),"dmesg runs");
    i64 h=t_open("/tmp/klog",O_RDONLY,0), got=0;
    while(h>=0&&got<(i64)sizeof log){ i64 r=t_read(h,log+got,sizeof log-(u64)got); if(r<=0) break; got+=r; }
    if(h>=0) t_close(h);
    check(got>0&&contains(log,(u64)got,"[net] dhcp: 10.0.2.15"),"dmesg prints it");
    SC1(N_UNLINK,"/tmp/klog");
#endif
}

#if HAVE_QMP
// Pulling the cable: the host takes the NIC's link down over QMP for 3 s. The
// stack must see it go and come back, then confirm its lease by broadcast.
static void test_link(void){
    section("link down / up (QMP set_link)");
    static char b[64];
    struct NetConfig c; get_config(&c,0,0);
    u64 gen=c.generation, gen0=c.generation;
    i64 r=http_text("/linkflap/3000",b,sizeof b);
    checkv(r>0,"the host takes the link down for 3 s",r);
    int down=0, up=0; i64 t0=now_ms(), t_down=-1, t_up=-1;
    while(!up&&now_ms()-t0<15000){
        get_config(&c,gen,500);
        gen=c.generation;
        if(!down&&c.linkUp==0){ down=1; t_down=now_ms()-t0; }
        if(down&&c.linkUp==1){ up=1; t_up=now_ms()-t0; }
    }
    checkv(down,"NetGetConfig reports the link down (ms)",t_down);
    checkv(up,"... and up again (ms)",t_up);
    check(SC0(N_NETLINK)==1,"NetLinkStatus agrees");
    // The REQUEST goes out at once and its ACK binds the lease again: a third
    // change after down and up (on virtio it can beat the loop above to it).
    t0=now_ms(); int bound=0;
    for(;;){
        bound=c.generation>=gen0+3&&c.flags==(CFG_UP|CFG_DHCP)&&c.address==kGuest&&c.linkUp==1;
        if(bound||now_ms()-t0>=10000) break;
        get_config(&c,c.generation,500);
    }
    checkv(bound,"the lease is confirmed after the link returns (ms)",now_ms()-t0);
    check(quick_echo(),"TCP echo works after the outage");
}
#endif

__attribute__((force_align_arg_pointer))
void _start(void){
    SC2(N_SIGNAL,SIG_PIPE,1);
    sw("\n[netsuite] BEGIN InstantOS network suite\n");
    test_nic();
    if(g_have_nic){
        test_config();
        test_ping();
        test_udp();
        test_tcp();
        test_blocking();
#if NET_LINK
        test_icmp();
#endif
        test_http();
        test_tcp_options();
#if NET_LINK
        test_tcp_impaired();
#endif
#if NET_INTERNET
        test_internet();
#endif
#if NET_TOOLS
        test_tools();
#if HAVE_TLS
        test_tls();
#endif
        test_clock();
#endif
        test_down_up();
        test_kernel_log();
#if HAVE_QMP
        test_link();
#endif
    }
    section("");
    // One write, so a service logging at the same moment cannot split it.
    static char score[128]; u64 n=0;
    const char* parts[]={"\n[netsuite] SCORE pass="," total="," fail="," gaps=","\n[netsuite] END\n"};
    i64 values[]={g_pass,g_total,g_total-g_pass,g_gaps};
    for(int p=0;p<5;p++){
        for(const char* c=parts[p];*c;c++) score[n++]=*c;
        if(p<4){ char d[24]; int i=23; u64 v=(u64)values[p]; d[23]=0; do{ d[--i]=(char)('0'+v%10); v/=10; }while(v); for(char* c=&d[i];*c;c++) score[n++]=*c; }
    }
    swn(score,n);
    for(;;){ SC1(N_SLEEP,1000); }
}
