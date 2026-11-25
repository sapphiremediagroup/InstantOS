// OpenSSL on InstantOS (tools/run-openssl-smoke.sh): known answers, randomness,
// a TLS 1.3 and a TLS 1.2 handshake in memory with certificate checks, and
// four threads doing all of it at once. Prints [openssl] ok/FAIL lines and a
// SCORE line.
//
// Built a second time with -DNO_ENTROPY: that program's own getentropy()
// always fails and takes the place of libc's, and every request for randomness
// must fail with it. OpenSSL falling back to anything else would be a hole.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

static int passed, failed;

static void check(int ok, const char* what) {
    printf("[openssl] %s: %s\n", ok ? "ok" : "FAIL", what);
    if (ok) {
        passed++;
    } else {
        failed++;
        ERR_print_errors_fp(stdout);
    }
    fflush(stdout);
}

static int score(const char* name) {
    printf("[openssl] SCORE %s pass=%d fail=%d\n", name, passed, failed);
    fflush(stdout);
    return failed != 0;
}

#ifdef NO_ENTROPY
static int calls;
int getentropy(void* buffer, size_t length) {
    (void)buffer;
    (void)length;
    calls++;
    errno = EIO;
    return -1;
}

int main(void) {
    unsigned char b[32] = {0};
    int r = RAND_bytes(b, sizeof(b));
    ERR_clear_error();
    int zero = 1;
    for (size_t i = 0; i < sizeof(b); i++) zero = zero && b[i] == 0;
    check(calls > 0, "OpenSSL asked getentropy");
    check(r != 1, "RAND_bytes fails when getentropy does");
    check(zero, "and returned nothing that looks random");
    check(RAND_status() != 1, "RAND_status says it is not seeded");
    ERR_clear_error();
    EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "X25519");
    ERR_clear_error();
    check(key == NULL, "a key cannot be made");
    return score("noentropy");
}
#else

static int hex_is(const unsigned char* data, size_t n, const char* hex) {
    char two[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(two, sizeof(two), "%02x", data[i]);
        if (strncmp(two, hex + 2 * i, 2) != 0) return 0;
    }
    return hex[2 * n] == 0;
}

static int sha256_ok(void) {
    unsigned char md[32];
    unsigned int n = 0;
    return EVP_Digest("abc", 3, md, &n, EVP_sha256(), NULL) == 1 && n == 32 &&
           hex_is(md, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// GCM specification, test case 14: AES-256, 128 zero bits, all-zero key and IV.
static int aes_gcm_ok(void) {
    unsigned char key[32] = {0}, iv[12] = {0}, in[16] = {0}, out[16], tag[16];
    int n = 0, m = 0;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
    int ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
             EVP_EncryptUpdate(c, out, &n, in, 16) == 1 && EVP_EncryptFinal_ex(c, out + n, &m) == 1 &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(c);
    return ok && hex_is(out, 16, "cea7403d4d606b6e074ec5d3baf39d18") &&
           hex_is(tag, 16, "d0d1c8a799996bf0265b98b5d48ab919");
}

static int sign_ok(const char* type) {
    EVP_PKEY* key = strcmp(type, "RSA") == 0 ? EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048)
                  : strcmp(type, "EC") == 0  ? EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256")
                                             : EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    if (!key) return 0;
    const EVP_MD* md = strcmp(type, "ED25519") == 0 ? NULL : EVP_sha256();
    unsigned char sig[512], msg[64];
    size_t n = sizeof(sig);
    memset(msg, 7, sizeof(msg));
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    int ok = EVP_DigestSignInit(c, NULL, md, NULL, key) == 1 && EVP_DigestSign(c, sig, &n, msg, sizeof(msg)) == 1;
    EVP_MD_CTX_free(c);
    c = EVP_MD_CTX_new();
    ok = ok && EVP_DigestVerifyInit(c, NULL, md, NULL, key) == 1 && EVP_DigestVerify(c, sig, n, msg, sizeof(msg)) == 1;
    EVP_MD_CTX_free(c);
    msg[0] ^= 1;
    c = EVP_MD_CTX_new();
    ok = ok && EVP_DigestVerifyInit(c, NULL, md, NULL, key) == 1 && EVP_DigestVerify(c, sig, n, msg, sizeof(msg)) != 1;
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(key);
    ERR_clear_error();
    return ok;
}

static X509* make_cert(EVP_PKEY* key, const char* dns) {
    X509* x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, key);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC, (const unsigned char*)dns, -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(x));
    char san[64];
    snprintf(san, sizeof(san), "DNS:%s", dns);
    X509_EXTENSION* e = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, san);
    X509_add_ext(x, e, -1);
    X509_EXTENSION_free(e);
    if (!X509_sign(x, key, EVP_sha256())) {
        X509_free(x);
        return NULL;
    }
    return x;
}

