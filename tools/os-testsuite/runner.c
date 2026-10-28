// InstantOS in-OS feature test suite (freestanding, raw syscalls, no libc).
//
// Runs as /bin/login so it owns the session and can exercise the maximum
// headless-testable surface: process/threads, memory, VFS across RamFS/ext4/
// initrd/devfs, files/dirs/links/metadata, PTY, pipes, dup/fcntl, poll, signals,
// time/entropy, TCP+UDP loopback sockets, IPC/services, shared memory/surfaces,
// users/sessions, and negative/error paths. Every check increments a tally; the
// final "[ostest] SCORE ..." line is machine-parseable.
//
// Design rules to keep the runner itself from hanging/crashing (which would end
// the run): never read a blocking fd with no data, use non-blocking sockets +
// bounded retries, send before recv, receive IPC with wait=0, install a SIGPIPE
// handler before pipe tests, exit forked children immediately, and put the
// riskiest tests (fork/signals/threads) last so earlier results are already on
// the serial log.


// Scaled memory soak size (MiB). Default is safe on the 1 GiB CI VM; the
// dedicated large-RAM boot overrides it via -DSOAK_MB=... (see build script).
#ifndef SOAK_MB
#define SOAK_MB 64
#endif

// Entries for the large-directory ext4 stress test. Default keeps every-boot CI
// fast; the dedicated large-FS boot overrides it (-DBIGDIR_N=10000) to validate
// the roadmap's 10k+ target (TCG's per-AHCI-command latency makes 10k slow).
#ifndef BIGDIR_N
#define BIGDIR_N 2000
#endif

#include "ostest.h"

// ============================ TEST SECTIONS ============================

static void test_osinfo(void){
    section("osinfo/identity");
    char buf[128]; memset(buf,0,sizeof buf);
    i64 r=SC1(N_OSINFO,buf); ok(r,"osinfo returns ok");
    check(buf[0]!=0,"osinfo osname non-empty");
    i64 pid=SC0(N_GETPID); checkv(pid>0,"getpid > 0",pid);
    i64 ppid=SC0(N_GETPPID); checkv(ppid>=0,"getppid >= 0",ppid);
    i64 uid=SC0(N_GETUID); checkv(uid==0,"getuid == 0 (root)",uid);
    i64 gid=SC0(N_GETGID); checkv(gid>=0,"getgid >= 0",gid);
    i64 sid=SC0(N_GETSESSIONID); checkv(sid>=0,"getsessionid >= 0",sid);
    // getuserinfo(0) -> UserInfo
    struct { u32 uid,gid; char name[32]; char home[256]; char shell[256]; } ui; memset(&ui,0,sizeof ui);
    i64 gu=SC2(N_GETUSERINFO,0,&ui); check(gu==0||gu==-1,"getuserinfo(0) returns");
}

static void test_time_entropy(void){
    section("time/entropy");
    i64 t1=SC0(N_GETTIME); i64 t2=SC0(N_GETTIME);
    checkv(t1>=0&&t2>=t1,"gettime monotonic",t2);
    i64 before=SC0(N_GETTIME); msleep(60); i64 after=SC0(N_GETTIME);
    checkv(after-before>=40,"sleep(60) elapsed >= 40ms",after-before);
    i64 ut=SC0(N_GETUNIXTIME); checkv(ut>1600000000,"unixtime > 2020",ut);
    u8 e1[32],e2[32]; memset(e1,0,32); memset(e2,0,32);
    i64 g1=SC2(N_GETENTROPY,e1,32); ok(g1,"getentropy(32) ok");
    i64 g2=SC2(N_GETENTROPY,e2,32); ok(g2,"getentropy(32) again ok");
    int nz=0; for(int i=0;i<32;i++) if(e1[i]) nz=1; check(nz,"entropy not all-zero");
    check(smemcmp(e1,e2,32)!=0,"two entropy reads differ");
    err(SC2(N_GETENTROPY,e1,300),E_INVAL,"getentropy(300) -> EINVAL");
}

static void test_memory(void){
    section("memory (mmap/mprotect/munmap)");
    i64 a=SC3(N_MMAP,0,8192,PROT_R|PROT_W);
    checkv(a>0,"mmap 8k returns addr",a);
    if(a>0){
        volatile u8* p=(volatile u8*)a;
        int zero=1; for(int i=0;i<8192;i++) if(p[i]!=0) zero=0;
        check(zero,"mmap memory zero-filled");
        for(int i=0;i<8192;i++) p[i]=(u8)(i&0xff);
        int okp=1; for(int i=0;i<8192;i++) if(p[i]!=(u8)(i&0xff)) okp=0;
        check(okp,"mmap memory read-back");
        ok(SC3(N_MPROTECT,a,4096,PROT_R),"mprotect RO ok");
        ok(SC3(N_MPROTECT,a,4096,PROT_R|PROT_W),"mprotect RW restore");
        ok(SC2(N_MUNMAP,a,8192),"munmap ok");
    }
    i64 big=SC3(N_MMAP,0,1024*1024,PROT_R|PROT_W);
    checkv(big>0,"mmap 1MB returns addr",big);
    if(big>0){ ((volatile u8*)big)[1024*1024-1]=7; ok(SC2(N_MUNMAP,big,1024*1024),"munmap 1MB"); }
}

static void test_files_tmp(void){
    section("files (RamFS /tmp)");
    const char* P="/tmp/ost_a.txt";
    i64 h=t_open(P,O_RDWR|O_CREAT|O_TRUNC,0644); checkv(h>0,"open O_CREAT|RDWR",h);
    if(h>0){
        i64 w=t_write(h,"hello",5); checkv(w==5,"write 5 bytes",w);
        struct Stat st; memset(&st,0,sizeof st);
        checkv(SC2(N_FSTAT,h,&st)==0&&st.size==5,"fstat size==5",(i64)st.size);
        check((st.mode&S_IFMT)==S_IFREG,"fstat mode is regular");
        i64 s=SC3(N_SEEK,h,0,SEEK_SET); checkv(s==0,"seek SET 0",s);
        char rb[8]; memset(rb,0,8); i64 r=t_read(h,rb,5);
        check(r==5&&smemcmp(rb,"hello",5)==0,"read back 'hello'");
        i64 e=SC3(N_SEEK,h,0,SEEK_END); checkv(e==5,"seek END==5",e);
        ok(SC3(N_TRUNCATE,h,2,1),"ftruncate to 2");
        memset(&st,0,sizeof st); SC2(N_FSTAT,h,&st); checkv(st.size==2,"size after truncate==2",(i64)st.size);
        t_close(h);
    }
    // O_APPEND
    i64 ha=t_open(P,O_WRONLY|O_APPEND,0); if(ha>0){ t_write(ha,"XYZ",3); t_close(ha); }
    struct Stat st2; memset(&st2,0,sizeof st2); SC2(N_STAT,P,&st2); checkv(st2.size==5,"append grew to 5",(i64)st2.size);
    ok(t_unlink(P),"unlink");
    err(SC2(N_STAT,P,&st2),E_NOENT,"stat after unlink -> ENOENT");
}

