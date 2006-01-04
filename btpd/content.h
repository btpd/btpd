#ifndef BTPD_CONTENT_H
#define BTPD_CONTENT_H

int cm_start(struct torrent *tp);
void cm_stop(struct torrent * tp);

int cm_full(struct torrent *tp);

uint8_t *cm_get_piece_field(struct torrent *tp);
uint8_t *cm_get_block_field(struct torrent *tp, uint32_t piece);

uint32_t cm_get_npieces(struct torrent *tp);

int cm_has_piece(struct torrent *tp, uint32_t piece);

int cm_put_block(struct torrent *tp, uint32_t piece, uint32_t block,
    const char *buf);
int cm_get_bytes(struct torrent *tp, uint32_t piece, uint32_t begin,
    size_t len, char **buf);

void cm_test_piece(struct piece *pc);

off_t cm_bytes_left(struct torrent *tp);

#endif
