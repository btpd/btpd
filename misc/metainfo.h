#ifndef BTPD_METAINFO_H
#define BTPD_METAINFO_H

struct fileinfo {
    char *path;
    off_t length;
};

struct metainfo {
    char *name;
    char *announce;
    uint8_t info_hash[20];
    uint8_t (*piece_hash)[20];
    unsigned pieces_off;
    uint32_t npieces;
    off_t piece_length;
    off_t total_length;
    unsigned nfiles;
    struct fileinfo *files;
};

int fill_fileinfo(const char *fdct, struct fileinfo *fip);
int fill_metainfo(const char *base, struct metainfo *mip, int mem_hashes);
void clear_metainfo(struct metainfo *mip);
void print_metainfo(struct metainfo *mip);
int load_metainfo(const char *path, off_t size, int mem_hashes, struct metainfo **res);

#endif