// A handshake over memory, 64 KiB each way. `trusted`: the client knows the
// certificate; `host`: the name it expects. 1 when it was set up and the data
// came through whole.
static int handshake(int version, int trusted, const char* host, long* verify) {
    int ok = 0;
    EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    X509* cert = key ? make_cert(key, "instant.test") : NULL;
    SSL_CTX* sc = SSL_CTX_new(TLS_server_method());
    SSL_CTX* cc = SSL_CTX_new(TLS_client_method());
    SSL *s = NULL, *c = NULL;
    unsigned char *data = NULL, *back = NULL;
    const int size = 65536;
    if (!cert || !sc || !cc) goto out;
    SSL_CTX_use_certificate(sc, cert);
    SSL_CTX_use_PrivateKey(sc, key);
    SSL_CTX_set_min_proto_version(cc, version);
    SSL_CTX_set_max_proto_version(cc, version);
    SSL_CTX_set_verify(cc, SSL_VERIFY_PEER, NULL);
    if (trusted) X509_STORE_add_cert(SSL_CTX_get_cert_store(cc), cert);
    s = SSL_new(sc);
    c = SSL_new(cc);
    SSL_set1_host(c, host);
    BIO *a, *b;
    if (!BIO_new_bio_pair(&a, 65536, &b, 65536)) goto out;
    SSL_set_bio(s, a, a);
    SSL_set_bio(c, b, b);
    SSL_set_accept_state(s);
    SSL_set_connect_state(c);
    int dc = 0, ds = 0;
    for (int i = 0; i < 50 && !(dc && ds); i++) {
        if (!dc) dc = SSL_do_handshake(c) == 1;
        if (!ds) ds = SSL_do_handshake(s) == 1;
    }
    if (verify) *verify = SSL_get_verify_result(c);
    if (!dc || !ds || SSL_version(c) != version) goto out;
    data = malloc(size);
    back = malloc(size);
    if (!data || !back || RAND_bytes(data, size) != 1) goto out;
    int sent = 0, got = 0;
    while (got < size) {
        if (sent < size) {
            int n = SSL_write(c, data + sent, size - sent > 8192 ? 8192 : size - sent);
            if (n > 0) sent += n;
        }
        int n = SSL_read(s, back + got, size - got);
        if (n > 0) got += n;
        else if (sent == size) break;
    }
    ok = got == size && memcmp(data, back, size) == 0;
out:
    free(data);
    free(back);
    SSL_free(s);
    SSL_free(c);
    SSL_CTX_free(sc);
    SSL_CTX_free(cc);
    X509_free(cert);
    EVP_PKEY_free(key);
    ERR_clear_error();
    return ok;
}

