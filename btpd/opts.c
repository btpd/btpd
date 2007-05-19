#include <btpd.h>
#include <netinet/in.h>

const char *btpd_dir;
#ifdef DEBUG
uint32_t btpd_logmask = BTPD_L_ALL;
#else
uint32_t btpd_logmask =  BTPD_L_BTPD | BTPD_L_ERROR;
#endif
int net_max_uploads = -2;
unsigned net_max_peers;
unsigned net_bw_limit_in;
unsigned net_bw_limit_out;
int net_port = 6881;
off_t cm_alloc_size = 2048 * 1024;
int ipcprot = 0600;
int empty_start = 0;
uint32_t tr_ip_arg = INADDR_ANY;
