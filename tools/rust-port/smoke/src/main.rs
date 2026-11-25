// rust-smoke: Rust's std (the x86_64-unknown-linux-musl one) on InstantOS,
// linked against mlibc (tools/build-rust-smoke.sh). Prints "[rust] ok/FAIL"
// lines and exits 0 only if everything passed. "rust-smoke --child N" is the
// half that std::process runs.
use std::cell::Cell;
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex, RwLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

static PASSED: AtomicUsize = AtomicUsize::new(0);
static FAILED: AtomicUsize = AtomicUsize::new(0);

fn check(ok: bool, what: &str) {
    if ok {
        PASSED.fetch_add(1, Ordering::Relaxed);
    } else {
        FAILED.fetch_add(1, Ordering::Relaxed);
    }
    println!("[rust] {}: {}", if ok { "ok" } else { "FAIL" }, what);
}

thread_local! {
    static LOCAL: Cell<u32> = const { Cell::new(0) };
}

fn threads() {
    // Mutex and Condvar are futexes in std: a counter several threads share.
    let shared = Arc::new((Mutex::new(0u64), Condvar::new()));
    let mut handles = Vec::new();
    for _ in 0..8 {
        let shared = Arc::clone(&shared);
        handles.push(std::thread::spawn(move || {
            for _ in 0..10000 {
                *shared.0.lock().unwrap() += 1;
            }
            shared.1.notify_all();
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    check(*shared.0.lock().unwrap() == 80000, "8 threads, a Mutex, 80000 increments");

    // A Condvar wait that something else ends.
    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let p2 = Arc::clone(&pair);
    let t = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(30));
        *p2.0.lock().unwrap() = true;
        p2.1.notify_one();
    });
    let mut ready = pair.0.lock().unwrap();
    while !*ready {
        ready = pair.1.wait(ready).unwrap();
    }
    drop(ready);
    t.join().unwrap();
    check(true, "Condvar::wait wakes");
    let (_g, timeout) = pair.1.wait_timeout(pair.0.lock().unwrap(), Duration::from_millis(20)).unwrap();
    check(timeout.timed_out(), "Condvar::wait_timeout times out");

    let lock = Arc::new(RwLock::new(5));
    let readers: Vec<_> = (0..4)
        .map(|_| {
            let lock = Arc::clone(&lock);
            std::thread::spawn(move || *lock.read().unwrap())
        })
        .collect();
    *lock.write().unwrap() += 1;
    let seen: Vec<i32> = readers.into_iter().map(|h| h.join().unwrap()).collect();
    check(seen.iter().all(|v| *v == 5 || *v == 6), "RwLock readers and a writer");

    let (tx, rx) = mpsc::channel();
    for i in 0..4 {
        let tx = tx.clone();
        std::thread::spawn(move || tx.send(i).unwrap());
    }
    drop(tx);
    let mut got: Vec<i32> = rx.iter().collect();
    got.sort();
    check(got == vec![0, 1, 2, 3], "mpsc from 4 threads");

    LOCAL.with(|v| v.set(7));
    let other = std::thread::spawn(|| LOCAL.with(|v| v.get())).join().unwrap();
    check(LOCAL.with(|v| v.get()) == 7 && other == 0, "thread_local is per thread");

    let named = std::thread::Builder::new()
        .name("worker".into())
        .stack_size(256 * 1024)
        .spawn(|| std::thread::current().name().map(String::from))
        .unwrap()
        .join()
        .unwrap();
    check(named.as_deref() == Some("worker"), "a named thread with its own stack size");
    check(std::thread::available_parallelism().map(|n| n.get() >= 1).unwrap_or(false), "available_parallelism");
}

fn time() {
    let t0 = Instant::now();
    std::thread::sleep(Duration::from_millis(50));
    let took = t0.elapsed();
    check(took >= Duration::from_millis(50) && took < Duration::from_secs(2), "sleep 50 ms by Instant");
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
    check(now > 1_700_000_000, "SystemTime after 2023");
}

fn files() {
    let dir = std::path::Path::new("/tmp/rust-smoke");
    let _ = std::fs::remove_dir_all(dir);
    check(std::fs::create_dir_all(dir.join("a/b")).is_ok(), "create_dir_all");
    let file = dir.join("a/b/hello.txt");
    check(std::fs::write(&file, b"hello from rust").is_ok(), "fs::write");
    check(std::fs::read_to_string(&file).ok().as_deref() == Some("hello from rust"), "fs::read_to_string");
    let meta = std::fs::metadata(&file);
    check(meta.as_ref().map(|m| m.len() == 15 && m.is_file()).unwrap_or(false), "metadata: size and kind");
    let mut f = std::fs::OpenOptions::new().append(true).open(&file).unwrap();
    f.write_all(b"!").unwrap();
    drop(f);
    check(std::fs::read(&file).map(|d| d.len()).unwrap_or(0) == 16, "append");
    let moved = dir.join("moved.txt");
    check(std::fs::rename(&file, &moved).is_ok() && moved.exists() && !file.exists(), "rename");
    let names: Vec<String> = std::fs::read_dir(dir)
        .map(|it| it.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
        .unwrap_or_default();
    check(names.contains(&"a".to_string()) && names.contains(&"moved.txt".to_string()), "read_dir");
    check(std::fs::canonicalize(dir.join("a/../moved.txt")).map(|p| p == moved).unwrap_or(false), "canonicalize");
    check(std::fs::remove_dir_all(dir).is_ok() && !dir.exists(), "remove_dir_all");
    check(std::env::current_dir().is_ok(), "current_dir");
    let exe = std::env::current_exe();
    check(exe.as_ref().map(|p| p.ends_with("rust-smoke")).unwrap_or(false), &format!("current_exe ({exe:?})"));
}

fn processes() {
    let out = std::process::Command::new("/bin/rust-smoke").args(["--child", "21"]).output();
    check(
        out.as_ref().map(|o| o.status.code() == Some(0) && String::from_utf8_lossy(&o.stdout).trim() == "42").unwrap_or(false),
        "Command::output runs a child and reads its stdout",
    );
    let status = std::process::Command::new("/bin/rust-smoke").args(["--child", "exit7"]).status();
    check(status.map(|s| s.code() == Some(7)).unwrap_or(false), "a child's exit status");
    check(std::process::id() > 0, "process::id");
}

fn sockets() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    let server = std::thread::spawn(move || {
        let (mut s, _) = listener.accept().unwrap();
        let mut buf = [0u8; 5];
        s.read_exact(&mut buf).unwrap();
        s.write_all(&buf).unwrap();
    });
    let mut c = TcpStream::connect(addr).unwrap();
    c.write_all(b"hello").unwrap();
    let mut back = [0u8; 5];
    c.read_exact(&mut back).unwrap();
    server.join().unwrap();
    check(&back == b"hello", "TCP echo on 127.0.0.1");

    let (mut a, mut b) = UnixStream::pair().unwrap();
    a.write_all(b"ping").unwrap();
    let mut got = [0u8; 4];
    b.read_exact(&mut got).unwrap();
    check(&got == b"ping", "UnixStream::pair");
    b.set_read_timeout(Some(Duration::from_millis(30))).unwrap();
    let t0 = Instant::now();
    let r = b.read(&mut got);
    check(r.is_err() && t0.elapsed() >= Duration::from_millis(25), "a read timeout");
}

fn panics_and_hashing() {
    let caught = std::panic::catch_unwind(|| {
        let v: Vec<i32> = Vec::new();
        v[3]
    });
    check(caught.is_err(), "catch_unwind catches a panic (unwinding through libunwind)");
    let a: HashMap<u32, u32> = (0..100).map(|i| (i, i * 2)).collect();
    check(a.get(&50) == Some(&100), "HashMap (random keys from getrandom)");
    let env = std::env::var("PATH");
    check(env.is_ok(), "env::var(PATH)");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() == 3 && args[1] == "--child" {
        if args[2] == "exit7" {
            std::process::exit(7);
        }
        let n: u32 = args[2].parse().unwrap_or(0);
        println!("{}", n * 2);
        return;
    }
    std::panic::set_hook(Box::new(|_| {}));   // the panic test is expected
    threads();
    time();
    files();
    processes();
    sockets();
    panics_and_hashing();
    let passed = PASSED.load(Ordering::Relaxed);
    let failed = FAILED.load(Ordering::Relaxed);
    println!("rust-smoke: {} of {} passed", passed, passed + failed);
    std::process::exit(if failed == 0 { 0 } else { 1 });
}
