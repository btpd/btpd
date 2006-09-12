#ifndef BTPD_METAINFO_H
#define BTPD_METAINFO_H

struct mi_file {
    char *path;
    off_t length;
};

struct mi_tier {
    int nurls;
    char **urls;
};

struct mi_announce {
    int ntiers;
    struct mi_tier *tiers;
};

char *mi_name(const char *p);
uint8_t *mi_info_hash(const char *p, uint8_t *hash);
uint8_t *mi_hashes(const char *p);
int mi_simple(const char *p);
size_t mi_npieces(const char *p);
off_t mi_total_length(const char *p);
off_t mi_piece_length(const char *p);

struct mi_announce *mi_announce(const char *p);
void mi_free_announce(struct mi_announce *ann);

size_t mi_nfiles(const char *p);
struct mi_file *mi_files(const char *p);
void mi_free_files(unsigned nfiles, struct mi_file *files);

int mi_test(const char *p, size_t size);
char *mi_load(const char *path, size_t *size);

#endif
