#include <math.h>
#include <string.h>

#include "btpd.h"

static void
kill_buf_no(char *buf, size_t len)
{

}

static void
kill_buf_free(char *buf, size_t len)
{
    free(buf);
}

static struct net_buf *
nb_create_alloc(short type, size_t len)
{
    struct net_buf *nb = btpd_calloc(1, sizeof(*nb) + len);
    nb->type = type;
    nb->buf = (char *)(nb + 1);
    nb->len = len;
    nb->kill_buf = kill_buf_no;
    return nb;
}

static struct net_buf *
nb_create_set(short type, char *buf, size_t len,
    void (*kill_buf)(char *, size_t))
{
    struct net_buf *nb = btpd_calloc(1, sizeof(*nb));
    nb->type = type;
    nb->buf = buf;
    nb->len = len;
    nb->kill_buf = kill_buf;
    return nb;
}

static struct net_buf *
nb_create_onesized(char mtype, int btype)
{
    struct net_buf *out = nb_create_alloc(btype, 5);
    net_write32(out->buf, 1);
    out->buf[4] = mtype;
    return out;
}

struct net_buf *
nb_create_piece(uint32_t index, uint32_t begin, size_t blen)
{
    struct net_buf *out;

    btpd_log(BTPD_L_MSG, "send piece: %u, %u, %u\n", index, begin, blen);

    out = nb_create_alloc(NB_PIECE, 13);
    net_write32(out->buf, 9 + blen);
    out->buf[4] = MSG_PIECE;
    net_write32(out->buf + 5, index);
    net_write32(out->buf + 9, begin);
    return out;
}

struct net_buf *
nb_create_torrentdata(char *block, size_t blen)
{
    struct net_buf *out;
    out = nb_create_set(NB_TORRENTDATA, block, blen, kill_buf_free);
    return out;
}

struct net_buf *
nb_create_request(uint32_t index, uint32_t begin, uint32_t length)
{
    struct net_buf *out = nb_create_alloc(NB_REQUEST, 17);
    net_write32(out->buf, 13);
    out->buf[4] = MSG_REQUEST;
    net_write32(out->buf + 5, index);
    net_write32(out->buf + 9, begin);
    net_write32(out->buf + 13, length);
    return out;
}

struct net_buf *
nb_create_cancel(uint32_t index, uint32_t begin, uint32_t length)
{
    struct net_buf *out = nb_create_alloc(NB_CANCEL, 17);
    net_write32(out->buf, 13);
    out->buf[4] = MSG_CANCEL;
    net_write32(out->buf + 5, index);
    net_write32(out->buf + 9, begin);
    net_write32(out->buf + 13, length);
    return out;
}

struct net_buf *
nb_create_have(uint32_t index)
{
    struct net_buf *out = nb_create_alloc(NB_HAVE, 9);
    net_write32(out->buf, 5);
    out->buf[4] = MSG_HAVE;
    net_write32(out->buf + 5, index);
    return out;
}

struct net_buf *
nb_create_multihave(struct torrent *tp)
{
    struct net_buf *out = nb_create_alloc(NB_MULTIHAVE, 9 * tp->have_npieces);
    for (uint32_t i = 0, count = 0; count < tp->have_npieces; i++) {
	if (has_bit(tp->piece_field, i)) {
	    net_write32(out->buf + count * 9, 5);
	    out->buf[count * 9 + 4] = MSG_HAVE;
	    net_write32(out->buf + count * 9 + 5, i);
	    count++;
	}
    }
    return out;
}

struct net_buf *
nb_create_unchoke(void)
{
    return nb_create_onesized(MSG_UNCHOKE, NB_UNCHOKE);
}

struct net_buf *
nb_create_choke(void)
{
    return nb_create_onesized(MSG_CHOKE, NB_CHOKE);
}

struct net_buf *
nb_create_uninterest(void)
{
    return nb_create_onesized(MSG_UNINTEREST, NB_UNINTEREST);
}

struct net_buf *
nb_create_interest(void)
{
    return nb_create_onesized(MSG_INTEREST, NB_INTEREST);
}

struct net_buf *
nb_create_bitfield(struct torrent *tp)
{
    uint32_t plen = ceil(tp->meta.npieces / 8.0);

    struct net_buf *out = nb_create_alloc(NB_BITFIELD, 5);
    net_write32(out->buf, plen + 1);
    out->buf[4] = MSG_BITFIELD;
    return out;
}

struct net_buf *
nb_create_bitdata(struct torrent *tp)
{
    uint32_t plen = ceil(tp->meta.npieces / 8.0);
    struct net_buf *out =
	nb_create_set(NB_BITDATA, tp->piece_field, plen, kill_buf_no);
    return out;
}

struct net_buf *
nb_create_shake(struct torrent *tp)
{
    struct net_buf *out = nb_create_alloc(NB_SHAKE, 68);
    bcopy("\x13""BitTorrent protocol\0\0\0\0\0\0\0\0", out->buf, 28);
    bcopy(tp->meta.info_hash, out->buf + 28, 20);
    bcopy(btpd.peer_id, out->buf + 48, 20);
    return out;
}

uint32_t
nb_get_index(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_HAVE:
    case NB_PIECE:
    case NB_REQUEST:
	return net_read32(nb->buf + 5);
    default:
	abort();
    }
}

uint32_t
nb_get_begin(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_PIECE:
    case NB_REQUEST:
	return net_read32(nb->buf + 9);
    default:
	abort();
    }
}

uint32_t
nb_get_length(struct net_buf *nb)
{
    switch (nb->type) {
    case NB_CANCEL:
    case NB_REQUEST:
	return net_read32(nb->buf + 13);
    case NB_PIECE:
	return net_read32(nb->buf) - 9;
    default:
	abort();
    }
}

int
nb_drop(struct net_buf *nb)
{
    assert(nb->refs > 0);
    nb->refs--;
    if (nb->refs == 0) {
	nb->kill_buf(nb->buf, nb->len);
	free(nb);
	return 1;
    } else
	return 0;
}

void
nb_hold(struct net_buf *nb)
{
    nb->refs++;
}