// P1.4: build-grade write-path stress on the real ext4 root (not RamFS).
// Skips gracefully if the root FS is not writable (e.g. a FAT32 dev image).
static void test_ext4_fs(void){
    section("ext4 root write-path (build-grade)");
    i64 mk=SC2(N_MKDIR,"/ext4t",0755);
    if(mk<0 && mk!=-E_EXIST){ sw("  note: root not writable ext4, skipping\n"); return; }

    // small create/write/read
    ok(put_file("/ext4t/a.txt","hello ext4!",11),"create+write small");
    { char b[16]; memset(b,0,16); i64 h=t_open("/ext4t/a.txt",O_RDONLY,0); i64 r=(h>=0)?t_read(h,b,16):-1; if(h>=0)t_close(h);
      checkv(r==11&&smemcmp(b,"hello ext4!",11)==0,"read-back small",r); }

    // large multi-block (256 KiB) file write + verify
    { i64 h=t_open("/ext4t/big.bin",O_WRONLY|O_CREAT|O_TRUNC,0644); checkv(h>=0,"open big",h);
      static u8 buf[4096]; for(int i=0;i<4096;i++) buf[i]=(u8)((i*7)&0xff);
      int okw=(h>=0); for(int blk=0;blk<64&&okw;blk++) if(t_write(h,buf,4096)!=4096) okw=0; if(h>=0)t_close(h);
      check(okw,"write 256KiB multi-block");
      h=t_open("/ext4t/big.bin",O_RDONLY,0); int okr=(h>=0); u64 tot=0;
      for(int blk=0;blk<64&&okr;blk++){ i64 r=t_read(h,buf,4096); if(r!=4096){okr=0;break;} tot+=(u64)r;
        for(int i=0;i<4096;i++) if(buf[i]!=(u8)((i*7)&0xff)){okr=0;break;} }
      if(h>=0)t_close(h); checkv(okr&&tot==256*1024,"read-back 256KiB verify",(i64)tot); }

    // append
    { i64 h=t_open("/ext4t/a.txt",O_WRONLY|O_APPEND,0); if(h>=0){t_write(h,"MORE",4);t_close(h);}
      struct Stat st; memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/a.txt",&st); checkv(st.size==15,"append grew to 15",(i64)st.size); }

    // truncate shrink + sparse grow
    { ok(SC3(N_TRUNCATE,"/ext4t/a.txt",5,0),"truncate to 5");
      struct Stat st; memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/a.txt",&st); checkv(st.size==5,"size==5",(i64)st.size);
      ok(SC3(N_TRUNCATE,"/ext4t/a.txt",100,0),"grow to 100 (sparse)");
      memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/a.txt",&st); checkv(st.size==100,"size==100",(i64)st.size); }

    // rename same-dir + overwrite existing target
    { struct Stat st; ok(SC2(N_RENAME,"/ext4t/a.txt","/ext4t/b.txt"),"rename a->b");
      err(SC2(N_STAT,"/ext4t/a.txt",&st),E_NOENT,"old name gone");
      put_file("/ext4t/victim.txt","old",3);
      ok(SC2(N_RENAME,"/ext4t/b.txt","/ext4t/victim.txt"),"rename overwrites target");
      err(SC2(N_STAT,"/ext4t/b.txt",&st),E_NOENT,"source gone after overwrite"); }

    // cross-dir rename + hardlink + unlink
    { struct Stat st; ok(SC2(N_MKDIR,"/ext4t/sub",0755),"mkdir sub");
      ok(SC2(N_RENAME,"/ext4t/victim.txt","/ext4t/sub/moved.txt"),"cross-dir rename");
      ok(SC2(N_LINK,"/ext4t/sub/moved.txt","/ext4t/hl.txt"),"hardlink");
      memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/hl.txt",&st); checkv(st.nlink==2,"nlink==2 after link",(i64)st.nlink);
      ok(t_unlink("/ext4t/sub/moved.txt"),"unlink one link");
      memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/hl.txt",&st); checkv(st.nlink==1,"nlink==1 after unlink",(i64)st.nlink); }

    // symlink + readlink
    { ok(SC2(N_SYMLINK,"hl.txt","/ext4t/sym"),"symlink");
      char lb[32]; memset(lb,0,32); i64 r=SC3(N_READLINK,"/ext4t/sym",lb,32); checkv(r==6&&smemcmp(lb,"hl.txt",6)==0,"readlink",r);
      // unlink on a symlink must remove the link itself (POSIX), not its target
      ok(t_unlink("/ext4t/sym"),"unlink removes the symlink itself");
      struct Stat ls; err(SC2(N_LSTAT,"/ext4t/sym",&ls),E_NOENT,"symlink gone after unlink");
      ok(SC2(N_SYMLINK,"hl.txt","/ext4t/sym"),"recreate symlink");
      // dangling symlink (target absent) must still be unlinkable (POSIX)
      ok(SC2(N_SYMLINK,"/ext4t/no_such_target","/ext4t/dang"),"create dangling symlink");
      ok(t_unlink("/ext4t/dang"),"unlink dangling symlink");
      err(SC2(N_LSTAT,"/ext4t/dang",&ls),E_NOENT,"dangling symlink gone"); }

    // chmod + chown persist
    { struct Stat st; ok(SC3(N_CHMOD,"/ext4t/hl.txt",0600,0),"chmod 600");
      memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/hl.txt",&st); checkv((st.mode&0777)==0600,"mode==600",(i64)(st.mode&0777));
      ok(SC5(N_CHOWN,"/ext4t/hl.txt",0,123,456,0),"chown 123:456");
      memset(&st,0,sizeof st); SC2(N_STAT,"/ext4t/hl.txt",&st); checkv(st.uid==123&&st.gid==456,"uid/gid persisted",(i64)st.uid); }

    // fsync: durability barrier on an open fd (flushes device write cache)
    { i64 h=t_open("/ext4t/fs.txt",O_WRONLY|O_CREAT|O_TRUNC,0644); checkv(h>=0,"open for fsync",h);
      if(h>=0){ t_write(h,"durable",7); ok(SC1(N_FSYNC,h),"fsync open fd"); t_close(h); }
      err(SC1(N_FSYNC,99),E_BADF,"fsync bad fd -> EBADF");
      t_unlink("/ext4t/fs.txt"); }

    // extent-mapped file O_TRUNC: /readme.txt is created extent-mapped by mke2fs;
    // O_TRUNC must convert it to block-mapped and allow rewrite (restore after).
    { struct Stat st0; if(SC2(N_STAT,"/readme.txt",&st0)==0){
        i64 h=t_open("/readme.txt",O_WRONLY|O_TRUNC,0644);
        checkv(h>=0,"O_TRUNC open of extent-mapped file (convert to block-mapped)",h);
        if(h>=0){ t_write(h,"converted",9); t_close(h); }
        char b[16]; memset(b,0,16); h=t_open("/readme.txt",O_RDONLY,0); i64 r=(h>=0)?t_read(h,b,16):-1; if(h>=0)t_close(h);
        checkv(r==9&&smemcmp(b,"converted",9)==0,"rewrite after extent->blockmap conversion",r);
        put_file("/readme.txt","InstantOS ext4 in-OS test seed\n",31);   // restore seed
      } }

    // mtime uses the real clock AND advances on write (the GNU make fix)
    { i64 h=t_open("/ext4t/mt.txt",O_WRONLY|O_CREAT|O_TRUNC,0644); if(h>=0){t_write(h,"1",1);t_close(h);}
      struct Stat s1; memset(&s1,0,sizeof s1); SC2(N_STAT,"/ext4t/mt.txt",&s1);
      checkv((i64)s1.mtime>1700000000,"mtime from real clock (not frozen constant)",(i64)s1.mtime);
      msleep(1100);
      h=t_open("/ext4t/mt.txt",O_WRONLY|O_APPEND,0); if(h>=0){t_write(h,"2",1);t_close(h);}
      struct Stat s2; memset(&s2,0,sizeof s2); SC2(N_STAT,"/ext4t/mt.txt",&s2);
      checkv((i64)s2.mtime>(i64)s1.mtime,"mtime advances on write (make dependency)",(i64)s2.mtime); }

    // moderate directory: 300 files, readdir, unlink all
    { ok(SC2(N_MKDIR,"/ext4t/many",0755),"mkdir many");
      char nm[48]; int made=0; for(int i=0;i<300;i++){ iname(nm,"/ext4t/many/f",i); if(put_file(nm,"x",1)==0) made++; }
      checkv(made==300,"created 300 files",made);
      static struct DirEntry de[512]; memset(de,0,sizeof de); i64 n=SC3(N_READDIR,"/ext4t/many",de,512);
      checkv(n>=302,"readdir >=302 entries (. .. +300)",n);
      int rm=0; for(int i=0;i<300;i++){ iname(nm,"/ext4t/many/f",i); if(t_unlink(nm)==0) rm++; }
      checkv(rm==300,"unlinked 300 files",rm);
      ok(SC1(N_RMDIR,"/ext4t/many"),"rmdir many"); }

    // large directory: BIGDIR_N entries (exercises the block cache + linear dir
    // scaling; empty files keep per-entry block allocation bounded).
    { ok(SC2(N_MKDIR,"/ext4t/huge",0755),"mkdir huge");
      char nm[48]; int made=0;
      for(int i=0;i<BIGDIR_N;i++){ iname(nm,"/ext4t/huge/f",i); i64 h=t_open(nm,O_WRONLY|O_CREAT,0644); if(h>=0){t_close(h);made++;} }
      checkv(made==BIGDIR_N,"created BIGDIR_N directory entries",made);
      struct Stat st; int look=1;
      iname(nm,"/ext4t/huge/f",0);          if(SC2(N_STAT,nm,&st)!=0) look=0;
      iname(nm,"/ext4t/huge/f",BIGDIR_N/2); if(SC2(N_STAT,nm,&st)!=0) look=0;
      iname(nm,"/ext4t/huge/f",BIGDIR_N-1); if(SC2(N_STAT,nm,&st)!=0) look=0;
      check(look,"lookup first/mid/last of large dir");
      int rm=0; for(int i=0;i<BIGDIR_N;i++){ iname(nm,"/ext4t/huge/f",i); if(t_unlink(nm)==0) rm++; }
      checkv(rm==BIGDIR_N,"unlinked all large-dir entries",rm);
      ok(SC1(N_RMDIR,"/ext4t/huge"),"rmdir huge"); }

    // cleanup: leave the root clean so the image stays e2fsck-clean + re-runnable
    t_unlink("/ext4t/hl.txt"); t_unlink("/ext4t/sym"); t_unlink("/ext4t/big.bin"); t_unlink("/ext4t/mt.txt");
    SC1(N_RMDIR,"/ext4t/sub"); SC1(N_RMDIR,"/ext4t");
}

static void test_file_errors(void){
    section("file flags/errors");
    struct Stat st;
    err(SC2(N_STAT,"/tmp/nope_xyz",&st),E_NOENT,"stat missing -> ENOENT");
    err(t_open("/tmp/nope_xyz",O_RDONLY,0),E_NOENT,"open missing RDONLY -> ENOENT");
    const char* P="/tmp/ost_excl";
    i64 h=t_open(P,O_RDWR|O_CREAT|O_EXCL,0644); ok(h,"open O_CREAT|O_EXCL"); if(h>0)t_close(h);
    err(t_open(P,O_RDWR|O_CREAT|O_EXCL,0644),E_EXIST,"O_CREAT|O_EXCL twice -> EEXIST");
    t_unlink(P);
    // access()
    put_file("/tmp/ost_acc","x",1);
    ok(SC2(N_ACCESS,"/tmp/ost_acc",F_OK),"access F_OK existing");
    err(SC2(N_ACCESS,"/tmp/nope_xyz",F_OK),E_NOENT,"access F_OK missing -> ENOENT");
    t_unlink("/tmp/ost_acc");
    // open directory for write -> EISDIR
    err(t_open("/tmp",O_WRONLY,0),E_ISDIR,"open dir O_WRONLY -> EISDIR");
    // invalid access mode (3)
    neg(t_open("/tmp/x",O_RDWR|1,0),"open bad accessmode -> error"); // O_RDWR|O_WRONLY = 3
}

static void test_dirs(void){
    section("directories");
    ok(SC2(N_MKDIR,"/tmp/ost_d",0755),"mkdir");
    err(SC2(N_MKDIR,"/tmp/ost_d",0755),E_EXIST,"mkdir existing -> EEXIST");
    ok(SC2(N_MKDIR,"/tmp/ost_d/sub",0755),"mkdir nested");
    put_file("/tmp/ost_d/f1","aa",2);
    put_file("/tmp/ost_d/f2","bb",2);
    static struct DirEntry ents[32]; memset(ents,0,sizeof ents);
    i64 n=SC3(N_READDIR,"/tmp/ost_d",ents,32); checkv(n>=4,"readdir >=4 entries (. .. sub f1 f2)",n);
    int seen_f1=0,seen_dot=0; for(i64 i=0;i<n&&i<32;i++){ if(sstreq(ents[i].name,"f1"))seen_f1=1; if(sstreq(ents[i].name,"."))seen_dot=1; }
    check(seen_f1,"readdir lists f1");
    check(seen_dot,"readdir lists .");
    err(SC1(N_RMDIR,"/tmp/ost_d"),E_NOTEMPTY,"rmdir non-empty -> ENOTEMPTY");
    ok(t_unlink("/tmp/ost_d/f1"),"unlink f1");
    ok(t_unlink("/tmp/ost_d/f2"),"unlink f2");
    ok(SC1(N_RMDIR,"/tmp/ost_d/sub"),"rmdir sub");
    ok(SC1(N_RMDIR,"/tmp/ost_d"),"rmdir empty");
    err(SC1(N_RMDIR,"/tmp/ost_d"),E_NOENT,"rmdir missing -> ENOENT");
    err(t_unlink("/tmp"),E_ISDIR,"unlink dir -> EISDIR");
}

static void test_links(void){
    section("links (symlink/hardlink)");
    put_file("/tmp/ost_tgt","target-data",11);
    ok(SC2(N_SYMLINK,"/tmp/ost_tgt","/tmp/ost_sym"),"symlink create");
    char lb[64]; memset(lb,0,sizeof lb);
    i64 rl=SC3(N_READLINK,"/tmp/ost_sym",lb,sizeof lb);
    check(rl>0&&contains(lb,rl,"ost_tgt"),"readlink returns target");
    struct Stat st; memset(&st,0,sizeof st); SC2(N_STAT,"/tmp/ost_sym",&st);
    check((st.mode&S_IFMT)==S_IFREG,"stat(symlink) follows to regular");
    struct Stat ls; memset(&ls,0,sizeof ls); SC2(N_LSTAT,"/tmp/ost_sym",&ls);
    check((ls.mode&S_IFMT)==S_IFLNK,"lstat(symlink) is symlink");
    err(SC3(N_READLINK,"/tmp/ost_tgt",lb,sizeof lb),E_INVAL,"readlink on non-symlink -> EINVAL");
    // hard link
    ok(SC2(N_LINK,"/tmp/ost_tgt","/tmp/ost_hl"),"hardlink create");
    memset(&st,0,sizeof st); SC2(N_STAT,"/tmp/ost_tgt",&st); checkv(st.nlink==2,"nlink==2 after link",(i64)st.nlink);
    ok(t_unlink("/tmp/ost_tgt"),"unlink original");
    char rb[16]; memset(rb,0,16); i64 hh=t_open("/tmp/ost_hl",O_RDONLY,0);
    i64 rr=(hh>0)?t_read(hh,rb,16):-1; if(hh>0)t_close(hh);
    check(rr==11&&smemcmp(rb,"target-data",11)==0,"hardlink still has data");
    t_unlink("/tmp/ost_hl"); t_unlink("/tmp/ost_sym");
}

static void test_metadata(void){
    section("metadata (chmod/chown/utime/rename)");
    put_file("/tmp/ost_m","z",1);
    ok(SC3(N_CHMOD,"/tmp/ost_m",0600,0),"chmod 0600");
    struct Stat st; memset(&st,0,sizeof st); SC2(N_STAT,"/tmp/ost_m",&st);
    checkv((st.mode&0777)==0600,"mode is 0600",(i64)(st.mode&0777));
    ok(SC5(N_CHOWN,"/tmp/ost_m",0,123,456,0),"chown 123:456");
    memset(&st,0,sizeof st); SC2(N_STAT,"/tmp/ost_m",&st); checkv(st.uid==123&&st.gid==456,"uid/gid updated",(i64)st.uid);
    ok(SC4(N_UTIME,"/tmp/ost_m",1000000,2000000,0),"utime");
    // rename
    ok(SC2(N_RENAME,"/tmp/ost_m","/tmp/ost_m2"),"rename");
    err(SC2(N_STAT,"/tmp/ost_m",&st),E_NOENT,"old name gone -> ENOENT");
    ok(SC2(N_STAT,"/tmp/ost_m2",&st),"new name exists");
    err(SC2(N_RENAME,"/tmp/ost_nope","/tmp/ost_x"),E_NOENT,"rename missing -> ENOENT");
    t_unlink("/tmp/ost_m2");
}

static void test_devices(void){
    section("devices (/dev)");
    // /dev/null
    i64 hn=t_open("/dev/null",O_RDWR,0); checkv(hn>0,"open /dev/null",hn);
    if(hn>0){ char b[4]; checkv(t_read(hn,b,4)==0,"read /dev/null == EOF(0)",0); checkv(t_write(hn,"abcd",4)==4,"write /dev/null == 4",0); t_close(hn); }
    // /dev/zero
    i64 hz=t_open("/dev/zero",O_RDONLY,0); if(hz>0){ char b[8]; for(int i=0;i<8;i++)b[i]=0x55; i64 r=t_read(hz,b,8); int z=(r==8); for(int i=0;i<8;i++) if(b[i])z=0; check(z,"read /dev/zero gives zeros"); t_close(hz);} else neg(hz,"open /dev/zero");
    // /dev/urandom
    i64 hu=t_open("/dev/urandom",O_RDONLY,0); if(hu>0){ u8 r1[16],r2[16]; memset(r1,0,16);memset(r2,0,16); i64 a=t_read(hu,r1,16),b=t_read(hu,r2,16); check(a==16&&b==16,"read /dev/urandom 16+16"); int nz=0;for(int i=0;i<16;i++)if(r1[i])nz=1; check(nz,"urandom not all-zero"); check(smemcmp(r1,r2,16)!=0,"two urandom reads differ"); t_close(hu);} else neg(hu,"open /dev/urandom");
    // stat char device + non-seekable
    struct Stat st; memset(&st,0,sizeof st); SC2(N_STAT,"/dev/null",&st); check((st.mode&S_IFMT)==S_IFCHR,"/dev/null is char device");
    i64 hz2=t_open("/dev/zero",O_RDONLY,0); if(hz2>0){ err(SC3(N_SEEK,hz2,0,SEEK_SET),E_SPIPE,"seek chardev -> ESPIPE"); t_close(hz2);}
    // readdir /dev
    static struct DirEntry de[32]; memset(de,0,sizeof de); i64 n=SC3(N_READDIR,"/dev",de,32); checkv(n>=5,"readdir /dev >=5",n);
    int null=0,zero=0,ptmx=0; for(i64 i=0;i<n&&i<32;i++){ if(sstreq(de[i].name,"null"))null=1; if(sstreq(de[i].name,"zero"))zero=1; if(sstreq(de[i].name,"ptmx"))ptmx=1;}
    check(null&&zero&&ptmx,"/dev has null,zero,ptmx");
}

static void test_initrd_ro(void){
    section("initrd read-only (/bin,/lib)");
    struct Stat st; memset(&st,0,sizeof st);
    checkv(SC2(N_STAT,"/bin",&st)==0&&(st.mode&S_IFMT)==S_IFDIR,"/bin is a directory",0);
    // /bin/login is us; read ELF magic
    i64 h=t_open("/bin/login",O_RDONLY,0); checkv(h>0,"open /bin/login RO",h);
    if(h>0){ u8 m[4]; i64 r=t_read(h,m,4); check(r==4&&m[0]==0x7f&&m[1]=='E'&&m[2]=='L'&&m[3]=='F',"/bin/login is ELF"); t_close(h);}
    // writes to read-only initrd must fail
    neg(t_open("/bin/ost_new",O_WRONLY|O_CREAT,0644),"create in /bin -> fail (RO)");
    neg(SC2(N_MKDIR,"/bin/ostdir",0755),"mkdir in /bin -> fail (RO)");
    static struct DirEntry de[64]; memset(de,0,sizeof de); i64 n=SC3(N_READDIR,"/bin",de,64); checkv(n>=1,"readdir /bin",n);
}

static void test_etc_seeded(void){
    section("seeded /etc + / root");
    char b[256]; memset(b,0,sizeof b);
    i64 h=t_open("/etc/passwd",O_RDONLY,0); i64 r=(h>0)?t_read(h,b,255):-1; if(h>0)t_close(h);
    check(r>0&&contains(b,r,"root"),"/etc/passwd contains root");
    memset(b,0,sizeof b); h=t_open("/etc/hostname",O_RDONLY,0); r=(h>0)?t_read(h,b,255):-1; if(h>0)t_close(h);
    check(r>0&&contains(b,r,"instantos"),"/etc/hostname is instantos");
    // root fs
    struct Stat st; memset(&st,0,sizeof st);
    checkv(SC2(N_STAT,"/",&st)==0&&(st.mode&S_IFMT)==S_IFDIR,"stat / is directory",0);
    // ext4 seed (present when / is the ext4 test disk)
    memset(b,0,sizeof b); h=t_open("/readme.txt",O_RDONLY,0); r=(h>0)?t_read(h,b,255):-1; if(h>0)t_close(h);
    check(r<=0||contains(b,r,"InstantOS ext4"),"/readme.txt seed (if ext4 root)");
    // write to root (works on ext4/fat writable root)
    if(put_file("/ost_root.tmp","root-write",10)==0){
        memset(b,0,sizeof b); h=t_open("/ost_root.tmp",O_RDONLY,0); r=(h>0)?t_read(h,b,255):-1; if(h>0)t_close(h);
        check(r==10&&contains(b,r,"root-write"),"root fs write+read-back");
        t_unlink("/ost_root.tmp");
    } else { sw("  note: root fs not writable (skipping root write)\n"); }
}

static void test_statfs(void){
    section("statfs");
    struct { u64 bsz,tot,free,tino,fino,nmax; u32 type,rsv; } sf; memset(&sf,0,sizeof sf);
    i64 r=SC3(N_STATFS,"/tmp",0,&sf); checkv(r==0,"statfs /tmp ok",r);
    check(sf.bsz>0,"statfs blockSize > 0");
    memset(&sf,0,sizeof sf); ok(SC3(N_STATFS,"/",0,&sf),"statfs / ok");
}

static void test_pipes(void){
    section("pipes");
    u64 fds[2]={0,0};
    i64 r=SC1(N_PIPE,fds); checkv(r==0,"pipe() ok",r);
    if(r==0){
        i64 rd=(i64)fds[0], wr=(i64)fds[1];
        checkv(t_write(wr,"pipedata",8)==8,"write pipe 8",0);
        struct PollFD pfd; pfd.fd=rd; pfd.events=POLLIN; pfd.revents=0;
        i64 pr=SC3(N_POLL,&pfd,1,100); check(pr>=1&&(pfd.revents&POLLIN),"poll pipe POLLIN");
        char b[16]; memset(b,0,16); i64 rr=t_read(rd,b,16); check(rr==8&&smemcmp(b,"pipedata",8)==0,"read pipe data");
        t_close(wr);
        i64 eof=t_read(rd,b,16); checkv(eof==0,"read after close write == EOF",eof);
        t_close(rd);
    }
    // EPIPE: write to pipe whose read end is closed (SIGPIPE handler installed in main)
    u64 f2[2]={0,0};
    if(SC1(N_PIPE,f2)==0){ t_close((i64)f2[0]); i64 w=t_write((i64)f2[1],"x",1); neg(w,"write to broken pipe -> error(EPIPE)"); t_close((i64)f2[1]); }
}

static void test_dup_fcntl(void){
    section("dup/dup2/fcntl");
    const char* P="/tmp/ost_dup";
    i64 h=t_open(P,O_RDWR|O_CREAT|O_TRUNC,0644); checkv(h>0,"open for dup",h);
    if(h>0){
        i64 d=SC1(N_DUP,h); checkv(d>0,"dup ok",d);
        if(d>0){ t_write(d,"dupdata",7); struct Stat st; memset(&st,0,sizeof st); SC2(N_FSTAT,h,&st); checkv(st.size==7,"write via dup visible on orig",(i64)st.size); t_close(d); }
        i64 fl=SC3(N_FCNTL,h,3,0); ok(fl,"fcntl F_GETFL ok"); // F_GETFL=3
        i64 nd=SC3(N_FCNTL,h,0,10); checkv(nd>0,"fcntl F_DUPFD ok",nd); if(nd>0)t_close(nd); // F_DUPFD=0
        t_close(h);
    }
    t_unlink(P);
}

static void test_pty(void){
    section("PTY (/dev/ptmx, pts, line discipline)");
    i64 m=t_open("/dev/ptmx",O_RDWR,0); checkv(m>0,"open /dev/ptmx",m);
    if(m<=0){ return; }
    u32 ptn=0xffffffff; i64 gi=SC3(N_IOCTL,m,TIOCGPTN,&ptn); checkv(gi==0,"ioctl TIOCGPTN ok",gi);
    checkv(ptn!=0xffffffff,"pty number set",(i64)ptn);
    // build /dev/pts/N
    char path[24]; const char* pre="/dev/pts/"; int i=0; while(pre[i]){path[i]=pre[i];i++;} u32 v=ptn; char d[12]; int k=0; if(v==0)d[k++]='0'; while(v){d[k++]=(char)('0'+v%10);v/=10;} while(k)path[i++]=d[--k]; path[i]=0;
    i64 s=t_open(path,O_RDWR,0); checkv(s>0,"open /dev/pts/N",s);
    // default winsize 24x80
    struct Winsize ws; memset(&ws,0,sizeof ws); SC3(N_IOCTL,m,TIOCGWINSZ,&ws); checkv(ws.row==24&&ws.col==80,"TIOCGWINSZ default 24x80",(i64)ws.row);
    ws.row=40; ws.col=100; ok(SC3(N_IOCTL,m,TIOCSWINSZ,&ws),"TIOCSWINSZ set 40x100");
    memset(&ws,0,sizeof ws); SC3(N_IOCTL,m,TIOCGWINSZ,&ws); checkv(ws.row==40&&ws.col==100,"winsize persisted 40x100",(i64)ws.row);
    if(s>0){
        // raw mode so data passes through untransformed
        struct Termios tio; memset(&tio,0,sizeof tio);
        if(SC3(N_IOCTL,s,TCGETS,&tio)==0){ tio.lflag&=~(0000001|0000002|0000010); tio.iflag&=~0000400; tio.oflag&=~0000001; SC3(N_IOCTL,s,TCSETS,&tio); }
        i64 w=t_write(m,"XY",2); checkv(w==2,"write master 2",w);
        // give line discipline a moment; then non-destructive read of slave
        yield(); msleep(5);
        struct PollFD pfd; pfd.fd=s; pfd.events=POLLIN; pfd.revents=0;
        i64 pr=SC3(N_POLL,&pfd,1,200);
        if(pr>=1&&(pfd.revents&POLLIN)){ char b[8]; memset(b,0,8); i64 rr=t_read(s,b,2); check(rr==2&&b[0]=='X'&&b[1]=='Y',"master->slave data"); }
        else { check(0,"master->slave data (no POLLIN)"); }
        // readdir /dev/pts lists our N
        static struct DirEntry de[8]; memset(de,0,sizeof de); i64 n=SC3(N_READDIR,"/dev/pts",de,8); checkv(n>=1,"readdir /dev/pts >=1",n);
        t_close(s);
    }
    // close master, slave should EOF/HUP — just close cleanly
    t_close(m);
}

static void test_ipc(void){
    section("IPC (queue/service) + shmem + surface");
    i64 q=SC0(N_QUEUECREATE); checkv(q>0,"queue_create",q);
    if(q>0){
        i64 sid=SC2(N_SERVICEREG,"ostest.svc",q); checkv(sid>=0,"service_register",sid);
        i64 conn=SC1(N_SERVICECONN,"ostest.svc"); checkv(conn>0,"service_connect",conn);
        struct IPCMessage msg; memset(&msg,0,sizeof msg); msg.size=4; msg.data[0]='p';msg.data[1]='o';msg.data[2]='n';msg.data[3]='g';
        i64 sn=SC3(N_QUEUESEND,q,&msg,0); checkv(sn==0,"queue_send",sn);
        struct IPCMessage got; memset(&got,0,sizeof got);
        i64 rc=SC3(N_QUEUERECV,q,&got,0); checkv(rc==0,"queue_receive (nonblock)",rc);
        check(got.size==4&&got.data[0]=='p'&&got.data[3]=='g',"ipc message payload matches");
    }
    // shared memory
    i64 sh=SC1(N_SHAREDALLOC,4096); checkv(sh>0,"shared_alloc",sh);
    if(sh>0){ i64 addr=SC1(N_SHAREDMAP,sh); checkv(addr>0,"shared_map",addr); if(addr>0){ volatile u8* p=(volatile u8*)addr; p[0]=0xAB; p[4095]=0xCD; check(p[0]==0xAB&&p[4095]==0xCD,"shared mem read/write"); } ok(SC1(N_SHAREDFREE,sh),"shared_free"); }
    // surface (not name-gated to create)
    i64 surf=SC3(N_SURFCREATE,64,64,0); checkv(surf>0,"surface_create 64x64",surf);
    if(surf>0){ i64 sa=SC1(N_SURFMAP,surf); checkv(sa>0,"surface_map",sa); if(sa>0){ ((volatile u32*)sa)[0]=0x11223344; } i64 cm=SC4(N_SURFCOMMIT,surf,0,0,((u64)64<<32)|64); ok(cm,"surface_commit"); }
}

static void test_sockets(void){
    section("sockets (loopback TCP/UDP + errors)");
    // error paths
    err(SC3(N_SOCKET,AF_UNIX,SOCK_STREAM,0),E_AFNOSUPPORT,"socket(AF_UNIX) -> EAFNOSUPPORT");
    err(SC3(N_SOCKET,AF_INET,SOCK_RAW,0),E_OPNOTSUPP,"socket(SOCK_RAW) -> EOPNOTSUPP");
    // UDP loopback
    i64 A=SC3(N_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0); checkv(A>0,"udp socket A",A);
    i64 B=SC3(N_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0); checkv(B>0,"udp socket B",B);
    if(A>0&&B>0){
        struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons(20001); sa.sin_addr=0x0100007f;
        ok(SC3(N_BIND,A,&sa,sizeof sa),"udp bind A 127.0.0.1:20001");
        struct sockaddr_in sb; memset(&sb,0,sizeof sb); sb.sin_family=AF_INET; sb.sin_port=htons(20002); sb.sin_addr=0x0100007f;
        ok(SC3(N_BIND,B,&sb,sizeof sb),"udp bind B 127.0.0.1:20002");
        ok(SC3(N_CONNECT,B,&sa,sizeof sa),"udp connect B->A");
        i64 sn=SC4(N_SEND,B,"ping",4,0); checkv(sn==4,"udp send 4",sn);
        char rb[16]; memset(rb,0,16); i64 rr=-E_AGAIN;
        for(int i=0;i<50&&rr==-E_AGAIN;i++){ rr=SC4(N_RECV,A,rb,16,0); if(rr==-E_AGAIN){ yield(); msleep(2);} }
        check(rr==4&&smemcmp(rb,"ping",4)==0,"udp recv 'ping' on A");
        // getsockname A -> port 20001
        struct sockaddr_in gn; memset(&gn,0,sizeof gn); u32 gl=sizeof gn;
        i64 gr=SC3(N_GETSOCKNAME,A,&gn,&gl); check(gr==0&&gn.sin_port==htons(20001),"getsockname port==20001");
        // getpeername on unconnected A -> ENOTCONN
        struct sockaddr_in pn; memset(&pn,0,sizeof pn); u32 pl=sizeof pn;
        neg(SC3(N_GETPEERNAME,A,&pn,&pl),"getpeername(unconnected) -> error");
        t_close(A); t_close(B);
    }
    // TCP loopback
    i64 L=SC3(N_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0); checkv(L>0,"tcp listen socket",L);
    if(L>0){
        int one=1; SC5(N_SETSOCKOPT,L,SOL_SOCKET,SO_REUSEADDR,&one,4);
        struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons(20010); sa.sin_addr=0x0100007f;
        ok(SC3(N_BIND,L,&sa,sizeof sa),"tcp bind 127.0.0.1:20010");
        ok(SC2(N_LISTEN,L,4),"tcp listen");
        i64 C=SC3(N_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0); checkv(C>0,"tcp client socket",C);
        i64 cr=SC3(N_CONNECT,C,&sa,sizeof sa); check(cr==0||cr==-E_AGAIN,"tcp connect (loopback)");
        i64 Aa=-E_AGAIN; for(int i=0;i<80&&Aa<0;i++){ Aa=SC3(N_ACCEPT,L,0,0); if(Aa<0){ SC0(N_YIELD); msleep(2);} }
        checkv(Aa>0,"tcp accept",Aa);
        if(Aa>0){
            i64 sn=SC4(N_SEND,C,"hello",5,0); checkv(sn==5,"tcp send 5",sn);
            char rb[16]; memset(rb,0,16); i64 rr=-E_AGAIN;
            for(int i=0;i<80&&rr==-E_AGAIN;i++){ rr=SC4(N_RECV,Aa,rb,16,0); if(rr==-E_AGAIN){SC0(N_YIELD);msleep(2);} }
            check(rr==5&&smemcmp(rb,"hello",5)==0,"tcp recv 'hello'");
            // SO_TYPE
            int ty=0; u32 tl=4; i64 go=SC5(N_GETSOCKOPT,Aa,SOL_SOCKET,SO_TYPE,&ty,&tl); check(go==0&&ty==SOCK_STREAM,"getsockopt SO_TYPE==STREAM");
            SC2(N_SHUTDOWN,Aa,SHUT_RDWR); t_close(Aa);
        }
        t_close(C); t_close(L);
    }
    // A blocking recv must notice the peer going away: by shutdown(SHUT_WR)
    // (mode 0) or by its last handle closing (mode 1).
    for(int mode=0;mode<2;mode++){
        i64 L2=SC3(N_SOCKET,AF_INET,SOCK_STREAM,0);
        struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons((u16)(20020+mode)); sa.sin_addr=0x0100007f;
        SC3(N_BIND,L2,&sa,sizeof sa); SC2(N_LISTEN,L2,4);
        i64 C2=SC3(N_SOCKET,AF_INET,SOCK_STREAM,0);
        i64 cr=SC3(N_CONNECT,C2,&sa,sizeof sa);
        i64 A2=SC3(N_ACCEPT,L2,0,0);
        if(cr!=0||A2<=0){ checkv(0,mode?"blocking recv wakes on peer close (setup)":"blocking recv wakes on peer shutdown (setup)",A2); continue; }
        i64 pid=SC0(N_FORK);
        if(pid==0){
            if(mode==1) t_close(C2);   // the parent's handle is then the last one
            struct { i64 s,us; } tv={5,0}; SC5(N_SETSOCKOPT,A2,SOL_SOCKET,20/*SO_RCVTIMEO*/,&tv,sizeof tv);
            char b[8]; i64 r=SC4(N_RECV,A2,b,sizeof b,0);
            SC1(N_EXIT,r==0?0:(r==-E_AGAIN?1:2)); for(;;){}
        }
        msleep(100);
        if(mode==0) SC2(N_SHUTDOWN,C2,1/*SHUT_WR*/); else t_close(C2);
        int st=-1; if(pid>0) SC3(N_WAIT,pid,&st,0);
        int code=(st>>8)&0xff; if(st>=0&&st<256) code=st;
        checkv(pid>0&&code==0,mode?"blocking recv wakes with EOF on peer close (1=timed out)":"blocking recv wakes with EOF on peer shutdown (1=timed out)",code);
        if(mode==0) t_close(C2);
        t_close(A2); t_close(L2);
    }
    // NIC presence syscalls (deterministic either way)
    i64 link=SC0(N_NETLINK); checkv(link==0||link==1,"net_link_status 0/1",link);
    u8 mac[6]; memset(mac,0,6); i64 mr=SC1(N_NETGETMAC,mac);
    check(mr==0||mr<0,"net_get_mac returns"); if(mr==0){ int nz=0; for(int i=0;i<6;i++) if(mac[i])nz=1; check(nz,"MAC not all-zero (NIC present)"); }
}

static void test_gated_negative(void){
    section("permission gating (expected failures)");
    // only /bin/graphics-compositor gets the framebuffer mapped; we are
    // /bin/login, so fbinfo gives the geometry with addr 0
    struct { u64 addr; u32 width,height,pitch,format; u64 size; u32 r,g,b; } fb;
    memset(&fb,0xff,sizeof fb);
    i64 fr=SC1(N_FBINFO,&fb);
    check(fr==0&&fb.addr==0&&fb.width>=320&&fb.height>=200,"fbinfo: geometry only, nothing mapped (not compositor)");
    neg(SC0(N_FBMAP),"fbmap gated -> fail (not compositor)");
    // kill(self, 0) probe succeeds; kill(bogus pid, 0) -> ENOENT
    ok(SC2(N_KILL,0,0),"kill(self,0) probe ok");
    err(SC2(N_KILL,999999,0),E_NOENT,"kill(bogus,0) -> ENOENT");
}

static void test_storage(void){
    section("storage info");
    struct StorageInfoT { u64 totalSize; u32 sectorSize; u32 flags; i32 mountError; u32 rsv; char dev[32]; char fs[16]; char mp[64]; } si;
    memset(&si,0,sizeof si);
    i64 r=SC1(N_STORAGEINFO,&si); checkv(r==0,"storage_info ok",r);
    check(si.totalSize>0,"storage totalSize > 0");
    check(si.sectorSize>0,"storage sectorSize > 0");
}

// ---- process / signals / threads (riskiest -> last) ----
static void test_process(void){
    section("process (fork/spawn/wait)");
    ok(SC0(N_YIELD),"yield ok");
    i64 pid=SC0(N_FORK);
    if(pid==0){ SC1(N_EXIT,77); for(;;){} }   // child: exit 77 immediately
    checkv(pid>0,"fork returns child pid",pid);
    if(pid>0){ int status=0; i64 w=SC3(N_WAIT,pid,&status,0); checkv(w==pid,"wait reaps child",w); checkv(status==77||((status>>8)&0xff)==77,"child exit code 77",status); }
    // spawn helper (exits 42)
    static const char* argv[]={"ostest-helper",0};
    static const char* envp[]={"PATH=/bin",0};
    i64 sp=SC3(N_SPAWN,"/bin/ostest-helper",argv,envp); checkv(sp>0,"spawn helper",sp);
    if(sp>0){ int st=0; i64 w=SC3(N_WAIT,sp,&st,0); checkv(w==sp,"wait spawned",w); checkv(st==42||((st>>8)&0xff)==42,"helper exit 42",st); }
    neg(SC3(N_SPAWN,"/bin/does-not-exist",argv,envp),"spawn missing -> error");
}

// ---- exec/spawn features (P1.2): argv/env, shebang, deep paths, cloexec/pipe2 ----
static void spawn_wait(const char* path, const char** av, const char** ev, const char* name, int wantCode){
    i64 sp=SC3(N_SPAWN,path,av,ev);
    if(sp<=0){ checkv(0,name,sp); return; }
    int st=0; i64 w=SC3(N_WAIT,sp,&st,0);
    checkv(w==sp && ((st>>8)&0xff)==wantCode, name, ((st>>8)&0xff));
}

static void test_exec_spawn(void){
    section("exec/spawn (argv/env/shebang/deep/pipe2)");
    static const char* env0[]={"PATH=/bin",0};

    // argv count survives spawn (helper "argc" exits argc).
    { static const char* av[]={"ostest-helper","argc","a","b",0};
      spawn_wait("/bin/ostest-helper",av,env0,"spawn argv count==4",4); }

    // large argv (>64, now up to 128): 100 entries -> exit 100.
    { static const char* big[128]; big[0]="ostest-helper"; big[1]="argc";
      for(int i=2;i<100;i++) big[i]="x"; big[100]=0;
      spawn_wait("/bin/ostest-helper",big,env0,"spawn large argv (100)",100); }

    // long argument (200 bytes) survives (helper "arglen" exits strlen(last arg)).
    { static char la[256]; for(int i=0;i<200;i++) la[i]='A'; la[200]=0;
      static const char* av[]={"ostest-helper","arglen",0,0}; av[2]=la;
      spawn_wait("/bin/ostest-helper",av,env0,"spawn long arg len==200",200); }

    // envp passing (helper "env" exits int value of OSTEST_CODE).
    { static const char* av[]={"ostest-helper","env",0};
      static const char* ev[]={"OSTEST_CODE=99","PATH=/bin",0};
      spawn_wait("/bin/ostest-helper",av,ev,"spawn envp OSTEST_CODE==99",99); }

    // shebang: interpreter is the helper -> helper runs (default exit 42).
    put_file("/tmp/sheb.sh","#!/bin/ostest-helper\n",slen("#!/bin/ostest-helper\n"));
    { static const char* av[]={"/tmp/sheb.sh",0};
      spawn_wait("/tmp/sheb.sh",av,env0,"shebang runs interpreter (42)",42); }

    // shebang argv layout: "#!/bin/ostest-helper argc" -> argv=[interp,argc,script] -> argc==3.
    put_file("/tmp/sheb2.sh","#!/bin/ostest-helper argc\n",slen("#!/bin/ostest-helper argc\n"));
    { static const char* av[]={"/tmp/sheb2.sh",0};
      spawn_wait("/tmp/sheb2.sh",av,env0,"shebang optarg+script argv (argc==3)",3); }

    // shebang with a missing interpreter -> spawn fails (no crash).
    put_file("/tmp/shebbad.sh","#!/bin/nope-xyz\n",slen("#!/bin/nope-xyz\n"));
    { static const char* av[]={"/tmp/shebbad.sh",0};
      neg(SC3(N_SPAWN,"/tmp/shebbad.sh",av,env0),"shebang missing interp -> error"); }
    t_unlink("/tmp/sheb.sh"); t_unlink("/tmp/sheb2.sh"); t_unlink("/tmp/shebbad.sh");

    // deep nested path: spawn a shebang script several directories deep.
    SC2(N_MKDIR,"/tmp/dp",0755); SC2(N_MKDIR,"/tmp/dp/a",0755); SC2(N_MKDIR,"/tmp/dp/a/b",0755);
    SC2(N_MKDIR,"/tmp/dp/a/b/c",0755); SC2(N_MKDIR,"/tmp/dp/a/b/c/d",0755); SC2(N_MKDIR,"/tmp/dp/a/b/c/d/e",0755);
    put_file("/tmp/dp/a/b/c/d/e/deep.sh","#!/bin/ostest-helper\n",slen("#!/bin/ostest-helper\n"));
    { static const char* av[]={"deep.sh",0};
      spawn_wait("/tmp/dp/a/b/c/d/e/deep.sh",av,env0,"spawn deep-path script (42)",42); }
    t_unlink("/tmp/dp/a/b/c/d/e/deep.sh");

    // over-long path -> graceful error (no crash).
    { static char lp[400]; lp[0]='/'; for(int i=1;i<390;i++) lp[i]='a'; lp[390]=0;
      static const char* av[]={"x",0}; neg(SC3(N_SPAWN,lp,av,env0),"spawn over-long path -> error"); }

    // O_CLOEXEC via open + fcntl(F_GETFD/F_SETFD).
    { i64 h=t_open("/tmp/ost_clo",O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0644); checkv(h>0,"open O_CLOEXEC",h);
      if(h>0){ checkv(SC3(N_FCNTL,h,F_GETFD,0)==FD_CLOEXEC,"F_GETFD == FD_CLOEXEC",SC3(N_FCNTL,h,F_GETFD,0));
               ok(SC3(N_FCNTL,h,F_SETFD,0),"F_SETFD clear cloexec");
               checkv(SC3(N_FCNTL,h,F_GETFD,0)==0,"FD_CLOEXEC cleared",0); t_close(h); }
      t_unlink("/tmp/ost_clo"); }
    { i64 h=t_open("/tmp/ost_clo2",O_RDWR|O_CREAT|O_TRUNC,0644);
      if(h>0){ checkv(SC3(N_FCNTL,h,F_GETFD,0)==0,"open w/o O_CLOEXEC -> FD_CLOEXEC clear",0); t_close(h); }
      t_unlink("/tmp/ost_clo2"); }

    // pipe2: basic, O_CLOEXEC, O_NONBLOCK, and unsupported-flag rejection.
    { u64 fds[2]={0,0}; i64 r=SC2(N_PIPE2,fds,0); checkv(r==0,"pipe2(0) ok",r);
      if(r==0){ checkv(t_write((i64)fds[1],"pp",2)==2,"pipe2 write 2",0);
                char b[4]; memset(b,0,4); i64 rr=t_read((i64)fds[0],b,4);
                check(rr==2&&b[0]=='p'&&b[1]=='p',"pipe2 read back");
                t_close((i64)fds[0]); t_close((i64)fds[1]); } }
    { u64 fds[2]={0,0}; i64 r=SC2(N_PIPE2,fds,O_CLOEXEC); checkv(r==0,"pipe2(O_CLOEXEC) ok",r);
      if(r==0){ checkv(SC3(N_FCNTL,(i64)fds[0],F_GETFD,0)==FD_CLOEXEC,"pipe2 read end FD_CLOEXEC",0);
                checkv(SC3(N_FCNTL,(i64)fds[1],F_GETFD,0)==FD_CLOEXEC,"pipe2 write end FD_CLOEXEC",0);
                t_close((i64)fds[0]); t_close((i64)fds[1]); } }
    { u64 fds[2]={0,0}; i64 r=SC2(N_PIPE2,fds,O_NONBLOCK); checkv(r==0,"pipe2(O_NONBLOCK) ok",r);
      if(r==0){ char b[4]; err(t_read((i64)fds[0],b,4),E_AGAIN,"pipe2 O_NONBLOCK read empty -> EAGAIN");
                t_close((i64)fds[0]); t_close((i64)fds[1]); } }
    { u64 fds[2]={0,0}; err(SC2(N_PIPE2,fds,O_APPEND),E_INVAL,"pipe2 unsupported flag -> EINVAL"); }
}

#define SA_RESTORER 0x04000000u
static volatile int g_sig_hit=0;
static void sigusr1_handler(int s){ (void)s; g_sig_hit=1; }
// Restorer trampoline: when a handler returns, control lands here with RSP at the
// saved signal frame; invoking sigreturn (syscall 21) restores RSP/regs/RIP. Raw
// signal() leaves no restorer (kernel returns to the interrupted RIP without
// restoring RSP -> crash), so handlers we actually let RETURN must set this.
__attribute__((naked)) static void sig_restorer(void){ asm volatile("mov $21,%eax\n\tsyscall\n\t"); }

static void test_signals(void){
    section("signals");
    struct SigAct { u64 handler,mask,flags,restorer; } act, old;
    memset(&act,0,sizeof act); memset(&old,0,sizeof old);
    act.handler=(u64)sigusr1_handler; act.flags=SA_RESTORER; act.restorer=(u64)sig_restorer;
    ok(SC3(N_SIGACTION,SIG_USR1,&act,&old),"sigaction(SIGUSR1) install");
    g_sig_hit=0;
    SC2(N_KILL,0,SIG_USR1);   // send to self (0 = self)
    for(int i=0;i<50&&!g_sig_hit;i++){ yield(); msleep(2); }
    check(g_sig_hit==1,"SIGUSR1 handler delivered + returned");
    // sigprocmask block/unblock (pure syscall behavior)
    u64 set=(1ull<<SIG_USR1); u64 oldm=0;
    ok(SC3(N_SIGPROCMASK,0,&set,&oldm),"sigprocmask BLOCK");   // how=0 BLOCK
    ok(SC3(N_SIGPROCMASK,1,&set,&oldm),"sigprocmask UNBLOCK"); // how=1 UNBLOCK
    // signal() install returns the previous handler (no delivery afterwards)
    i64 pr=SC2(N_SIGNAL,SIG_USR1,(i64)sigusr1_handler); ok(pr,"signal() install returns");
}

// Network step 5: sendto/recvfrom with addresses, ICMP ping sockets, the
// nanosecond clock and ITIMER_REAL.
enum { N_SENDTO=143, N_RECVFROM=144, N_GETTIMENS=145, N_SETITIMER=146, N_GETITIMER=147, SIG_ALRM=14 };
static volatile int g_alarms=0;
static void sigalrm_handler(int s){ (void)s; g_alarms++; }

static void test_net_p5(void){
    section("sendto/recvfrom, ping sockets, timers");
    i64 A=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0), B=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,0);
    if(A>0&&B>0){
        struct { i64 s,us; } tv={2,0};
        SC5(N_SETSOCKOPT,A,SOL_SOCKET,20,&tv,sizeof tv); SC5(N_SETSOCKOPT,B,SOL_SOCKET,20,&tv,sizeof tv);
        struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_port=htons(20031); sa.sin_addr=0x0100007f;
        struct sockaddr_in sb=sa; sb.sin_port=htons(20032);
        SC3(N_BIND,A,&sa,sizeof sa); SC3(N_BIND,B,&sb,sizeof sb);
        i64 n=SC5(N_SENDTO,B,"abc",3,0,&sa); checkv(n==3,"sendto on an unconnected UDP socket",n);
        char buf[16]; struct sockaddr_in from; memset(&from,0,sizeof from);
        n=SC5(N_RECVFROM,A,buf,sizeof buf,0,&from);
        check(n==3&&smemcmp(buf,"abc",3)==0,"recvfrom gets the datagram");
        check(from.sin_family==AF_INET&&from.sin_port==htons(20032)&&from.sin_addr==0x0100007f,"recvfrom reports the sender 127.0.0.1:20032");
        n=SC5(N_SENDTO,A,"xyz",3,0,&from);
        memset(&from,0,sizeof from);
        i64 m=SC5(N_RECVFROM,B,buf,sizeof buf,0,&from);
        check(n==3&&m==3&&smemcmp(buf,"xyz",3)==0&&from.sin_port==htons(20031),"reply to the reported address arrives");
        struct sockaddr_in pn; u32 pl=sizeof pn;
        neg(SC3(N_GETPEERNAME,B,&pn,&pl),"sendto does not connect the socket");
    }
    if(A>0) t_close(A);
    if(B>0) t_close(B);

    i64 P=SC3(N_SOCKET,AF_INET,SOCK_DGRAM,1/*IPPROTO_ICMP*/);
    checkv(P>0,"socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)",P);
    if(P>0){
        struct { i64 s,us; } tv={2,0}; SC5(N_SETSOCKOPT,P,SOL_SOCKET,20,&tv,sizeof tv);
        struct sockaddr_in lo; memset(&lo,0,sizeof lo); lo.sin_family=AF_INET; lo.sin_addr=0x0100007f;
        u8 req[24]={8,0,0,0, 0,0, 0,7}; for(int i=8;i<24;i++) req[i]=(u8)i;
        i64 n=SC5(N_SENDTO,P,req,sizeof req,0,&lo); checkv(n==24,"echo request to 127.0.0.1",n);
        u8 rep[64]; struct sockaddr_in from; memset(&from,0,sizeof from);
        i64 m=SC5(N_RECVFROM,P,rep,sizeof rep,0,&from);
        check(m==24&&rep[0]==0&&rep[7]==7&&smemcmp(rep+8,req+8,16)==0,"echo reply with the same sequence and data");
        check(from.sin_addr==0x0100007f,"... from 127.0.0.1");
        u32 sum=0; for(int i=0;i<24;i+=2) sum+=(u32)((rep[i]<<8)|rep[i+1]); while(sum>>16) sum=(sum&0xffff)+(sum>>16);
        check(m==24&&sum==0xffff,"... with a valid checksum");
        u8 bad[8]={13,0,0,0,0,0,0,1};
        err(SC5(N_SENDTO,P,bad,8,0,&lo),E_INVAL,"a ping socket sends echo requests only");
        t_close(P);
    }

    i64 ns1=SC0(N_GETTIMENS), ms=SC0(N_GETTIME), ns2=SC0(N_GETTIMENS);
    check(ns1>0&&ns2>=ns1,"GetTimeNs is monotonic");
    i64 diff=ns1/1000000-ms; if(diff<0) diff=-diff;
    checkv(diff<=20,"GetTimeNs agrees with the ms clock (ms apart)",diff);

    struct SigAct { u64 handler,mask,flags,restorer; } act, old;
    memset(&act,0,sizeof act); act.handler=(u64)sigalrm_handler; act.flags=SA_RESTORER; act.restorer=(u64)sig_restorer;
    ok(SC3(N_SIGACTION,SIG_ALRM,&act,&old),"sigaction(SIGALRM)");
    struct Itv { i64 is,ius,vs,vus; } it={0,0,0,50000}, cur;
    g_alarms=0; i64 t0=SC0(N_GETTIME);
    ok(SC3(N_SETITIMER,0,&it,0),"setitimer(ITIMER_REAL, 50 ms)");
    memset(&cur,0,sizeof cur); SC2(N_GETITIMER,0,&cur);
    check(cur.vs==0&&cur.vus>0&&cur.vus<=50000,"getitimer shows it running");
    while(!g_alarms&&SC0(N_GETTIME)-t0<2000) msleep(5);
    i64 took=SC0(N_GETTIME)-t0;
    checkv(g_alarms==1&&took>=40&&took<1000,"SIGALRM after ~50 ms (ms)",took);
    memset(&cur,0,sizeof cur); SC2(N_GETITIMER,0,&cur);
    check(cur.vs==0&&cur.vus==0,"a one-shot timer is off afterwards");
    struct Itv rep={0,20000,0,20000}; g_alarms=0; t0=SC0(N_GETTIME);
    SC3(N_SETITIMER,0,&rep,0);
    while(g_alarms<3&&SC0(N_GETTIME)-t0<2000) msleep(5);
    struct Itv off={0,0,0,0}; SC3(N_SETITIMER,0,&off,0);
    checkv(g_alarms>=3,"an interval timer repeats",g_alarms);
    int seen=g_alarms; msleep(100);
    check(g_alarms==seen,"setitimer(0) stops it");
    SC3(N_SIGACTION,SIG_ALRM,&old,0);
}

static void test_wallclock(void){
    section("wall clock (GetUnixTimeNs/SetUnixTime)");
    i64 s0=SC0(N_GETUNIXTIME), ns=SC0(N_GETUNIXTIMENS), s1=SC0(N_GETUNIXTIME);
    checkv(ns/1000000000>=s0&&ns/1000000000<=s1,"GetUnixTimeNs matches GetUnixTime",ns);
    checkv(s0>1700000000,"the RTC gives a date after 2023",s0);
    i64 a=SC0(N_GETUNIXTIMENS); msleep(20); i64 b=SC0(N_GETUNIXTIMENS);
    checkv(b-a>=15000000&&b-a<2000000000,"the wall clock advances with sleep (ns)",b-a);
    ok(SC1(N_SETUNIXTIME,ns+3600LL*1000000000LL),"SetUnixTime one hour ahead");
    i64 moved=SC0(N_GETUNIXTIME)-s1;
    checkv(moved>=3599&&moved<=3602,"the clock moved by an hour (s)",moved);
    ok(SC1(N_SETUNIXTIME,SC0(N_GETUNIXTIMENS)-3600LL*1000000000LL),"SetUnixTime back");
    moved=SC0(N_GETUNIXTIME)-s1;
    checkv(moved>=-1&&moved<=2,"the clock is back (s)",moved);
    err(SC1(N_SETUNIXTIME,5),22,"SetUnixTime before boot: EINVAL");
    err(SC1(N_SETUNIXTIME,-1),22,"SetUnixTime negative: EINVAL");
}

static volatile u32 g_thread_val=0;
static void thread_fn(void* arg){ (void)arg; g_thread_val=0xABCD; SC1(N_THREADEXIT,7); for(;;){} }

static void test_threads(void){
    section("threads");
    g_thread_val=0;
    i64 th=SC3(N_THREADCREATE,(i64)thread_fn,0,0); checkv(th>0,"thread_create returns handle",th);
    if(th>0){
        for(int i=0;i<200&&g_thread_val!=0xABCD;i++){ yield(); msleep(2); }
        check(g_thread_val==0xABCD,"thread ran and set value");
        // NOTE: thread_join intentionally omitted — a faulting thread never
        // reaches 'done', so join would spin forever.
    }
}

// ---- process lifecycle stress (churn / wait-race / kill-storm / orphan) ----
// Regression coverage for: sys_wait lost-wakeup (block-then-recheck ordering),
// signal kills notifying + auto-reaping via onProcessTerminated, orphan
// reparent+auto-reap, and WNOHANG/WUNTRACED/WCONTINUED option handling. The
// total process count from procinfo() is the process-leak oracle.
static u64 proc_count(void){ u64 t=0; SC3(N_PROCINFO,0,0,&t); return t; }

// User GS base must survive every way the kernel is entered and left: syscalls,
// blocking (context switch), timer preemption (IRQ entry/exit swapgs), and fork.
// The kernel keeps its per-CPU pointer in GS, so any missing swapgs shows here.
static int cpu_has_fsgsbase(void){
    u32 a=7,b=0,c=0,d=0;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"+c"(c),"=d"(d));
    return (b&1)!=0;
}
static u64 rd_gsbase(void){ u64 v; __asm__ volatile("rdgsbase %0":"=r"(v)); return v; }
static void wr_gsbase(u64 v){ __asm__ volatile("wrgsbase %0"::"r"(v)); }
static void test_gs_base(void){
    section("user GS base");
    if(!cpu_has_fsgsbase()){ sw("  (skipped: CPU has no FSGSBASE)\n"); return; }
    const u64 magic=0x00005a5a12345000ULL;
    wr_gsbase(magic);

    for(int i=0;i<64;i++) SC0(N_GETPID);
    checkv(rd_gsbase()==magic,"GS base kept across syscalls",(i64)rd_gsbase());

    msleep(20); yield();
    checkv(rd_gsbase()==magic,"GS base kept across sleep/yield",(i64)rd_gsbase());

    // Spin long enough for several timer ticks to preempt us from user mode.
    for(volatile u64 i=0;i<30000000ULL;i++){}
    checkv(rd_gsbase()==magic,"GS base kept across preemption",(i64)rd_gsbase());

    i64 pid=SC0(N_FORK);
    if(pid==0){ SC1(N_EXIT,rd_gsbase()==magic?0:1); for(;;){} }
    int st=0; i64 w=(pid>0)?SC3(N_WAIT,pid,&st,0):pid;
    checkv(w==pid && ((st>>8)&0xff)==0,"fork child inherits GS base",st);
    checkv(rd_gsbase()==magic,"GS base kept in parent after fork",(i64)rd_gsbase());

    wr_gsbase(0);
}

