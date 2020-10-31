/* public domain sha1 implementation based on rfc3174 and libtomcrypt */
#include <stdint.h>
#include <string.h>

#include "sha1.h"

static uint32_t rol(uint32_t n, int k) { return (n << k) | (n >> (32-k)); }
#define F0(b,c,d) (d ^ (b & (c ^ d)))
#define F1(b,c,d) (b ^ c ^ d)
#define F2(b,c,d) ((b & c) | (d & (b | c)))
#define F3(b,c,d) (b ^ c ^ d)
#define G0(a,b,c,d,e,i) e += rol(a,5)+F0(b,c,d)+W[i]+0x5A827999; b = rol(b,30)
#define G1(a,b,c,d,e,i) e += rol(a,5)+F1(b,c,d)+W[i]+0x6ED9EBA1; b = rol(b,30)
#define G2(a,b,c,d,e,i) e += rol(a,5)+F2(b,c,d)+W[i]+0x8F1BBCDC; b = rol(b,30)
#define G3(a,b,c,d,e,i) e += rol(a,5)+F3(b,c,d)+W[i]+0xCA62C1D6; b = rol(b,30)

static void
processblock(struct sha1 *s, const uint8_t *buf)
{
	uint32_t W[80], a, b, c, d, e;
	int i;

	for (i = 0; i < 16; i++) {
		W[i] = (uint32_t)buf[4*i]<<24;
		W[i] |= (uint32_t)buf[4*i+1]<<16;
		W[i] |= (uint32_t)buf[4*i+2]<<8;
		W[i] |= buf[4*i+3];
	}
	for (; i < 80; i++)
		W[i] = rol(W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16], 1);
	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	for (i = 0; i < 20; ) {
		G0(a,b,c,d,e,i++);
		G0(e,a,b,c,d,i++);
		G0(d,e,a,b,c,i++);
		G0(c,d,e,a,b,i++);
		G0(b,c,d,e,a,i++);
	}
	while (i < 40) {
		G1(a,b,c,d,e,i++);
		G1(e,a,b,c,d,i++);
		G1(d,e,a,b,c,i++);
		G1(c,d,e,a,b,i++);
		G1(b,c,d,e,a,i++);
	}
	while (i < 60) {
		G2(a,b,c,d,e,i++);
		G2(e,a,b,c,d,i++);
		G2(d,e,a,b,c,i++);
		G2(c,d,e,a,b,i++);
		G2(b,c,d,e,a,i++);
	}
	while (i < 80) {
		G3(a,b,c,d,e,i++);
		G3(e,a,b,c,d,i++);
		G3(d,e,a,b,c,i++);
		G3(c,d,e,a,b,i++);
		G3(b,c,d,e,a,i++);
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
}

static void
pad(struct sha1 *s)
{
	unsigned r = s->len % 64;

	s->buf[r++] = 0x80;
	if (r > 56) {
		memset(s->buf + r, 0, 64 - r);
		r = 0;
		processblock(s, s->buf);
	}
	memset(s->buf + r, 0, 56 - r);
	s->len *= 8;
	s->buf[56] = s->len >> 56;
	s->buf[57] = s->len >> 48;
	s->buf[58] = s->len >> 40;
	s->buf[59] = s->len >> 32;
	s->buf[60] = s->len >> 24;
	s->buf[61] = s->len >> 16;
	s->buf[62] = s->len >> 8;
	s->buf[63] = s->len;
	processblock(s, s->buf);
}

void
sha1_init(void *ctx)
{
	struct sha1 *s = ctx;

	s->len = 0;
	s->h[0] = 0x67452301;
	s->h[1] = 0xEFCDAB89;
	s->h[2] = 0x98BADCFE;
	s->h[3] = 0x10325476;
	s->h[4] = 0xC3D2E1F0;
}

void
sha1_sum(void *ctx, uint8_t md[SHA1_DIGEST_LENGTH])
{
	struct sha1 *s = ctx;
	int i;

	pad(s);
	for (i = 0; i < 5; i++) {
		md[4*i] = s->h[i] >> 24;
		md[4*i+1] = s->h[i] >> 16;
		md[4*i+2] = s->h[i] >> 8;
		md[4*i+3] = s->h[i];
	}
}

void
sha1_update(void *ctx, const void *m, unsigned long len)
{
	struct sha1 *s = ctx;
	const uint8_t *p = m;
	unsigned r = s->len % 64;

	s->len += len;
	if (r) {
		if (len < 64 - r) {
			memcpy(s->buf + r, p, len);
			return;
		}
		memcpy(s->buf + r, p, 64 - r);
		len -= 64 - r;
		p += 64 - r;
		processblock(s, s->buf);
	}
	for (; len >= 64; len -= 64, p += 64)
		processblock(s, p);
	memcpy(s->buf, p, len);
}

uint8_t *
quicksha1(const void *m, unsigned long len, uint8_t md[SHA1_DIGEST_LENGTH])
{
    struct sha1 ctx;

    if (m == NULL || md == NULL)
        return NULL;

    sha1_init(&ctx);
    sha1_update(&ctx, m, len);
    sha1_sum(&ctx, md);

    return md;
}
