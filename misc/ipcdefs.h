#ifndef ERRDEF
#define __IPCE
#define ERRDEF(name, msg)
#endif
ERRDEF(OK,              "no error")
ERRDEF(COMMERR,         "communication error")
ERRDEF(EBADCDIR,        "bad content directory")
ERRDEF(EBADT,           "bad torrent")
ERRDEF(EBADTENT,        "bad torrent entry")
ERRDEF(EBADTRACKER,     "bad tracker")
ERRDEF(ECREATECDIR,     "couldn't create content directory")
ERRDEF(ENOKEY,          "no such key")
ERRDEF(ENOTENT,         "no such torrent entry")
ERRDEF(ESHUTDOWN,       "btpd is shutting down")
ERRDEF(ETACTIVE,        "torrent is active")
ERRDEF(ETENTEXIST,      "torrent entry exists")
ERRDEF(ETINACTIVE,      "torrent is inactive")
#ifdef __IPCE
#undef __IPCE
#undef ERRDEF
#endif
#ifndef TVDEF
#define __IPCTV
#define TVDEF(val, type, name)
#endif
TVDEF(CGOT,     NUM,            "content_got")
TVDEF(CSIZE,    NUM,            "content_size")
TVDEF(DIR,      PATH,           "dir")
TVDEF(NAME,     STR,            "name")
TVDEF(NUM,      NUM,            "num")
TVDEF(IHASH,    BIN,            "info_hash")
TVDEF(PCGOT,    NUM,            "pieces_got")
TVDEF(PCOUNT,   NUM,            "peer_count")
TVDEF(PCCOUNT,  NUM,            "piece_count")
TVDEF(PCSEEN,   NUM,            "pieces_seen")
TVDEF(RATEDWN,  NUM,            "rate_down")
TVDEF(RATEUP,   NUM,            "rate_up")
TVDEF(SESSDWN,  NUM,            "sess_down")
TVDEF(SESSUP,   NUM,            "sess_up")
TVDEF(STATE,    TSTATE,         "state")
TVDEF(TOTDWN,   NUM,            "total_down")
TVDEF(TOTUP,    NUM,            "total_up")
TVDEF(TRERR,    NUM,            "tr_errors")
TVDEF(TRGOOD,   NUM,            "tr_good")
TVDEF(LABEL,    STR,            "label")
#ifdef __IPCTV
#undef __IPCTV
#undef TVDEF
#endif
