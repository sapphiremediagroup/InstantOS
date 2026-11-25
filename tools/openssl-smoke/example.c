// gcc /examples/openssl.c -o /tmp/openssl-example -lssl -lcrypto
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

int main(void) {
    unsigned char md[32], random[16];
    unsigned int n = 0;
    const char* text = "InstantOS";
    if (EVP_Digest(text, strlen(text), md, &n, EVP_sha256(), NULL) != 1 || RAND_bytes(random, sizeof(random)) != 1) {
        return 1;
    }
    SSL_CTX* tls = SSL_CTX_new(TLS_client_method());
    if (!tls) {
        return 1;
    }
    SSL_CTX_free(tls);
    printf("%s, sha256(%s) = ", OpenSSL_version(OPENSSL_VERSION), text);
    for (unsigned int i = 0; i < n; i++) printf("%02x", md[i]);
    printf("\n");
    return 0;
}