// TLB shootdown (SMP step 5): a sibling thread keeps writing a page from
// another CPU while this thread changes the mapping under it. Without a
// shootdown its CPU keeps the stale writable translation:
//  - fork: the page turns copy-on-write, but the thread's writes would still
//    land in the frame the child now shares, so the child's snapshot moves;
//  - munmap: the thread would keep writing a page that no longer exists
//    instead of faulting (it dies, which is the expected outcome here).
// On one CPU the thread only runs when this one is not, so both pass trivially.
static volatile u64* g_tlb_page;
static volatile u64 g_tlb_iters;
static volatile int g_tlb_stop;
static volatile int g_tlb_done;
static void tlb_writer(void* arg){
    (void)arg;
    u64 n=0;
    while(!g_tlb_stop){ g_tlb_page[0]=++n; g_tlb_iters=n; }
    g_tlb_done=1;
    SC1(N_THREADEXIT,0); for(;;){}
}
static int tlb_wait_progress(void){
    u64 start=g_tlb_iters;
    for(int i=0;i<500;i++){ if(g_tlb_iters-start>1000) return 1; yield(); msleep(1); }
    return 0;
}
static void test_tlb_shootdown(void){
    section("TLB shootdown");

    // 1) fork while a sibling thread writes a now-copy-on-write page.
    g_tlb_page=(volatile u64*)SC3(N_MMAP,0,4096,PROT_R|PROT_W);
    checkv((i64)g_tlb_page>0,"mmap page for the writer thread",(i64)g_tlb_page);
    if((i64)g_tlb_page<=0) return;
    g_tlb_page[0]=0; g_tlb_iters=0; g_tlb_stop=0; g_tlb_done=0;
    i64 th=SC3(N_THREADCREATE,(i64)tlb_writer,0,0); checkv(th>0,"writer thread created",th);
    if(th<=0) return;
    check(tlb_wait_progress(),"writer thread runs");
    {   // Informational: on one CPU this only shows timer preemption.
        u64 a=g_tlb_iters; for(volatile u64 i=0;i<2000000ULL;i++){}
        sw(g_tlb_iters!=a ? "  (writer made progress while we spun)\n" : "  (writer idle while we spun)\n");
    }
    // Several rounds: the stale window only shows if the writer's CPU does not
    // happen to switch address spaces (e.g. to run the child) meanwhile.
    int forks_ok=1, st=0;
    for(int round=0;round<4&&forks_ok;round++){
        i64 pid=SC0(N_FORK);
        if(pid==0){
            // Our copy must not move, however long the parent's thread writes.
            u64 v=g_tlb_page[0]; int stable=1;
            for(int r=0;r<20&&stable;r++){
                for(volatile u64 i=0;i<200000ULL;i++){ if(g_tlb_page[0]!=v){ stable=0; break; } }
                msleep(1);
            }
            SC1(N_EXIT,stable?0:1); for(;;){}
        }
        i64 w=(pid>0)?SC3(N_WAIT,pid,&st,0):pid;
        if(w!=pid || ((st>>8)&0xff)!=0) forks_ok=0;
        if(!tlb_wait_progress()) forks_ok=0;
    }
    checkv(forks_ok,"4x fork: child's copy-on-write page is not written by the parent's thread",st);
    check(tlb_wait_progress(),"writer keeps running after the copy-on-write breaks");

    g_tlb_stop=1;
    for(int i=0;i<500&&!g_tlb_done;i++) msleep(1);

    // 2) munmap under the writer: once munmap returns it must not complete
    //    another store to the page (at most the one in flight to g_tlb_iters).
    //    The writer's fault kills its whole process, so this runs in a child:
    //    139 (SIGSEGV) = it faulted at once, 11 = it kept writing, 10 = no fault.
    sw("  (the child's writer thread is expected to crash on the unmapped page)\n");
    i64 pid=SC0(N_FORK);
    if(pid==0){
        g_tlb_page=(volatile u64*)SC3(N_MMAP,0,4096,PROT_R|PROT_W);
        if((i64)g_tlb_page<=0) SC1(N_EXIT,12);
        g_tlb_iters=0; g_tlb_stop=0; g_tlb_done=0;
        if(SC3(N_THREADCREATE,(i64)tlb_writer,0,0)<=0 || !tlb_wait_progress()) SC1(N_EXIT,13);
        if(SC2(N_MUNMAP,(i64)g_tlb_page,4096)!=0) SC1(N_EXIT,14);
        u64 a=g_tlb_iters;
        for(volatile u64 i=0;i<3000000ULL;i++){}
        if(g_tlb_iters-a>1) SC1(N_EXIT,11);
        msleep(20);   // on one CPU the writer only runs now
        SC1(N_EXIT,10); for(;;){}
    }
    st=0; i64 w=(pid>0)?SC3(N_WAIT,pid,&st,0):pid;
    i64 code=(w==pid)?((st>>8)&0xff):-1;
    checkv(code==139||code==10,"writer stops as soon as its page is unmapped",code);
    checkv(code==139,"writer died on the fault, taking its process with it",code);
}

