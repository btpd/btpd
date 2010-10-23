#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

#include "btpd.h"
#include "hook.h"

struct hook_item_s {
    pid_t pid;
    char st;
    const char *tr_name;
    BTPDQ_ENTRY(hook_item_s) entry;
};

struct hook_list_s {
    int count;
    BTPDQ_HEAD(item_tq, hook_item_s) head;
};

static struct hook_list_s hook_list;
static pthread_mutex_t hook_lock;

void
hook_handler(int __attribute__((unused)) signal)
{
    pid_t pid;
    int status;
    struct hook_item_s *p;

    while ((pid = waitpid (-1, &status, WNOHANG)) > 0)
    {
        pthread_mutex_lock(&hook_lock);
        BTPDQ_FOREACH(p, &hook_list.head, entry)
            if (p->pid == pid) {
                BTPDQ_REMOVE(&hook_list.head, p, entry);
                btpd_log(BTPD_L_BTPD, "Hook script '%s' for status '%c' "
                        "and torrent '%s' finished with status '%d'.\n",
                        hook_script, p->st, p->tr_name, status);
            }
        pthread_mutex_unlock(&hook_lock);

    } /* while (waitpid) */
}

void
hook_shutdown(void)
{
    struct hook_item_s *p;

    pthread_mutex_lock(&hook_lock);

    BTPDQ_FOREACH(p, &hook_list.head, entry)
    {
        btpd_log(BTPD_L_BTPD, "Sending INT signal to hook process %d...\n", p->pid);
        kill(p->pid, SIGINT);
    }
    pthread_mutex_unlock(&hook_lock);
}

void
hook_init(void)
{
    struct sigaction sa;
    bzero(&sa, sizeof(sa));
    sa.sa_handler = hook_handler;
    sigaction (SIGCHLD, &sa, NULL);
    BTPDQ_INIT(&hook_list.head);
}

void
hook_exec(hook_t type, const char *tr_name)
{
    char st[2] = "U\0";
    char *args[4] = { (char *)hook_script, st, (char *)tr_name, NULL };
    pid_t pid;
    struct hook_item_s *p;

    *st = (char) type;

    if (!hook_script || btpd_is_stopping())
        return;

    pid = fork();

    switch (pid) {
        case -1: btpd_log(BTPD_L_BTPD, "Failed to fork to run hook"); break;
        case 0:
                 /* Parent execution */
                 (void) execv(hook_script, args);
                 btpd_log(BTPD_L_BTPD, "Failed to starting hook: '%s' '%c' '%s'.\n",
                         hook_script, *st, tr_name);
                 break;
        default:
                 /* Child execution */
                 p = calloc(1, sizeof(struct hook_item_s));
                 p->pid = pid;
                 p->st  = *st;
                 p->tr_name = tr_name;
                 pthread_mutex_lock(&hook_lock);
                 hook_list.count++;
                 BTPDQ_INSERT_TAIL(&hook_list.head, p, entry);
                 pthread_mutex_unlock(&hook_lock);
    }
}

