#include <btpd.h>

short btpd_daemon = 1;
const char *btpd_dir;
#ifdef DEBUG
uint32_t btpd_logmask = BTPD_L_ALL;
#else
uint32_t btpd_logmask =  BTPD_L_BTPD | BTPD_L_ERROR;
#endif
unsigned net_max_peers;
unsigned net_bw_limit_in;
unsigned net_bw_limit_out;
int net_port = 6881;