// ---- SMP step 6: ext4 under the storage lock ----
// Disk-backed filesystem operations run under the storage lock without the big
// kernel lock, so other CPUs keep running kernel code while one waits for the
// disk. Forked workers create, verify and delete files in parallel, in their
// own directories and in one shared directory, and a sibling thread closes an
// fd while another thread is reading from it.
enum { PAR_WORKERS=3, PAR_FILES=16, PAR_SIZE=1500 };
static void par_pattern(u8* b,int k,int i){ for(int j=0;j<PAR_SIZE;j++) b[j]=(u8)(k*31+i*7+j); }
static int par_worker(int k){
    static u8 wb[PAR_SIZE], rb[PAR_SIZE];
    char dir[32], pfx[48], nm[64];
    iname(dir,"/ext4p/w",k);
    if(SC2(N_MKDIR,dir,0755)!=0) return 1;
    int i=0; while(dir[i]){pfx[i]=dir[i];i++;} pfx[i++]='/'; pfx[i++]='f'; pfx[i]='\0';
    char spfx[48]; iname(spfx,"/ext4p/shared/w",k); i=0; while(spfx[i]) i++; spfx[i++]='_'; spfx[i]='\0';
    for(int f=0;f<PAR_FILES;f++){
        par_pattern(wb,k,f);
        iname(nm,pfx,f); if(put_file(nm,(const char*)wb,PAR_SIZE)!=0) return 2;
        iname(nm,spfx,f); if(put_file(nm,(const char*)wb,PAR_SIZE)!=0) return 3;
    }
    for(int f=0;f<PAR_FILES;f++){
        par_pattern(wb,k,f); memset(rb,0,PAR_SIZE);
        iname(nm,pfx,f); i64 h=t_open(nm,O_RDONLY,0); if(h<0) return 4;
        i64 r=t_read(h,rb,PAR_SIZE); t_close(h);
        if(r!=PAR_SIZE || smemcmp(rb,wb,PAR_SIZE)!=0) return 5;
        if(t_unlink(nm)!=0) return 6;
    }
    if(SC1(N_RMDIR,dir)!=0) return 7;
    return 0;
}
static volatile i64 g_race_fd;
static volatile u64 g_race_reads;
static volatile i64 g_race_result;
static volatile int g_race_done;
static void race_reader(void* arg){
    (void)arg;
    static u8 b[4096];   // several 512-byte kernel chunks per read
    for(;;){
        i64 r=t_read(g_race_fd,b,sizeof b);
        if(r==0){ SC3(N_SEEK,g_race_fd,0,SEEK_SET); continue; }
        if(r<0){ g_race_result=r; break; }
        g_race_reads++;
    }
    g_race_done=1;
    SC1(N_THREADEXIT,0); for(;;){}
}
static void test_storage_parallel(void){
    section("parallel ext4 (SMP storage lock)");
    i64 mk=SC2(N_MKDIR,"/ext4p",0755);
    if(mk<0 && mk!=-E_EXIST){ sw("  note: root not writable ext4, skipping\n"); return; }
    ok(SC2(N_MKDIR,"/ext4p/shared",0755),"mkdir shared dir");

    i64 t0=SC0(N_GETTIME);
    i64 pids[PAR_WORKERS]; int forked=0;
    for(int k=0;k<PAR_WORKERS;k++){
        i64 pid=SC0(N_FORK);
        if(pid==0){ SC1(N_EXIT,par_worker(k)); for(;;){} }
        if(pid>0) pids[forked++]=pid;
    }
    checkv(forked==PAR_WORKERS,"forked the parallel workers",forked);
    int codes_ok=1; i64 bad=0;
    for(int k=0;k<forked;k++){
        int st=0; i64 w=SC3(N_WAIT,pids[k],&st,0);
        int code=(st>>8)&0xff;
        if(w!=pids[k] || code!=0){ codes_ok=0; bad=code; }
    }
    checkv(codes_ok,"workers wrote, verified and removed their files",bad);
    sw("  (3 workers x "); sdec(PAR_FILES*2); sw(" files: "); sdec(SC0(N_GETTIME)-t0); sw(" ms)\n");

    static struct DirEntry de[128]; memset(de,0,sizeof de);
    i64 n=SC3(N_READDIR,"/ext4p/shared",de,128);
    checkv(n==2+PAR_WORKERS*PAR_FILES,"shared dir holds every worker's files",n);
    int content_ok=1, rm=0;
    static u8 wb[PAR_SIZE], rb[PAR_SIZE];
    for(int k=0;k<PAR_WORKERS;k++){
        char spfx[48]; iname(spfx,"/ext4p/shared/w",k); int i=0; while(spfx[i]) i++; spfx[i++]='_'; spfx[i]='\0';
        for(int f=0;f<PAR_FILES;f++){
            char nm[64]; iname(nm,spfx,f); par_pattern(wb,k,f); memset(rb,0,PAR_SIZE);
            i64 h=t_open(nm,O_RDONLY,0); i64 r=(h>=0)?t_read(h,rb,PAR_SIZE):-1; if(h>=0) t_close(h);
            if(r!=PAR_SIZE || smemcmp(rb,wb,PAR_SIZE)!=0) content_ok=0;
            if(t_unlink(nm)==0) rm++;
        }
    }
    check(content_ok,"shared-dir files have the right contents");
    checkv(rm==PAR_WORKERS*PAR_FILES,"shared-dir files unlinked",rm);
    ok(SC1(N_RMDIR,"/ext4p/shared"),"rmdir shared dir");

    // A thread closes the fd that its sibling is reading from, often while the
    // read runs without the big kernel lock: the read must end with EBADF, not
    // crash. A smoke test only: without the fd pin in sys_read the stale fd is
    // freed memory whose poison happens to make VFS::read bail out, so a
    // missing pin does not reliably fail here.
    {
        static u8 blk[4096]; for(int i=0;i<4096;i++) blk[i]=(u8)i;
        i64 h=t_open("/ext4p/race.bin",O_WRONLY|O_CREAT|O_TRUNC,0644); int wok=(h>=0);
        for(int i=0;i<16&&wok;i++) if(t_write(h,blk,4096)!=4096) wok=0;
        if(h>=0) t_close(h);
        check(wok,"write the close-race file");
        int rounds_ok=1;
        for(int round=0;round<4&&rounds_ok;round++){
            g_race_fd=t_open("/ext4p/race.bin",O_RDONLY,0);
            if(g_race_fd<0){ rounds_ok=0; break; }
            g_race_reads=0; g_race_result=0; g_race_done=0;
            i64 th=SC3(N_THREADCREATE,(i64)race_reader,0,0);
            if(th<=0){ rounds_ok=0; t_close(g_race_fd); break; }
            // Spin rather than sleep: this thread keeps its CPU, so the close
            // runs while the reader is inside a read on another CPU.
            for(volatile u64 i=0;i<20000000ULL&&g_race_reads<(u64)(4+round);i++){}
            t_close(g_race_fd);
            for(int i=0;i<500&&!g_race_done;i++){ yield(); msleep(2); }
            if(!g_race_done || g_race_result!=-E_BADF) rounds_ok=0;
        }
        checkv(rounds_ok,"4x close under a reading thread: read ends with EBADF",g_race_result);
        t_unlink("/ext4p/race.bin");
    }
    ok(SC1(N_RMDIR,"/ext4p"),"rmdir /ext4p");
}

