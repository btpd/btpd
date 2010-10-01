#ifndef HOOK_H
#define HOOK_H

#define HOOK_STARTED 'S'
#define HOOK_STOPPED 'T'
#define HOOK_FINISHED 'F'

typedef char hook_t;

void
hook_run(hook_t type, const char *tr_name);

#endif
