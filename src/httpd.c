/*
 * foal-httpd — minimal HTTP/1.0 server for SymbOS
 *
 * All files in the document root are loaded into RAM at startup.
 * Requests are served entirely from memory — no disk I/O at serve time.
 * One connection at a time; HTTP/1.0 with Connection: close.
 *
 * Usage:  httpd [port [docroot]]
 *   port    — TCP port (default 80)
 *   docroot — filesystem path to serve files from (default: app directory)
 *
 * Compile: cc src/httpd.c -lnet -N "foal-httpd" -o build/httpd.com
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <network.h>
#include <symbos.h>
#include <symbos/proc.h>
#include <symbos/systray.h>

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

#define HTTP_PORT     80
#define REQ_BUF_SIZE  512
#define FILE_BUF_SIZE 512
#define PATH_SIZE     128
#define FSPATH_SIZE   200
#define STORE_SIZE    8192   /* bytes available for all cached file content */
#define MAX_FILES     16     /* max files in the in-memory store */

/* ------------------------------------------------------------------ */
/* In-memory file store                                                */
/* ------------------------------------------------------------------ */

static char file_store[STORE_SIZE];
static unsigned short store_used;

static struct {
    char path[FSPATH_SIZE];   /* full SymbOS path — lookup key */
    unsigned short offset;    /* byte offset into file_store */
    unsigned short size;      /* content length in bytes */
} g_files[MAX_FILES];
static unsigned char g_nfiles;

/* ------------------------------------------------------------------ */
/* Globals  (kept in data segment to spare the Z80 stack)             */
/* ------------------------------------------------------------------ */

static char req_buf[REQ_BUF_SIZE + 1];
static char file_buf[FILE_BUF_SIZE];
static char g_method[8];
static char g_urlpath[PATH_SIZE];
static char g_fspath[FSPATH_SIZE];
static char g_docroot[PATH_SIZE];
static char g_dirpath[FSPATH_SIZE];  /* wildcard scan path: docroot\*.* */
static unsigned short g_port;
static unsigned char g_keep_fh;
static signed char g_tray_id = -1;

