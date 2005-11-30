#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <sys/wait.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>

#include <curl/curl.h>

#include "btpd.h"
#include "tracker_req.h"

#ifndef PRIu64
#define PRIu64 "llu"
#endif

#define REQ_SIZE (getpagesize() * 2)

struct tracker_req {
    enum tr_event tr_event;
    uint8_t info_hash[20];
    struct io_buffer *res;
};

static void
maybe_connect_to(struct torrent *tp, const char *pinfo)
{
    const char *pid = NULL;
    char *ip = NULL;
    int64_t port;
    size_t len;

    if (!benc_isdct(pinfo))
        return;

    if (benc_dget_str(pinfo, "peer id", &pid, &len) != 0 || len != 20)
        return;

    if (bcmp(btpd_get_peer_id(), pid, 20) == 0)
        return;

    if (torrent_has_peer(tp, pid))
        return;

    if (benc_dget_strz(pinfo, "ip", &ip, NULL) != 0)
        goto out;

    if (benc_dget_int64(pinfo, "port", &port) != 0)
        goto out;

    peer_create_out(tp, pid, ip, port);

out:
    if (ip != NULL)
        free(ip);
}

static void
tracker_done(pid_t pid, void *arg)
{
    struct tracker_req *req = arg;
    int failed = 0;
    char *buf;
    const char *peers;
    uint32_t interval;
    struct torrent *tp;

    if ((tp = btpd_get_torrent(req->info_hash)) == NULL)
        goto out;

    if (benc_validate(req->res->buf, req->res->buf_off) != 0
        || !benc_isdct(req->res->buf)) {
        if (req->res->buf_off != 0) {
            fwrite(req->res->buf, 1, req->res->buf_off, (stdout));
            putchar('\n');
        }

        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        failed = 1;
        goto out;
    }

    if ((benc_dget_strz(req->res->buf, "failure reason", &buf, NULL)) == 0) {
        btpd_log(BTPD_L_ERROR, "Tracker failure: %s.\n", buf);
        free(buf);
        failed = 1;
        goto out;
    }

    if ((benc_dget_uint32(req->res->buf, "interval", &interval)) != 0) {
        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        failed = 1;
        goto out;
    }

    //tp->tracker_time = btpd_seconds + interval;

    int error = 0;
    size_t length;

    if ((error = benc_dget_lst(req->res->buf, "peers", &peers)) == 0) {
        for (peers = benc_first(peers);
             peers != NULL && net_npeers < net_max_peers;
             peers = benc_next(peers))
            maybe_connect_to(tp, peers);
    }

    if (error == EINVAL) {
        error = benc_dget_str(req->res->buf, "peers", &peers, &length);
        if (error == 0 && length % 6 == 0) {
            size_t i;
            for (i = 0; i < length && net_npeers < net_max_peers; i += 6)
                peer_create_out_compact(tp, peers + i);
        }
    }

    if (error != 0) {
        btpd_log(BTPD_L_ERROR, "Bad data from tracker.\n");
        failed = 1;
        goto out;
    }

out:
    if (failed) {
        if (req->tr_event == TR_STARTED) {
            btpd_log(BTPD_L_BTPD,
                "Start request failed for %s.\n", tp->relpath);
            torrent_unload(tp);
        } else
            ;//tp->tracker_time = btpd_seconds + 10;
    }
    munmap(req->res, REQ_SIZE);
    free(req);
}

static const char *
event2str(enum tr_event ev)
{
    switch (ev) {
    case TR_STARTED:
        return "started";
    case TR_STOPPED:
        return "stopped";
    case TR_COMPLETED:
        return "completed";
    case TR_EMPTY:
        return "";
    default:
        btpd_err("Bad tracker event %d.\n", ev);
        return ""; // Shut GCC up!
    }
}

