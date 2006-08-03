#ifndef BTPD_ACTIVE_H
#define BTPD_ACTIVE_H

void active_add(const uint8_t *hash);
void active_del(const uint8_t *hash);
void active_clear(void);
void active_start(void);

#endif