// ---- CPU affinity (SMP step 7) ----
// Must match kernel CpuInfoEntry.
struct CpuInfoEntry { u32 index,lapicId; u64 deviceIrqs; u32 currentPid,reserved; };
static i64 set_aff(i64 pid,u64 mask){ return SC3(N_SETAFFINITY,pid,8,&mask); }
static u64 get_aff(i64 pid){ u64 m=0; return SC3(N_GETAFFINITY,pid,8,&m)==8 ? m : 0; }
// True when every sample, taken across yields and a sleep, says CPU `cpu`.
static int stays_on(i64 cpu){
    for(int i=0;i<20;i++){
        if(SC0(N_GETCPU)!=cpu) return 0;
        if(i&1) yield(); else if(i==10) msleep(3);
    }
    return 1;
}
static volatile i64 g_aff_thread_cpu=-1; static volatile u64 g_aff_thread_mask=0;
static void aff_thread(void* arg){
    (void)arg;
    g_aff_thread_mask=get_aff(0);
    i64 cpu=SC0(N_GETCPU);
    for(int i=0;i<10&&cpu==SC0(N_GETCPU);i++) yield();
    g_aff_thread_cpu = cpu==SC0(N_GETCPU) ? cpu : -2;
    SC1(N_THREADEXIT,0); for(;;){}
}
static void test_affinity(void){
    section("CPU affinity");
    i64 n=SC2(N_CPUINFO,0,0);
    checkv(n>=1&&n<=16,"cpu_info: online CPU count",n);
    if(n<1||n>16) return;
    sw("  (cpus="); sdec(n); sw(")\n");
    u64 all=(1ULL<<n)-1;
    i64 cpu=SC0(N_GETCPU); checkv(cpu>=0&&cpu<n,"getcpu in range",cpu);
    checkv(get_aff(0)==all,"default affinity is every CPU",(i64)get_aff(0));

    { u64 m=0; err(SC3(N_GETAFFINITY,0,4,&m),E_INVAL,"getaffinity: size not a whole word -> EINVAL"); }
    err(set_aff(0,0),E_INVAL,"setaffinity: empty mask -> EINVAL");
    if(n<64) err(set_aff(0,1ULL<<63),E_INVAL,"setaffinity: only offline CPUs -> EINVAL");
    err(set_aff(999999,all),3,"setaffinity: no such pid -> ESRCH");
    { struct CpuInfoEntry e; err(SC2(N_CPUINFO,n,&e),E_INVAL,"cpu_info: index past last CPU -> EINVAL"); }

    // Pin to each CPU in turn: the scheduler must move us there and keep us.
    int pinned=1; i64 bad=-1;
    for(i64 c=0;c<n;c++){
        if(set_aff(0,1ULL<<c)<0 || get_aff(0)!=(1ULL<<c) || !stays_on(c)){ pinned=0; bad=c; break; }
    }
    checkv(pinned,"pinned to each CPU, getcpu agrees across yields/sleep",bad);

    // fork and threads inherit the mask.
    i64 last=n-1;
    set_aff(0,1ULL<<last);
    i64 pid=SC0(N_FORK);
    if(pid==0){ SC1(N_EXIT, get_aff(0)==(1ULL<<last) && stays_on(last) ? 51 : 52); for(;;){} }
    if(pid>0){ int st=0; SC3(N_WAIT,pid,&st,0); checkv(st==51||((st>>8)&0xff)==51,"fork child inherits affinity and runs there",st); }
    else checkv(0,"fork for affinity",pid);
    g_aff_thread_cpu=-1; g_aff_thread_mask=0;
    i64 th=SC3(N_THREADCREATE,(i64)aff_thread,0,0);
    if(th>0){ for(int i=0;i<500&&g_aff_thread_cpu==-1;i++){ yield(); msleep(2); } }
    checkv(th>0&&g_aff_thread_mask==(1ULL<<last)&&g_aff_thread_cpu==last,"new thread inherits affinity and runs there",g_aff_thread_cpu);

    ok(set_aff(0,all),"restore full affinity");
    checkv(get_aff(0)==all,"affinity restored",(i64)get_aff(0));

    // Informational: who served device interrupts (balanced onto APs).
    for(i64 c=0;c<n;c++){
        struct CpuInfoEntry e; memset(&e,0,sizeof(e));
        if(SC2(N_CPUINFO,c,&e)!=n || e.index!=(u32)c){ checkv(0,"cpu_info entry",c); return; }
        sw("  (cpu "); sdec(c); sw(" lapic="); sdec(e.lapicId); sw(" device irqs="); sdec((i64)e.deviceIrqs); sw(")\n");
    }
    check(1,"cpu_info entry for every CPU");
}

