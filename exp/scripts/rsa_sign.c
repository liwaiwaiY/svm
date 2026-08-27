/* rsa_sign.c - AF_ALG akcipher RSA-PKCS1(v1.5, SHA-256) sign workload.
 *
 * Usage: ./rsa_sign <key.der> <iters>
 *
 * Routes through the kernel crypto API ("akcipher" type) so that the
 * kernel picks the highest-priority implementation, which is
 * virtio-pkcs1-rsa / virtio-crypto-rsa (priority 150) on the SVM setup.
 * In SVM mode the operation is forwarded to the remote stub (cryptodev).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/if_alg.h>

#ifndef AF_ALG
#define AF_ALG 38
#endif

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <key.der> <iters>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen key"); return 1; }
    fseek(f, 0, SEEK_END);
    long klen = ftell(f);
    rewind(f);
    unsigned char *key = malloc(klen);
    if (fread(key, 1, klen, f) != (size_t)klen) { perror("fread"); return 1; }
    fclose(f);

    long iters = atol(argv[2]);
    if (iters <= 0) iters = 1;

    int tf = socket(AF_ALG, SOCK_SEQPACKET, 0);
    if (tf < 0) { perror("socket af_alg"); return 1; }

    struct sockaddr_alg sa;
    memset(&sa, 0, sizeof(sa));
    sa.salg_family = AF_ALG;
    strcpy((char *)sa.salg_type, "akcipher");

    /* Fallback chain: prefer pkcs1pad(rsa) sign, else raw rsa. */
    static const char *names[] = { "pkcs1pad(rsa)", "rsa" };
    int algo = -1;
    for (size_t a = 0; a < sizeof(names) / sizeof(names[0]); a++) {
        memset(sa.salg_name, 0, sizeof(sa.salg_name));
        strcpy((char *)sa.salg_name, names[a]);
        if (bind(tf, (struct sockaddr *)&sa, sizeof(sa)) == 0) { algo = (int)a; break; }
    }
    if (algo < 0) { perror("bind akcipher"); return 1; }
    if (setsockopt(tf, SOL_ALG, ALG_SET_KEY, key, klen) < 0) {
        perror("setkey");
        return 1;
    }

    unsigned char in[512];
    unsigned char out[1024];
    memset(in, 0x42, sizeof(in));
    size_t inlen = (algo == 0) ? 51 : 256; /* pkcs1pad: DigestInfo(19)+digest(32) ; raw rsa: modulus size */
    if (algo == 0) {
        /* SHA-256 DigestInfo prefix */
        static const unsigned char di_prefix[] = {
            0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
            0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
            0x00, 0x04, 0x20
        };
        memcpy(in, di_prefix, sizeof(di_prefix));
    }

    for (long i = 0; i < iters; i++) {
        int op = accept(tf, NULL, NULL);
        if (op < 0) { perror("accept"); return 1; }
        in[inlen - 1] = (unsigned char)i;
        if (write(op, in, inlen) != (ssize_t)inlen) {
            perror("write"); close(op); return 1;
        }
        ssize_t n = read(op, out, sizeof(out));
        if (n < 0) { perror("read"); close(op); return 1; }
        close(op);
    }
    close(tf);
    printf("OK %ld iters rsa-%s\n", iters, algo == 0 ? "pkcs1-sign" : "raw");
    return 0;
}
