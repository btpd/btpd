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

void tlib_update_info(struct tlib *tl, int only_file);

struct tlib *tlib_by_hash(const uint8_t *hash);
struct tlib *tlib_by_num(unsigned num);
unsigned tlib_count(void);

int tlib_load_mi(struct tlib *tl, char **res);

void tlib_read_hash(struct tlib *tl, size_t off, uint32_t piece,
    uint8_t *hash);

struct resume_data *tlib_open_resume(struct tlib *tl, unsigned nfiles,
    size_t pfsize, size_t bfsize);
void tlib_close_resume(struct resume_data *resume);

uint8_t *resume_piece_field(struct resume_data *resd);
uint8_t *resume_block_field(struct resume_data *resd);
void resume_set_fts(struct resume_data *resd, int i,
    struct file_time_size *fts);
void resume_get_fts(struct resume_data *resd, int i,
    struct file_time_size *fts);

#endif