// exec from a process whose other threads are spinning on another CPU, sleeping
// and blocked in a pipe read: they must all die before the image is replaced.
static volatile i64 g_xt_spin=0, g_xt_sleep=0, g_xt_reader=0; static i64 g_xt_cpus=1; static u64 g_xt_pipe[2];
static void xt_spinner(void* arg){ (void)arg; set_aff(0,1ULL<<(g_xt_cpus-1)); for(;;) g_xt_spin++; }
static void xt_sleeper(void* arg){ (void)arg; for(;;){ g_xt_sleep++; msleep(3); } }
static void xt_reader(void* arg){ (void)arg; u8 b; g_xt_reader=1; t_read((i64)g_xt_pipe[0],&b,1); g_xt_reader=2; SC1(N_THREADEXIT,0); for(;;){} }
static void test_exec_threads(void){
    section("exec with live threads");
    g_xt_cpus=SC2(N_CPUINFO,0,0); if(g_xt_cpus<1) g_xt_cpus=1;
    u64 base=proc_count();
    int good=0; i64 bad=0;
    i64 t0=SC0(N_GETTIME);
    for(int round=0;round<3;round++){
        i64 pid=SC0(N_FORK);
        if(pid==0){
            set_aff(0,1);
            if(SC1(N_PIPE,g_xt_pipe)!=0) SC1(N_EXIT,61);
            if(SC3(N_THREADCREATE,(i64)xt_spinner,0,0)<=0 || SC3(N_THREADCREATE,(i64)xt_sleeper,0,0)<=0 ||
               SC3(N_THREADCREATE,(i64)xt_reader,0,0)<=0) SC1(N_EXIT,62);
            for(int i=0;i<1000&&!(g_xt_spin&&g_xt_sleep&&g_xt_reader);i++) msleep(2);
            msleep(10);   // let the reader block
            static const char* av[]={"ostest-helper","argc","a","b","c",0};
            static const char* ev[]={"PATH=/bin",0};
            SC3(N_EXEC,"/bin/ostest-helper",av,ev);
            SC1(N_EXIT,60); for(;;){}
        }
        if(pid<0){ bad=pid; break; }
        int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
        if(w==pid && ((st>>8)&0xff)==5) good++; else { bad=(st>>8)&0xff; break; }
    }
    sw("  (3 rounds: "); sdec(SC0(N_GETTIME)-t0); sw(" ms)\n");
    checkv(good==3,"3x exec kills spinning/sleeping/blocked threads, new image runs",bad);
    { u64 ac=proc_count(); checkv(ac<=base+2,"exec'd-away threads are reaped",(i64)ac-(i64)base); }
}