static int random_ok(void) {
    unsigned char a[32], b[32];
    if (RAND_status() != 1 || RAND_bytes(a, 32) != 1 || RAND_bytes(b, 32) != 1 || memcmp(a, b, 32) == 0) return 0;
    // About half the bits of 4 KiB are set (sigma is 90 bits).
    static unsigned char big[4096];
    if (RAND_bytes(big, sizeof(big)) != 1) return 0;
    int bits = 0;
    for (size_t i = 0; i < sizeof(big); i++) bits += __builtin_popcount(big[i]);
    return bits > 16384 - 900 && bits < 16384 + 900;
}

// A child must not repeat its parent's randomness.
static int fork_ok(void) {
    int p[2];
    unsigned char mine[32], theirs[32];
    if (RAND_bytes(mine, 32) != 1 || pipe(p) != 0) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        unsigned char b[32];
        int ok = RAND_bytes(b, 32) == 1 && write(p[1], b, 32) == 32;
        _exit(ok ? 0 : 1);
    }
    close(p[1]);
    size_t got = 0;
    ssize_t n;
    while (got < 32 && (n = read(p[0], theirs + got, 32 - got)) > 0) got += (size_t)n;
    close(p[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return pid > 0 && got == 32 && RAND_bytes(mine, 32) == 1 && memcmp(mine, theirs, 32) != 0;
}

static void* worker(void* arg) {
    int* ok = arg;
    for (int i = 0; i < 15 && *ok; i++) {
        long v = -1;
        *ok = sha256_ok() && aes_gcm_ok() && sign_ok("EC") && sign_ok("ED25519") &&
              handshake(i % 2 ? TLS1_2_VERSION : TLS1_3_VERSION, 1, "instant.test", &v) && v == X509_V_OK;
    }
    return NULL;
}

int main(void) {
    printf("[openssl] %s\n", OpenSSL_version(OPENSSL_VERSION));
    check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER, "the library is the version of its headers");
    check(strstr(OpenSSL_version(OPENSSL_DIR), "\"/etc/ssl\"") != NULL, "OPENSSLDIR is /etc/ssl");
    check(sha256_ok(), "SHA-256 known answer");
    check(aes_gcm_ok(), "AES-256-GCM known answer");
    check(random_ok(), "RAND_bytes");
    check(fork_ok(), "a forked child's randomness is its own");
    check(sign_ok("RSA"), "RSA 2048 sign and verify");
    check(sign_ok("EC"), "ECDSA P-256 sign and verify");
    check(sign_ok("ED25519"), "Ed25519 sign and verify");
    long v = -1;
    check(handshake(TLS1_3_VERSION, 1, "instant.test", &v) && v == X509_V_OK, "TLS 1.3, certificate verified");
    check(handshake(TLS1_2_VERSION, 1, "instant.test", &v) && v == X509_V_OK, "TLS 1.2, certificate verified");
    check(!handshake(TLS1_3_VERSION, 0, "instant.test", &v) && v != X509_V_OK, "an unknown certificate is refused");
    check(!handshake(TLS1_3_VERSION, 1, "other.test", &v) && v == X509_V_ERR_HOSTNAME_MISMATCH,
          "a certificate for another name is refused");
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    check(ctx && SSL_CTX_set_default_verify_paths(ctx) == 1, "the default verify paths");
    X509_STORE* store = ctx ? SSL_CTX_get_cert_store(ctx) : NULL;
    check(store && X509_STORE_load_file(store, "/etc/ssl/cert.pem") == 1 &&
              sk_X509_OBJECT_num(X509_STORE_get0_objects(store)) > 50,
          "/etc/ssl/cert.pem holds the system's CAs");
    SSL_CTX_free(ctx);

    pthread_t threads[4];
    int ok[4] = {1, 1, 1, 1};
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&threads[i], NULL, worker, &ok[i]) != 0) ok[i] = -1;
    }
    for (int i = 0; i < 4; i++) {
        if (ok[i] != -1) pthread_join(threads[i], NULL);
    }
    check(ok[0] == 1 && ok[1] == 1 && ok[2] == 1 && ok[3] == 1, "four threads: digests, signatures and handshakes at once");
    return score("main");
}
#endif
