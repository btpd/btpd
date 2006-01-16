#ifndef BTPD_CONTENT_H
#define BTPD_CONTENT_H

void cm_init(void);

int cm_start(struct torrent *tp);
void cm_stop(struct torrent * tp);

int cm_full(struct torrent *tp);

uint8_t *cm_get_piece_field(struct torrent *tp);
uint8_t *cm_get_block_field(struct torrent *tp, uint32_t piece);

uint32_t cm_get_npieces(struct torrent *tp);

int cm_has_piece(struct torrent *tp, uint32_t piece);

int cm_put_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    const uint8_t *buf, size_t len);
int cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    size_t len, uint8_t **buf);

void cm_prealloc(struct torrent *tp, uint32_t piece);
void cm_test_piece(struct torrent *tp, uint32_t piece);

off_t cm_get_size(struct torrent *tp);

#endif