// exit() (from any thread) and a crash in a thread end the whole process.
static void xe_exiter(void* arg){ (void)arg; msleep(5); SC1(N_EXIT,24); for(;;){} }
static void xe_crasher(void* arg){ (void)arg; msleep(5); *(volatile int*)0=1; for(;;){} }
// Starts the spinner/sleeper/pipe-reader threads of test_exec_threads.
static int xt_start_threads(void){
    g_xt_spin=0; g_xt_sleep=0; g_xt_reader=0;
    if(SC1(N_PIPE,g_xt_pipe)!=0) return 0;
    if(SC3(N_THREADCREATE,(i64)xt_spinner,0,0)<=0 || SC3(N_THREADCREATE,(i64)xt_sleeper,0,0)<=0 ||
       SC3(N_THREADCREATE,(i64)xt_reader,0,0)<=0) return 0;
    for(int i=0;i<1000&&!(g_xt_spin&&g_xt_sleep&&g_xt_reader);i++) msleep(2);
    msleep(10);
    return 1;
}
static i64 xe_run(int mode){   // child exit status, or -1
    i64 pid=SC0(N_FORK);
    if(pid==0){
        set_aff(0,1);
        if(!xt_start_threads()) SC1(N_EXIT,62);
        if(mode==0) SC1(N_EXIT,23);
        SC3(N_THREADCREATE,(i64)(mode==1?xe_exiter:xe_crasher),0,0);
        for(;;) msleep(50);   // the main thread never exits by itself
    }
    if(pid<0) return -1;
    int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
    return w==pid ? ((st>>8)&0xff) : -1;
}
static void test_exit_threads(void){
    section("exit kills all threads");
    i64 t0=SC0(N_GETTIME);
    g_xt_cpus=SC2(N_CPUINFO,0,0); if(g_xt_cpus<1) g_xt_cpus=1;
    u64 base=proc_count();
    i64 r=xe_run(0); checkv(r==23,"main thread exit() kills spinning/sleeping/blocked threads",r);
    { u64 ac=proc_count(); checkv(ac<=base+1,"wait() returns only once the threads are reaped",(i64)ac-(i64)base); }
    r=xe_run(1); checkv(r==24,"exit() from a thread ends the process with its code",r);
    r=xe_run(2); checkv(r==128+11,"crash in a thread ends the process (SIGSEGV)",r);
    sw("  (3 processes: "); sdec(SC0(N_GETTIME)-t0); sw(" ms)\n");
    { u64 ac=proc_count(); checkv(ac<=base+1,"no threads left behind",(i64)ac-(i64)base); }
}

static void test_process_stress(void){
    section("process stress (churn/wait-race/kill-storm/orphan)");
    u64 base=proc_count(); checkv(base>0,"procinfo baseline count",(i64)base);

    // 1) fork/exit/wait churn: short-lived children, reaped exactly, codes intact.
    int churn_ok=1,code_ok=1;
    for(int i=0;i<20;i++){
        i64 pid=SC0(N_FORK);
        if(pid==0){ SC1(N_EXIT,(i&0x7f)); for(;;){} }
        if(pid<=0){ churn_ok=0; break; }
        int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
        if(w!=pid) churn_ok=0;
        if(((st>>8)&0xff)!=(i&0x7f)) code_ok=0;
    }
    check(churn_ok,"20x fork/exit/wait all reaped (no hang)");
    check(code_ok,"20x child exit codes preserved");
    { u64 ac=proc_count(); checkv(ac<=base+4,"no process leak after churn",(i64)ac-(i64)base); }
    sw("[stress] phase1 churn done\n");

    // 2) blocking wait must wake: parent parks in wait(), the child exits only
    //    after the parent has yielded into it, so a lost wakeup would hang here.
    int race_ok=1;
    for(int i=0;i<10;i++){
        i64 pid=SC0(N_FORK);
        if(pid==0){ for(int s=0;s<12;s++) SC0(N_YIELD); SC1(N_EXIT,5); for(;;){} }
        if(pid<=0){ race_ok=0; break; }
        int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
        if(w!=pid || ((st>>8)&0xff)!=5) race_ok=0;
    }
    check(race_ok,"10x blocking wait wakes (no lost wakeup)");
    sw("[stress] phase2 wait-race done\n");

    // 3) kill-storm: children looping across a syscall boundary (so signals are
    //    deliverable), SIGKILLed en masse, then reaped with status 128+SIGKILL.
    //    Reaping is bounded and re-asserts the kill each round (POSIX kill is
    //    idempotent) and polls with WNOHANG, so a single slow-to-die child can
    //    never wedge the whole run, and every child gets ample CPU to take
    //    delivery of its fatal signal.
    enum { KN=12 };
    static i64 kids[KN]; int spawned=0;
    for(int i=0;i<KN;i++){
        i64 pid=SC0(N_FORK);
        if(pid==0){ for(;;){ SC0(N_YIELD); SC1(N_SLEEP,5); } }
        if(pid>0) kids[spawned++]=pid; else break;
    }
    checkv(spawned==KN,"kill-storm forked all children",spawned);
    int kill_ok=1,reap_ok=1,sig_ok=1;
    for(int i=0;i<spawned;i++){ if(SC2(N_KILL,kids[i],SIG_KILL)!=0) kill_ok=0; }
    for(int i=0;i<spawned;i++){
        int st=0; i64 w=0;
        for(int tries=0; tries<500 && w==0; tries++){
            w=SC3(N_WAIT,kids[i],&st,WNOHANG);     // non-blocking reap attempt
            if(w==0){ SC2(N_KILL,kids[i],SIG_KILL); SC0(N_YIELD); SC1(N_SLEEP,2); } // not dead yet: re-assert + give CPU
        }
        if(w!=kids[i]) reap_ok=0;
        else if(((st>>8)&0xff)!=(128+SIG_KILL)) sig_ok=0;
    }
    check(kill_ok,"kill-storm SIGKILL delivered to all");
    check(reap_ok,"kill-storm all children reaped");
    check(sig_ok,"kill-storm exit status is 128+SIGKILL");
    { u64 ac=proc_count(); checkv(ac<=base+4,"no leak after kill-storm",(i64)ac-(i64)base); }
    sw("[stress] phase3 kill-storm done\n");

    // 4) orphan reaping: a child forks a grandchild then exits WITHOUT waiting.
    //    The grandchild is reparented to init(0) and must be auto-reaped rather
    //    than leaked as a permanent zombie.
    i64 c=SC0(N_FORK);
    if(c==0){
        i64 g=SC0(N_FORK);
        if(g==0){ for(int s=0;s<20;s++) SC1(N_SLEEP,2); SC1(N_EXIT,0); for(;;){} }
        SC1(N_EXIT,0); for(;;){}
    }
    if(c>0){ int st=0; i64 w=SC3(N_WAIT,c,&st,0); checkv(w==c,"reap child that abandoned a grandchild",w); }
    for(int i=0;i<40;i++){ SC0(N_YIELD); SC1(N_SLEEP,5); } // let the orphan die + be swept
    { u64 ac=proc_count(); checkv(ac<=base+4,"orphaned grandchild auto-reaped (no zombie leak)",(i64)ac-(i64)base); }
    sw("[stress] phase4 orphan done\n");

    // 5) waitpid option semantics: WNOHANG on a live child, tolerated no-op
    //    flags, rejected unknown flags, and ECHILD when no child matches.
    i64 lp=SC0(N_FORK);
    if(lp==0){ for(int s=0;s<25;s++) SC1(N_SLEEP,3); SC1(N_EXIT,9); for(;;){} }
    if(lp>0){
        int st=0;
        checkv(SC3(N_WAIT,lp,&st,WNOHANG)==0,"WNOHANG on live child returns 0",0);
        i64 wu=SC3(N_WAIT,lp,&st,WNOHANG|WUNTRACED|WCONTINUED); check(wu==0,"WUNTRACED|WCONTINUED accepted (no EINVAL)");
        err(SC3(N_WAIT,lp,&st,0x40000),E_INVAL,"unknown wait option -> EINVAL");
        int fst=0; i64 fw=SC3(N_WAIT,lp,&fst,0); checkv(fw==lp&&((fst>>8)&0xff)==9,"blocking wait reaps lingering child",fw);
    }
    err(SC3(N_WAIT,999999,0,0),E_CHILD,"wait(no such child) -> ECHILD");
}

// P1.3: copy-on-write fork, demand paging / overcommit, mprotect enforcement,
// and a scaled memory + fork-storm soak.
static void test_memory_p13(void){
    section("memory P1.3 (COW/demand/overcommit/soak)");

    // ---- copy-on-write fork isolation ----
    volatile u8* buf=(volatile u8*)SC3(N_MMAP,0,8192,PROT_R|PROT_W);
    checkv((i64)buf>0,"mmap COW buffer",(i64)buf);
    if((i64)buf>0){
        buf[0]=100; buf[4096]=101;            // fault both pages in, writable
        i64 pid=SC0(N_FORK);
        if(pid==0){
            int saw=(buf[0]==100 && buf[4096]==101);   // pre-fork data visible
            buf[0]=200; buf[4096]=201;                 // triggers COW copies
            int mine=(buf[0]==200 && buf[4096]==201);  // own writes visible
            SC1(N_EXIT,(saw&&mine)?55:1); for(;;){}
        }
        checkv(pid>0,"fork for COW",pid);
        if(pid>0){
            int st=0; i64 w=SC3(N_WAIT,pid,&st,0);
            checkv(w==pid,"reap COW child",w);
            checkv(((st>>8)&0xff)==55,"child: saw pre-fork data + wrote private COW copy",st);
            check(buf[0]==100 && buf[4096]==101,"parent COW pages unchanged by child");
            buf[0]=250; check(buf[0]==250,"parent COW write after fork ok");
        }
        ok(SC2(N_MUNMAP,buf,8192),"munmap COW buffer");
    }

    // ---- demand paging / overcommit: reserve 2 GiB (> VM RAM), touch sparsely ----
    u64 huge=2ULL*1024*1024*1024;
    i64 h=SC3(N_MMAP,0,huge,PROT_R|PROT_W);
    checkv(h>0,"mmap 2GiB lazy (overcommit) ok",h);
    if(h>0){
        volatile u8* p=(volatile u8*)h;
        p[0]=1; p[huge/2]=2; p[huge-1]=3;
        check(p[0]==1 && p[huge/2]==2 && p[huge-1]==3,"sparse touch across 2GiB mapping");
        check(p[4096]==0,"untouched demand page reads zero");
        ok(SC2(N_MUNMAP,h,huge),"munmap 2GiB");
    }

    // ---- mprotect enforcement: write to PROT_R page must SIGSEGV the child ----
    i64 mp=SC0(N_FORK);
    if(mp==0){
        volatile u8* q=(volatile u8*)SC3(N_MMAP,0,4096,PROT_R|PROT_W);
        if((i64)q<=0) SC1(N_EXIT,1);
        q[0]=1;                              // fault in read/write
        SC3(N_MPROTECT,q,4096,PROT_R);       // downgrade to read-only
        q[0]=2;                              // must fault -> SIGSEGV (exit 128+11)
        SC1(N_EXIT,3); for(;;){}             // 3 == protection not enforced
    }
    if(mp>0){
        int st=0; i64 w=SC3(N_WAIT,mp,&st,0);
        checkv(w==mp,"reap mprotect child",w);
        checkv(((st>>8)&0xff)==139,"write to PROT_R page kills child (SIGSEGV=128+11)",st);
    }

    // ---- scaled soak: touch every page of a large mapping, verify, free ----
    u64 soak=(u64)SOAK_MB*1024*1024;
    i64 s=SC3(N_MMAP,0,soak,PROT_R|PROT_W);
    checkv(s>0,"soak mmap ok",s);
    if(s>0){
        volatile u8* p=(volatile u8*)s;
        for(u64 off=0; off<soak; off+=4096) p[off]=(u8)((off>>12)&0xff);
        int okc=1; for(u64 off=0; off<soak && okc; off+=4096) if(p[off]!=(u8)((off>>12)&0xff)) okc=0;
        check(okc,"soak: touch+verify every page");
        ok(SC2(N_MUNMAP,s,soak),"soak munmap");
    }

    // ---- fork-storm: pressure COW + demand paging + heap/PMM under churn ----
    enum { NF=16 }; int good=0;
    for(int i=0;i<NF;i++){
        i64 f=SC0(N_FORK);
        if(f==0){
            volatile u8* c=(volatile u8*)SC3(N_MMAP,0,256*1024,PROT_R|PROT_W);
            int r=((i64)c>0);
            if(r){ for(u64 j=0;j<256*1024;j+=4096) c[j]=(u8)i; r=(c[0]==(u8)i && c[4096]==(u8)i); }
            SC1(N_EXIT,r?7:1); for(;;){}
        }
        if(f>0){ int st=0; i64 w=SC3(N_WAIT,f,&st,0); if(w==f && ((st>>8)&0xff)==7) good++; }
    }
    checkv(good==NF,"fork-storm: all children mmap+touch+exit ok",good);
}

