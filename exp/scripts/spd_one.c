/* spd_one.c - single-chunksize bounded cryptodev AES-128-CBC test.
 * usage: spd_one <chunksize> <nops>
 * Runs nops encrypt ops of given chunksize via /dev/crypto (CIOCCRYPT).
 * Prints "OK: ... done" on success, error message on failure.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <crypto/cryptodev.h>

#define KEY_SIZE 16

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <chunksize> <nops>\n", argv[0]);
		return 2;
	}
	int chunksize = atoi(argv[1]);
	long nops = atol(argv[2]);
	if (chunksize <= 0 || nops <= 0) {
		fprintf(stderr, "bad args\n");
		return 2;
	}

	int cfd = open("/dev/crypto", O_RDWR);
	if (cfd < 0) { perror("open /dev/crypto"); return 1; }

	unsigned char key[KEY_SIZE];
	memset(key, 0x23, KEY_SIZE);

	struct session_op sess;
	memset(&sess, 0, sizeof(sess));
	sess.cipher = CRYPTO_AES_CBC;
	sess.keylen = KEY_SIZE;
	sess.key = key;
	if (ioctl(cfd, CIOCGSESSION, &sess)) { perror("CIOCGSESSION"); return 1; }

	unsigned char iv[32];
	memset(iv, 0x42, sizeof(iv));
	char *buf = malloc(chunksize);
	if (!buf) { perror("malloc"); return 1; }
	memset(buf, 0x33, chunksize);

	struct crypt_op cop;
	for (long i = 0; i < nops; i++) {
		memset(&cop, 0, sizeof(cop));
		cop.ses = sess.ses;
		cop.op = COP_ENCRYPT;
		cop.len = chunksize;
		cop.iv = iv;
		cop.src = cop.dst = (unsigned char *)buf;
		if (ioctl(cfd, CIOCCRYPT, &cop)) {
			fprintf(stderr, "ioctl(CIOCCRYPT) at op %ld: %s\n", i, strerror(errno));
			return 1;
		}
	}
	printf("OK: %d bytes x %ld ops done\n", chunksize, nops);

	ioctl(cfd, CIOCFSESSION, &sess.ses);
	close(cfd);
	free(buf);
	return 0;
}
