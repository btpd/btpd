#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "btpd.h"
#include "hook.h"

void
hook_run(hook_t type, const char *tr_name)
{
    char st[2] = "U\0";
    char *args[4] = { (char *)hook_script, st, (char *)tr_name, NULL };
    pid_t pid;
    int status;

    *st = (char) type;

    if (!hook_script)
        return;

    pid = fork();

    switch (pid) {
        case -1: btpd_log(BTPD_L_BTPD, "Failed to fork to run hook"); break;
        case 0:
                 (void) execv(hook_script, args);
                 btpd_log(BTPD_L_BTPD, "Failed to starting hook: '%s' '%c' '%s'.\n",
                         hook_script, *st, tr_name);
                 break;
        default:
                 if (waitpid (pid, &status, 0) > 0)
                     btpd_log(BTPD_L_BTPD, "Hook successfully run: '%s' '%c' '%s'.\n",
                             hook_script, *st, tr_name);
    }
}

