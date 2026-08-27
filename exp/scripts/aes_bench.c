/* aes_bench.c - AF_ALG AES-256-CBC benchmark over virtio-crypto (SVM).
 *
 * Usage: ./aes_bench <blocksize> <iters> <threads> [warmup_ops]
 *
 * Each thread opens its own AF_ALG skcipher socket ("cbc(aes)", 256-bit
 * key), then loops: set IV (sendmsg ALG_SET_IV) -> write(data) -> read(out).
 * One op == encrypt one block. Reports throughput + per-op latency percentiles.
 * In SVM mode every op is forwarded local QEMU -> remote stub (cryptodev).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <linux/if_alg.h>

#ifndef AF_ALG
#define AF_ALG 38
#endif

typedef struct {
    int blocksize;
    long iters;
    long warmup;
    long *lats;          /* per-op latency, ns */
} job_t;

static int sock_skcipher(void)
{
    int tf = socket(AF_ALG, SOCK_SEQPACKET, 0);
    if (tf < 0) return -1;
    struct sockaddr_alg sa;
    memset(&sa, 0, sizeof(sa));
    sa.salg_family = AF_ALG;
    strcpy((char *)sa.salg_type, "skcipher");
    strcpy((char *)sa.salg_name, "cbc(aes)");
    if (bind(tf, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(tf); return -1; }
    unsigned char key[32];
    memset(key, 0x11, sizeof(key));
    if (setsockopt(tf, SOL_ALG, ALG_SET_KEY, key, sizeof(key)) < 0) { close(tf); return -1; }
    return tf;
}

/* One encrypt op: single sendmsg whose control carries ALG_SET_IV and whose
 * iov carries the plaintext. ALG_SET_OP is deliberately omitted (skcipher
 * defaults to encrypt). */
/* Kernel 7.0.0-30 af_alg_sendmsg() ALG_SET_IV cmsg payload = u32 ivlen (byte
 * count of the IV, 16 for AES-CBC) followed by the IV bytes; the handler also
 * requires cmsg_len >= CMSG_LEN(4 + ivlen). struct af_alg_iv comes from
 * <linux/if_alg.h> (flexible iv[]). */
static ssize_t do_op(int op, unsigned char *in, size_t bs, unsigned char *out)
{
    unsigned char iv[16];
    memset(iv, 0x22, sizeof(iv));
    size_t plen = sizeof(struct af_alg_iv) + sizeof(iv);
    unsigned char pl[sizeof(struct af_alg_iv) + 16];
    ((struct af_alg_iv *)pl)->ivlen = sizeof(iv); /* byte length of IV (16) */
    memcpy(pl + sizeof(struct af_alg_iv), iv, sizeof(iv));
    char cbuf[CMSG_SPACE(plen)];
    memset(cbuf, 0, sizeof(cbuf));
    struct iovec iov = { .iov_base = in, .iov_len = bs };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_ALG;
    c->cmsg_type = ALG_SET_IV;
    c->cmsg_len = CMSG_LEN(plen);
    memcpy(CMSG_DATA(c), pl, plen);
    ssize_t s = sendmsg(op, &msg, 0);
    if (s != (ssize_t)bs) {
        fprintf(stderr, "sendmsg=%zd errno=%d (%s)\n", s, errno, strerror(errno));
        return -1;
    }
    ssize_t r = read(op, out, bs);
    if (r < 0) fprintf(stderr, "read errno=%d (%s)\n", errno, strerror(errno));
    return r;
}

static inline long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void *worker(void *arg)
{
    job_t *j = (job_t *)arg;
    int bs = j->blocksize;
    long total = j->iters + j->warmup;

    int tf = sock_skcipher();
    if (tf < 0) { perror("socket"); return NULL; }
    int op = accept(tf, NULL, NULL);
    if (op < 0) { perror("accept"); return NULL; }

    unsigned char *in = malloc(bs);
    unsigned char *out = malloc(bs + 4096);
    if (!in || !out) { perror("malloc"); return NULL; }
    memset(in, 0x33, bs);

    for (long i = 0; i < total; i++) {
        long t0 = now_ns();
        ssize_t n = do_op(op, in, bs, out);
        long t1 = now_ns();
        if (n < 0) { perror("op"); break; }
        if (i >= j->warmup) {
            j->lats[i - j->warmup] = t1 - t0;
        }
    }
    close(op);
    close(tf);
    free(in);
    free(out);
    return NULL;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <blocksize> <iters> <threads> [warmup_ops]\n", argv[0]);
        return 1;
    }
    int bs = atoi(argv[1]);
    long iters = atol(argv[2]);
    int threads = atoi(argv[3]);
    long warmup = (argc > 4) ? atol(argv[4]) : 8;
    if (bs <= 0 || iters <= 0 || threads <= 0) return 1;

    job_t *jobs = calloc(threads, sizeof(job_t));
    pthread_t *tid = calloc(threads, sizeof(pthread_t));
    for (int t = 0; t < threads; t++) {
        jobs[t].blocksize = bs;
        jobs[t].iters = iters;
        jobs[t].warmup = warmup;
        jobs[t].lats = malloc(sizeof(long) * iters);
    }

    long wall0 = now_ns();
    for (int t = 0; t < threads; t++) pthread_create(&tid[t], NULL, worker, &jobs[t]);
    for (int t = 0; t < threads; t++) pthread_join(tid[t], NULL);
    long wall1 = now_ns();

    /* merge latencies */
    long total = iters * threads;
    long *all = malloc(sizeof(long) * total);
    for (int t = 0; t < threads; t++) memcpy(all + t * iters, jobs[t].lats, sizeof(long) * iters);
    qsort(all, total, sizeof(long), cmp_long);

    long sum = 0;
    for (long i = 0; i < total; i++) sum += all[i];
    double mean_us = (double)sum / total / 1000.0;
    double p50 = all[total * 50 / 100] / 1000.0;
    double p90 = all[total * 90 / 100] / 1000.0;
    double p99 = all[total * 99 / 100] / 1000.0;
    double p999 = all[total * 999 / 1000] / 1000.0;
    double max_us = all[total - 1] / 1000.0;
    double wall_s = (double)(wall1 - wall0) / 1e9;
    long ops = total;
    double mbps = (double)ops * bs / 1048576.0 / wall_s;
    double ops_s = (double)ops / wall_s;

    printf("RESULT bs=%d threads=%d ops=%ld wall_s=%.3f MBps=%.2f ops/s=%.0f "
           "mean_us=%.2f p50=%.2f p90=%.2f p99=%.2f p999=%.2f max_us=%.2f\n",
           bs, threads, ops, wall_s, mbps, ops_s, mean_us, p50, p90, p99, p999, max_us);
    return 0;
}