/* 4-colour 8x8 systray icon: {mode=2, w=8, h=8, 16 bytes CPC mode-1 pixels} */
static const char g_tray_icon[19] = {
    2, 8, 8,
    '\xf0','\x0f','\xf6','\x0f','\xf0','\x5d','\x4f','\xaf',
    '\x5f','\x2f','\xab','\xf0','\x0f','\xf6','\x0f','\xf0'
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *mime_type(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    e++;
    if (!strcmp(e, "html") || !strcmp(e, "htm") ||
        !strcmp(e, "HTML") || !strcmp(e, "HTM")) return "text/html";
    if (!strcmp(e, "txt")  || !strcmp(e, "TXT"))  return "text/plain";
    if (!strcmp(e, "css")  || !strcmp(e, "CSS"))  return "text/css";
    if (!strcmp(e, "js")   || !strcmp(e, "JS"))   return "application/javascript";
    if (!strcmp(e, "png")  || !strcmp(e, "PNG"))  return "image/png";
    if (!strcmp(e, "gif")  || !strcmp(e, "GIF"))  return "image/gif";
    if (!strcmp(e, "jpg")  || !strcmp(e, "jpeg") ||
        !strcmp(e, "JPG")  || !strcmp(e, "JPEG")) return "image/jpeg";
    return "application/octet-stream";
}

static int path_safe(const char *p)
{
    while (*p) {
        if (p[0] == '.' && p[1] == '.') return 0;
        p++;
    }
    return 1;
}

/* Build absolute SymbOS filesystem path from URL path into g_fspath. */
static void make_fspath(const char *url_path)
{
    char *p = (char *)url_path;
    if (*p == '/') p++;
    Dir_PathAdd(g_docroot[0] ? g_docroot : 0, p, g_fspath);
}

/* Case-insensitive path comparison (FAT names may be uppercase). */
static int path_cmp(const char *a, const char *b)
{
    unsigned char ca, cb;
    while (*a && *b) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 1;
    }
    return (*a || *b) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* HTTP responses                                                       */
/* ------------------------------------------------------------------ */

static void resp_400(signed char sock)
{
    strcpy(file_buf, "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n");
    TCP_Send(sock, _symbank, file_buf, (unsigned short)strlen(file_buf));
}

static void resp_404(signed char sock)
{
    static const char body[] =
        "<html><body><h1>404 Not Found</h1></body></html>\r\n";
    sprintf(file_buf,
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        (unsigned int)strlen(body));
    TCP_Send(sock, _symbank, file_buf, (unsigned short)strlen(file_buf));
    strcpy(file_buf, body);
    TCP_Send(sock, _symbank, file_buf, (unsigned short)strlen(file_buf));
}

static void resp_501(signed char sock)
{
    strcpy(file_buf, "HTTP/1.0 501 Not Implemented\r\nConnection: close\r\n\r\n");
    TCP_Send(sock, _symbank, file_buf, (unsigned short)strlen(file_buf));
}

/* ------------------------------------------------------------------ */
/* Startup: load all files from docroot into RAM                       */
/* ------------------------------------------------------------------ */

static void load_directory(void)
{
    int count;
    int i;
    unsigned char fattrib;
    char *p;
    char *fname;
    unsigned short namelen;
    unsigned char fh;
    unsigned short n;
    unsigned short chunk;
    unsigned short room;
    unsigned char ci;
    unsigned short dlen;

    /* Build wildcard path: docroot\*.* (or app dir\*.* if no docroot) */
    if (g_docroot[0]) {
        strncpy(g_dirpath, g_docroot, FSPATH_SIZE - 5);
        g_dirpath[FSPATH_SIZE - 5] = '\0';
        dlen = (unsigned short)strlen(g_dirpath);
        if (dlen > 0 && g_dirpath[dlen - 1] == '\\') g_dirpath[dlen - 1] = '\0';
        strcat(g_dirpath, "\\*.*");
    } else {
        Dir_PathAdd(0, "*.*", g_dirpath);
    }

    printf("Scan %s\r\n", g_dirpath);

    count = Dir_ReadRaw(_symbank, g_dirpath,
                        ATTRIB_DIR | ATTRIB_VOLUME,
                        _symbank, req_buf, REQ_BUF_SIZE, 0);

    printf("Dir: count=%d err=%u\r\n", count, (unsigned int)_fileerr);

    if (count <= 0) {
        printf("Store: 0 bytes, 0 files\r\n");
        return;
    }

    p = req_buf;
    for (i = 0; i < count && g_nfiles < MAX_FILES; i++) {
        /* raw entry: 4 bytes len + 4 bytes time + 1 byte attrib + name\0 */
        fattrib = (unsigned char)p[8];
        fname   = p + 9;
        namelen = (unsigned short)strlen(fname);

        /* advance pointer past this entry */
        p += 9 + namelen + 1;

        /* skip directories and volume labels */
        if (fattrib & ATTRIB_DIR) continue;

        /* lowercase filename in place */
        for (ci = 0; fname[ci]; ci++) {
            if (fname[ci] >= 'A' && fname[ci] <= 'Z')
                fname[ci] += 32;
        }
        /* fname[] is inside req_buf which we own — safe to mutate before File_Open */

        if (store_used >= STORE_SIZE) break;

        Dir_PathAdd(g_docroot[0] ? g_docroot : 0, fname, g_fspath);
        fh = File_Open(_symbank, g_fspath);
        if (_fileerr) continue;

        File_Seek(fh, 0, SEEK_SET);
        n = 0;
        do {
            room = STORE_SIZE - store_used - n;
            if (room == 0) break;
            chunk = File_Read(fh, _symbank, file_store + store_used + n, room);
            n += chunk;
        } while (chunk > 0);
        File_Close(fh);

        if (n == 0) continue;

        strcpy(g_files[g_nfiles].path, g_fspath);
        g_files[g_nfiles].offset = store_used;
        g_files[g_nfiles].size   = n;
        store_used += n;
        g_nfiles++;
        printf("Cached %s: %u bytes\r\n", fname, n);
    }

    printf("Store: %u bytes, %u files\r\n", store_used, (unsigned int)g_nfiles);
}

/* ------------------------------------------------------------------ */
/* File serving — pure RAM, zero disk I/O                             */
/* ------------------------------------------------------------------ */

static void serve_file(signed char sock, int head_only)
{
    unsigned char i;
    unsigned short n;

    for (i = 0; i < g_nfiles; i++) {
        if (path_cmp(g_files[i].path, g_fspath) != 0) continue;

        sprintf(file_buf,
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            mime_type(g_fspath),
            (unsigned int)g_files[i].size);
        n = (unsigned short)strlen(file_buf);
        TCP_Send(sock, _symbank, file_buf, n);

        if (!head_only)
            TCP_Send(sock, _symbank, file_store + g_files[i].offset, g_files[i].size);

        printf("served %s (%u)\r\n", g_fspath, g_files[i].size);
        return;
    }

    printf("404: %s\r\n", g_fspath);
    resp_404(sock);
}

/* ------------------------------------------------------------------ */
/* Request handling                                                     */
/* ------------------------------------------------------------------ */

static void handle_request(signed char sock)
{
    int req_len  = 0;
    int done     = 0;
    NetStat  ns;
    TCP_Trans tr;
    char *p;
    int i;
    unsigned short to_read;

    while (!done) {
        _symmsg[0] = 0;
        Msg_Sleep(_sympid, _netpid, _symmsg);
        if (_symmsg[0] != NET_TCPEVT) continue;

        TCP_Event(_symmsg, &ns);
        if (ns.socket != (unsigned char)sock) continue;

        while (ns.bytesrec > 0 && req_len < REQ_BUF_SIZE) {
            to_read = ns.bytesrec;
            if (to_read > (unsigned short)(REQ_BUF_SIZE - req_len))
                to_read = (unsigned short)(REQ_BUF_SIZE - req_len);

            if (TCP_Receive(sock, _symbank, req_buf + req_len, to_read, &tr) != 0)
                return;

            req_len += (int)tr.transferred;
            req_buf[req_len] = '\0';
            ns.bytesrec = tr.remaining;

            if (strstr(req_buf, "\r\n\r\n") || strstr(req_buf, "\n\n")) {
                done = 1;
                break;
            }
        }

        if (done) break;
        if (req_len >= REQ_BUF_SIZE) { resp_400(sock); return; }
        if (ns.status == TCP_CLOSED) return;
    }

    p = req_buf;
    for (i = 0; i < 7 && *p && *p != ' '; i++) g_method[i] = *p++;
    g_method[i] = '\0';
    if (*p == ' ') p++;

    for (i = 0; i < PATH_SIZE-1 && *p && *p != ' ' && *p != '\r' && *p != '\n'; i++)
        g_urlpath[i] = *p++;
    g_urlpath[i] = '\0';

    p = strchr(g_urlpath, '?'); if (p) *p = '\0';
    p = strchr(g_urlpath, '#'); if (p) *p = '\0';

    printf("%s %s\r\n", g_method, g_urlpath);

    if (strcmp(g_method, "GET") != 0 && strcmp(g_method, "HEAD") != 0) {
        resp_501(sock);
        return;
    }

    if (g_urlpath[0] == '\0' || strcmp(g_urlpath, "/") == 0)
        strcpy(g_urlpath, "/index.htm");

    if (!path_safe(g_urlpath)) { resp_404(sock); return; }

    make_fspath(g_urlpath);
    serve_file(sock, strcmp(g_method, "HEAD") == 0);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    signed char sock;
    int c;
    unsigned short mresp;

    g_port = HTTP_PORT;
    g_docroot[0] = '\0';

    if (argc >= 2 && argv[1][0] != '\0') {
        g_port = (unsigned short)atoi(argv[1]);
        if (g_port == 0) g_port = HTTP_PORT;
    }
    if (argc >= 3 && argv[2][0] != '\0') {
        strncpy(g_docroot, argv[2], PATH_SIZE - 1);
        g_docroot[PATH_SIZE - 1] = '\0';
    }

    printf("foal-httpd for SymbOS\r\n");

    /* Prevent double launch */
    if (App_Search(_symbank, "foal-httpd") != 0) {
        printf("Already running.\r\n");
        return 1;
    }
    App_Service(_symbank, "foal-httpd");

    if (Net_Init() < 0) {
        printf("Error: network daemon not found\r\n");
        return 1;
    }

    printf("Port    : %u\r\n", (unsigned int)g_port);
    if (g_docroot[0])
        printf("Root    : %s\r\n", g_docroot);
    else
        printf("Root    : (app directory)\r\n");

    /* Hold one handle open to suppress SymbOS STORLD (FAT media reload)
     * before every File_Open while loading the directory. */
    g_keep_fh = File_Open(_symbank, argv[0]);
    if (!_fileerr)
        printf("Drive   : live (fh %u)\r\n", (unsigned int)g_keep_fh);
    else
        printf("Drive   : keep-alive failed (%u)\r\n", (unsigned int)_fileerr);

    /* Load all files from docroot into RAM. */
    load_directory();

    /* Keep-alive handle no longer needed after load. */
    File_Close(g_keep_fh);

    /* Add systray icon so foal-httpd appears as a running service. */
    g_tray_id = Systray_Add(_symbank, (char *)g_tray_icon, 0);

    printf("Shell   : pid=%u ver=%u\r\n",
           (unsigned int)_shellpid, (unsigned int)_shellver);
    printf("\r\n");

    if (_shellpid)
        printf("Press Q to exit.\r\n");
    else
        printf("Running as daemon. Use task manager to exit.\r\n");
    printf("Waiting for connection...\r\n");

    for (;;) {
        sock = TCP_OpenServer(g_port);
        if (sock < 0) {
            if (_neterr == ERR_CONNECT) {
                /* Poll for OS quit message (non-blocking). */
                mresp = Msg_Receive(_sympid, -1, _symmsg);
                if ((mresp & 1) && _symmsg[0] == 0) break;
                /* Also allow Q from shell. */
                if (_shellpid) {
                    c = Shell_CharTest(0, 1);
                    if (c == 'Q' || c == 'q') break;
                }
                continue;
            }
            printf("Error: cannot open server socket (err %u)\r\n",
                   (unsigned int)_neterr);
            Net_ErrMsg(0);
            if (g_tray_id >= 0) Systray_Remove((unsigned char)g_tray_id);
            return 1;
        }

        printf("Connected\r\n");
        handle_request(sock);

        if (TCP_Disconnect(sock) != 0)
            TCP_Close(sock);

        printf("Done\r\n\r\n");

        /* Poll for OS quit message (non-blocking). */
        mresp = Msg_Receive(_sympid, -1, _symmsg);
        if ((mresp & 1) && _symmsg[0] == 0) break;
        if (_shellpid) {
            c = Shell_CharTest(0, 1);
            if (c == 'Q' || c == 'q') break;
        }
    }

    if (g_tray_id >= 0) Systray_Remove((unsigned char)g_tray_id);
    printf("Bye.\r\n");
    return 0;
}
