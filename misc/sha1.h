/* public domain sha1 implementation based on rfc3174 and libtomcrypt */

struct sha1 {
	uint64_t len;    /* processed message length */
	uint32_t h[5];   /* hash state */
	uint8_t buf[64]; /* message block buffer */
};

enum { SHA1_DIGEST_LENGTH = 20 };

/* reset state */
void sha1_init(void *ctx);
/* process message */
void sha1_update(void *ctx, const void *m, unsigned long len);
/* get message digest */
/* state is ruined after sum, keep a copy if multiple sum is needed */
/* part of the message might be left in s, zero it if secrecy is needed */
void sha1_sum(void *ctx, uint8_t md[SHA1_DIGEST_LENGTH]);
/* single-call version */
uint8_t *quicksha1(const void *m, unsigned long len, uint8_t md[SHA1_DIGEST_LENGTH]);