// P2.1c: fd->path syscall + the resolve/compose/open mechanism that mlibc uses
// to make the *at family (openat/unlinkat/...) and fchdir honor a real dirfd.
static void test_fd_path(void){
    section("fd->path + *at resolve (P2.1c)");
    char buf[256];

    // fd->path on a regular file returns its absolute path.
    put_file("/fdp.txt","hi",2);
    i64 fh=t_open("/fdp.txt",O_RDONLY,0); checkv(fh>0,"open /fdp.txt",fh);
    memset(buf,0,sizeof buf);
    i64 n=t_fd_path(fh,buf,sizeof buf);
    checkv(n==8&&sstreq(buf,"/fdp.txt"),"fd_path(file) == /fdp.txt",n);
    t_close(fh);

    // fd->path canonicalizes a cwd-relative open to an absolute path.
    SC1(N_CHDIR,"/"); put_file("/rel.txt","x",1);
    fh=t_open("rel.txt",O_RDONLY,0);       // relative to cwd "/"
    memset(buf,0,sizeof buf); n=t_fd_path(fh,buf,sizeof buf);
    check(n>0&&sstreq(buf,"/rel.txt"),"fd_path canonicalizes relative open");
    t_close(fh);

    // fd->path on a directory fd, then compose dir + "/" + name and open it
    // (exactly what mlibc's at_resolve + openat does).
    SC2(N_MKDIR,"/fdpd",0755);
    put_file("/fdpd/inner.txt","INNER",5);
    i64 dh=t_open("/fdpd",O_RDONLY,0); checkv(dh>0,"open dir /fdpd",dh);
    memset(buf,0,sizeof buf); n=t_fd_path(dh,buf,sizeof buf);
    check(n>0&&sstreq(buf,"/fdpd"),"fd_path(dir) == /fdpd");
    // compose "/fdpd" + "/" + "inner.txt"
    char comp[300]; int ci=0; for(int i=0;buf[i];i++)comp[ci++]=buf[i];
    if(ci==0||comp[ci-1]!='/')comp[ci++]='/'; const char* nm="inner.txt";
    for(int i=0;nm[i];i++)comp[ci++]=nm[i]; comp[ci]='\0';
    i64 ih=t_open(comp,O_RDONLY,0); checkv(ih>0,"openat-composed /fdpd/inner.txt",ih);
    if(ih>0){ char b[8]; memset(b,0,8); i64 r=t_read(ih,b,8); check(r==5&&smemcmp(b,"INNER",5)==0,"composed open reads correct file"); t_close(ih); }
    t_close(dh);

    // Errors: ERANGE for a too-small buffer, EBADF for a bogus handle.
    fh=t_open("/fdp.txt",O_RDONLY,0);
    neg(t_fd_path(fh,buf,3),"fd_path ERANGE on tiny buffer");
    t_close(fh);
    neg(t_fd_path(0x7fffffff,buf,sizeof buf),"fd_path bad handle fails");

    // fd->path on a pipe (pathless) fails rather than returning garbage.
    { u64 pf[2]={0,0}; if(SC1(N_PIPE,pf)==0){ neg(t_fd_path((i64)pf[0],buf,sizeof buf),"fd_path on pipe fails"); t_close((i64)pf[0]); t_close((i64)pf[1]); } }

    t_unlink("/fdpd/inner.txt"); SC1(N_RMDIR,"/fdpd"); t_unlink("/fdp.txt"); t_unlink("/rel.txt");
}

// P2.1a: real file-backed mmap (kernel MmapFile/Msync + demand paging from the
// vnode + MAP_SHARED writeback). Exercised against the ext4 root so the disk I/O
// path is covered.
static void test_mmap_file(void){
    section("mmap (file-backed, P2.1a)");
    const char* P="/mmapf.bin";
    enum { FSZ=5000 };   // spans two pages plus a 904-byte tail
    static u8 pat[FSZ];
    for(int i=0;i<FSZ;i++) pat[i]=(u8)((i*7+3)&0xff);
    checkv(put_file(P,(const char*)pat,FSZ)==0,"seed 5000B file",0);

    // MAP_PRIVATE, read-only: file bytes visible, EOF tail zero-filled.
    i64 fd=t_open(P,O_RDONLY,0); checkv(fd>=0,"open ro",fd);
    i64 m=t_mmap_file(fd,8192,PROT_R,MAP_PRIVATE,0);
    checkv(m>0,"mmap MAP_PRIVATE ro",m);
    if(m>0){
        const volatile u8* p=(const volatile u8*)m;
        int okc=1; for(int i=0;i<FSZ;i++){ if(p[i]!=pat[i]){okc=0;break;} }
        check(okc,"file contents visible via mapping");
        int okz=1; for(int i=FSZ;i<8192;i++){ if(p[i]!=0){okz=0;break;} }
        check(okz,"bytes past EOF are zero-filled");
        ok(SC2(N_MUNMAP,m,8192),"munmap private ro");
    }
    t_close(fd);

    // MAP_PRIVATE writes stay local: the file must not change (even after msync).
    fd=t_open(P,O_RDWR,0); checkv(fd>=0,"open rw (private)",fd);
    m=t_mmap_file(fd,4096,PROT_R|PROT_W,MAP_PRIVATE,0);
    checkv(m>0,"mmap MAP_PRIVATE rw",m);
    if(m>0){
        volatile u8* p=(volatile u8*)m; p[0]=0xAA; p[1]=0xBB; p[100]=0xCC;
        SC3(N_MSYNC,m,4096,0);   // private: must be a no-op for the file
        ok(SC2(N_MUNMAP,m,4096),"munmap private rw");
    }
    t_close(fd);
    { u8 b[8]; memset(b,0,8); i64 h=t_open(P,O_RDONLY,0); i64 r=(h>=0)?t_read(h,b,8):-1; if(h>=0)t_close(h);
      check(r==8&&b[0]==pat[0]&&b[1]==pat[1],"private writes did NOT reach the file"); }

    // MAP_SHARED write + msync: changes land in the file.
    fd=t_open(P,O_RDWR,0); checkv(fd>=0,"open rw (shared)",fd);
    m=t_mmap_file(fd,4096,PROT_R|PROT_W,MAP_SHARED,0);
    checkv(m>0,"mmap MAP_SHARED rw",m);
    if(m>0){
        volatile u8* p=(volatile u8*)m; p[0]=0x11; p[1]=0x22; p[2]=0x33; p[9]=0x99;
        ok(SC3(N_MSYNC,m,4096,0),"msync shared");
        ok(SC2(N_MUNMAP,m,4096),"munmap shared");
    }
    t_close(fd);
    { u8 b[16]; memset(b,0,16); i64 h=t_open(P,O_RDONLY,0); i64 r=(h>=0)?t_read(h,b,16):-1; if(h>=0)t_close(h);
      check(r>=10&&b[0]==0x11&&b[1]==0x22&&b[2]==0x33&&b[9]==0x99,"shared writes landed via msync"); }

    // MAP_SHARED write back happens on munmap even without an explicit msync.
    fd=t_open(P,O_RDWR,0);
    m=t_mmap_file(fd,4096,PROT_R|PROT_W,MAP_SHARED,0);
    if(m>0){ volatile u8* p=(volatile u8*)m; p[5]=0x5A; ok(SC2(N_MUNMAP,m,4096),"munmap shared (implicit flush)"); }
    t_close(fd);
    { u8 b[8]; memset(b,0,8); i64 h=t_open(P,O_RDONLY,0); i64 r=(h>=0)?t_read(h,b,8):-1; if(h>=0)t_close(h);
      check(r>=6&&b[5]==0x5A,"munmap flushed shared page to file"); }

    // Non-zero page offset: mapping the 2nd page must expose file[4096..].
    const char* P2="/mmapf2.bin";
    enum { FSZ2=9000 };
    static u8 pat2[FSZ2];
    for(int i=0;i<FSZ2;i++) pat2[i]=(u8)((i*3+1)&0xff);
    put_file(P2,(const char*)pat2,FSZ2);
    fd=t_open(P2,O_RDONLY,0);
    m=t_mmap_file(fd,4096,PROT_R,MAP_PRIVATE,4096);
    checkv(m>0,"mmap at offset 4096",m);
    if(m>0){
        const volatile u8* p=(const volatile u8*)m;
        int okc=1; for(int i=0;i<4096;i++){ if(p[i]!=pat2[4096+i]){okc=0;break;} }
        check(okc,"offset mapping matches file[4096..]");
        SC2(N_MUNMAP,m,4096);
    }
    t_close(fd);

    // Bad fd must fail rather than return anonymous memory.
    { struct MmapFileArgs a; a.addr=0;a.length=4096;a.prot=PROT_R;a.flags=MAP_PRIVATE;a.fd=0x7fffffff;a.offset=0;
      neg(SC1(N_MMAPFILE,&a),"mmap with bad fd fails"); }

    t_unlink(P); t_unlink(P2);
}

// The kernel enters _start with a 16-byte-aligned RSP (pointing at argc), but the
// compiler assumes the post-`call` SysV convention (RSP % 16 == 8). Without
// realignment, 16-byte-aligned stack locals are actually 8-misaligned and any
// compiler-emitted aligned SSE store (movaps, e.g. from `u64 x[2]={0,0}`) #GPs.
__attribute__((force_align_arg_pointer))
void _start(void){
    // Ignore SIGPIPE (handler==1 => SIG_IGN; kernel skips delivery entirely) so the
    // broken-pipe write test just observes EPIPE instead of taking a signal.
    SC2(N_SIGNAL,SIG_PIPE,1);

    sw("\n[ostest] BEGIN InstantOS feature test suite\n");

    test_osinfo();
    test_time_entropy();
    test_memory();
    test_files_tmp();
    test_ext4_fs();   // P1.4: build-grade write-path stress on the ext4 root
    test_file_errors();
    test_dirs();
    test_links();
    test_metadata();
    test_devices();
    test_initrd_ro();
    test_etc_seeded();
    test_statfs();
    test_pipes();
    test_dup_fcntl();
    test_pty();
    test_ipc();
    test_sockets();
    test_gated_negative();
    test_storage();
    test_process();
    test_exec_spawn();   // P1.2: argv/env, shebang, deep paths, cloexec/pipe2
    test_signals();
    test_net_p5();       // network step 5: sendto/recvfrom, ping sockets, timers
    test_wallclock();    // network step 6: settable wall clock
    test_threads();   // fixed: FPUState null-init + right-sized buffer + XSAVE guard
    test_gs_base();      // SMP step 2: kernel GS invariant (swapgs on every user entry/exit)
    test_tlb_shootdown(); // SMP step 5: stale translations on sibling CPUs
    test_storage_parallel(); // SMP step 6: ext4 without the big kernel lock
    test_affinity();         // SMP step 7: sched_setaffinity / getcpu / cpu_info
    test_exec_threads();     // exec kills the other threads first
    test_exit_threads();     // exit()/crash in any thread ends the process
    test_process_stress();   // P1.1: wait-race / kill-storm / orphan reaping
    test_memory_p13();   // P1.3: COW fork, demand paging/overcommit, mprotect, soak
    test_mmap_file();    // P2.1a: file-backed mmap (demand page from vnode + MAP_SHARED writeback)
    test_fd_path();      // P2.1c: fd->path syscall + *at resolve/compose mechanism

    section("");  // flush last section summary
    sw("\n[ostest] SCORE pass="); sdec(g_pass); sw(" total="); sdec(g_total);
    sw(" fail="); sdec(g_total-g_pass);
    sw(" pct="); sdec(g_total?(g_pass*100)/g_total:0); sw("\n");
    sw("[ostest] END\n");

    for(;;){ SC1(N_SLEEP,1000); }
}