static int
create_url(struct tracker_req *req, struct torrent *tp, char **url)
{
    char e_hash[61], e_id[61];
    const uint8_t *peer_id = btpd_get_peer_id();
    char qc;
    int i;
    uint64_t left;
    const char *event;

    event = event2str(req->tr_event);

    qc = (strchr(tp->meta.announce, '?') == NULL) ? '?' : '&';

    for (i = 0; i < 20; i++)
        snprintf(e_hash + i * 3, 4, "%%%.2x", tp->meta.info_hash[i]);

    for (i = 0; i < 20; i++)
        snprintf(e_id + i * 3, 4, "%%%.2x", peer_id[i]);

    left = torrent_bytes_left(tp);

    i = asprintf(url, "%s%cinfo_hash=%s"
                 "&peer_id=%s"
                 "&port=%d"
                 "&uploaded=%" PRIu64
                 "&downloaded=%" PRIu64
                 "&left=%" PRIu64
                 "&compact=1"
                 "%s%s",
                 tp->meta.announce, qc, e_hash, e_id, net_port,
                 tp->uploaded, tp->downloaded, left,
                 req->tr_event == TR_EMPTY ? "" : "&event=",
                 event);

    if (i < 0)
        return ENOMEM;
    return 0;
}

static size_t
http_cb(void *ptr, size_t size, size_t nmemb, void *stream)
{
    struct tracker_req *req = (struct tracker_req *)stream;
    size_t nbytes = size * nmemb;
    if (nbytes <=  req->res->buf_len - req->res->buf_off) {
        memcpy(req->res->buf + req->res->buf_off, ptr, nbytes);
        req->res->buf_off += nbytes;
        return nbytes;
    }
    else
        return 0;
}

static void
http_helper(struct tracker_req *req, struct torrent *tp)
{
    char cerror[CURL_ERROR_SIZE];
    char fr[] = "failure reason";
    CURL *handle;
    char *url;
    int err;

    if (create_url(req, tp, &url) != 0)
        goto memory_error;

    if (curl_global_init(0) != 0)
        goto libcurl_error;

    if ((handle = curl_easy_init()) == NULL)
        goto libcurl_error;

    err = curl_easy_setopt(handle, CURLOPT_URL, url);
    if (err == 0)
        err = curl_easy_setopt(handle, CURLOPT_USERAGENT, BTPD_VERSION);
    if (err == 0)
        err = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, http_cb);
    if (err == 0)
        err = curl_easy_setopt(handle, CURLOPT_WRITEDATA, req);
    if (err == 0)
        err = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, cerror);
    if (err != 0) {
        strncpy(cerror, curl_easy_strerror(err), CURL_ERROR_SIZE - 1);
        goto handle_error;
    }

    req->res->buf_off = 0;
    if (curl_easy_perform(handle) != 0)
        goto handle_error;

#if 0
    curl_easy_cleanup(handle);
    curl_global_cleanup();
    free(url);
#endif
    exit(0);

memory_error:
    strncpy(cerror, "Out of memory", CURL_ERROR_SIZE - 1);
    goto handle_error;

libcurl_error:
    strncpy(cerror, "Generic libcurl error", CURL_ERROR_SIZE - 1);
    goto handle_error;

handle_error:
    req->res->buf_off =
        snprintf(req->res->buf, req->res->buf_len,
            "d%d:%s%d:%se", (int)strlen(fr), fr, (int)strlen(cerror), cerror);
    if (req->res->buf_off >= req->res->buf_len)
        req->res->buf_off = 0;

    exit(1);
}

void
tracker_req(struct torrent *tp, enum tr_event tr_event)
{
    struct tracker_req *req;
    pid_t pid;

    btpd_log(BTPD_L_TRACKER,
        "request for %s, event: %s.\n",
        tp->relpath, event2str(tr_event));

    req = (struct tracker_req *)btpd_calloc(1, sizeof(*req));

    req->res = mmap(NULL, REQ_SIZE, PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED, -1, 0);

    if (req->res == MAP_FAILED)
        btpd_err("Failed mmap: %s\n", strerror(errno));

    req->res->buf_len = REQ_SIZE - sizeof(*req->res);
    req->res->buf_off = 0;
    req->res->buf = (char *)req->res + sizeof(*req->res);

    req->tr_event = tr_event;
    bcopy(tp->meta.info_hash, req->info_hash, 20);

    fflush(NULL);

    pid = fork();
    if (pid < 0) {
        btpd_err("Couldn't fork (%s).\n", strerror(errno));
    } else if (pid == 0) { // Child
        int nfiles = getdtablesize();
        for (int i = 0; i < nfiles; i++)
            close(i);
        http_helper(req, tp);
    } else
        btpd_add_child(pid, tracker_done, req);
}
