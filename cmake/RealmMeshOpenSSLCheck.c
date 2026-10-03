/* Configure-time OpenSSL probe (see RealmMeshOpenSSL.cmake): linking proves the
 * libraries suit the target, running proves the headers match the runtime. */
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <stdio.h>

int main(void) {
    SSL_CTX *context = SSL_CTX_new(TLS_method());
    if (context == NULL) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        return 2;
    }
    SSL_CTX_free(context);

    unsigned long header = (unsigned long)OPENSSL_VERSION_NUMBER;
    unsigned long runtime = (unsigned long)OpenSSL_version_num();
    printf("headers: %s\nruntime: %s\n", OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION));
    /* 0xMNN00PP0L: compare major and minor. */
    return (header >> 20) == (runtime >> 20) ? 0 : 1;
}
