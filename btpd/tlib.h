#ifndef BTPD_TLIB_H
#define BTPD_TLIB_H

struct tlib {
    unsigned num;
    uint8_t hash[20];
    struct torrent *tp;

    char *name;
    char *dir;

    unsigned long long tot_up, tot_down;
    off_t content_size, content_have;

    HTBL_ENTRY(nchain);
    HTBL_ENTRY(hchain);
};

struct file_time_size {
    off_t size;
    time_t mtime;
};

void tlib_init(void);
void tlib_put_all(struct tlib **v);

struct tlib *tlib_add(const uint8_t *hash, const char *mi, size_t mi_size,
    const char *content, char *name);
int tlib_del(struct tlib *tl);

void tlib_update_info(struct tlib *tl);

struct tlib *tlib_by_hash(const uint8_t *hash);
struct tlib *tlib_by_num(unsigned num);
unsigned tlib_count(void);

void tlib_read_hash(struct tlib *tl, size_t off, uint32_t piece,
    uint8_t *hash);

int tlib_load_resume(struct tlib *tl, unsigned nfiles,
    struct file_time_size *fts, size_t pfsize, uint8_t *pc_field,
    size_t bfsize, uint8_t *blk_field);

void tlib_save_resume(struct tlib *tl, unsigned nfiles,
    struct file_time_size *fts, size_t pfsize, uint8_t *pc_field,
    size_t bfsize, uint8_t *blk_field);

#endif
