/*
 * foal-httpd — minimal HTTP/1.0 server for SymbOS
 *
 * Serves static files from a document-root directory over TCP.
 * One connection handled at a time; HTTP/1.0 with Connection: close.
 *
 * Usage:  httpd [port [docroot]]
 *   port    — TCP port (default 80)
 *   docroot — filesystem path to serve files from (default: current dir)
 *
 * Compile: cc src/httpd.c -lnet -N "foal-httpd" -o build/httpd.com
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <network.h>
#include <symbos.h>

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

#define HTTP_PORT     80
#define REQ_BUF_SIZE  512   /* request accumulation (headers only)     */
#define FILE_BUF_SIZE 512   /* file read chunk                         */
#define PATH_SIZE     128   /* individual path component               */
#define FSPATH_SIZE   200   /* combined filesystem path                */

/* ------------------------------------------------------------------ */
/* Globals  (kept in data segment to spare the Z80 stack)             */
/* ------------------------------------------------------------------ */

static char req_buf[REQ_BUF_SIZE + 1];
static char file_buf[FILE_BUF_SIZE];
static char g_method[8];
static char g_urlpath[PATH_SIZE];
static char g_fspath[FSPATH_SIZE];
static char g_docroot[PATH_SIZE];
static unsigned short g_port;
static unsigned char g_keep_fh;     /* open handle held to suppress media reload */
static char g_warmpath[FSPATH_SIZE]; /* path to default doc */
static char page_buf[FILE_BUF_SIZE]; /* startup-cached default document body */
static unsigned short page_size;     /* bytes in page_buf (0 = not cached) */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *mime_type(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    e++;
    if (!strcmp(e, "html") || !strcmp(e, "htm")) return "text/html";
    if (!strcmp(e, "txt"))  return "text/plain";
    if (!strcmp(e, "css"))  return "text/css";
    if (!strcmp(e, "js"))   return "application/javascript";
    if (!strcmp(e, "png"))  return "image/png";
    if (!strcmp(e, "gif"))  return "image/gif";
    if (!strcmp(e, "jpg") || !strcmp(e, "jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

/* Return 0 if path contains ".." (directory traversal attempt) */
static int path_safe(const char *p)
{
    while (*p) {
        if (p[0] == '.' && p[1] == '.') return 0;
        p++;
    }
    return 1;
}

/* Send NUL-terminated string over TCP */
static void tcp_puts(signed char sock, const char *s)
{
    unsigned short n = (unsigned short)strlen(s);
    if (n) TCP_Send(sock, _symbank, (char *)s, n);
}

/* Build absolute SymbOS filesystem path from URL path into g_fspath.
 * Dir_PathAdd: null base → app's directory; handles / → \ conversion. */
static void make_fspath(const char *url_path)
{
    char *p = (char *)url_path;
    if (*p == '/') p++;
    Dir_PathAdd(g_docroot[0] ? g_docroot : 0, p, g_fspath);
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

/* Open g_fspath, send headers + body (or headers only if head_only).
 * No Dir_ReadRaw, no File_Seek — stream until File_Read returns 0 (EOF). */
static void serve_file(signed char sock, int head_only)
{
    unsigned char fh;
    unsigned short n;
    unsigned short total;

    printf("open: %s\r\n", g_fspath);
    fh = File_Open(_symbank, g_fspath);
    if (_fileerr) {
        printf("open err %u\r\n", (unsigned int)_fileerr);
        resp_404(sock);
        return;
    }
    printf("fh=%u\r\n", (unsigned int)fh);
    /* Reset position to 0: per-path position may be non-zero from a prior
     * keepalive read of the same file.  SEEK_SET without SEEK_END is safe. */
    File_Seek(fh, 0, SEEK_SET);

    sprintf(file_buf,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n\r\n",
        mime_type(g_fspath));
    n = (unsigned short)strlen(file_buf);
    TCP_Send(sock, _symbank, file_buf, n);

    if (!head_only) {
        printf("ps=%u eq=%d\r\n", page_size, strcmp(g_fspath, g_warmpath));
        if (page_size > 0 && strcmp(g_fspath, g_warmpath) == 0) {
            /* Serve from startup RAM cache — no per-request disk access */
            TCP_Send(sock, _symbank, page_buf, page_size);
            printf("sent from cache: %u\r\n", (unsigned int)page_size);
        } else {
            total = 0;
            File_Seek(fh, 0, SEEK_SET);
            for (;;) {
                n = File_Read(fh, _symbank, file_buf, FILE_BUF_SIZE);
                printf("rd=%u\r\n", (unsigned int)n);
                if (n == 0) break;
                total += n;
                if (TCP_Send(sock, _symbank, file_buf, n) != 0)
                    break;
            }
            printf("sent %u\r\n", total);
        }
    }

    File_Close(fh);
}

/* ------------------------------------------------------------------ */
/* Request handling                                                     */
/* ------------------------------------------------------------------ */

/*
 * Accumulate HTTP headers, parse the request line, and serve the
 * response.  Returns when the response has been sent (or on error).
 */
static void handle_request(signed char sock)
{
    int req_len  = 0;
    int done     = 0;
    NetStat  ns;
    TCP_Trans tr;
    char *p;
    int i;
    unsigned short to_read;

    /* --- Accumulate HTTP headers (\r\n\r\n ends them) --- */
    while (!done) {
        _symmsg[0] = 0;
        Msg_Sleep(_sympid, _netpid, _symmsg);
        if (_symmsg[0] != NET_TCPEVT) continue;

        TCP_Event(_symmsg, &ns);
        if (ns.socket != (unsigned char)sock) continue;

        /* Drain everything currently in the receive buffer */
        while (ns.bytesrec > 0 && req_len < REQ_BUF_SIZE) {
            to_read = ns.bytesrec;
            if (to_read > (unsigned short)(REQ_BUF_SIZE - req_len))
                to_read = (unsigned short)(REQ_BUF_SIZE - req_len);

            if (TCP_Receive(sock, _symbank, req_buf + req_len, to_read, &tr) != 0)
                return; /* receive error */

            req_len += (int)tr.transferred;
            req_buf[req_len] = '\0';
            ns.bytesrec = tr.remaining;

            if (strstr(req_buf, "\r\n\r\n") || strstr(req_buf, "\n\n")) {
                done = 1;
                break;
            }
        }

        if (done) break;

        /* Buffer full without seeing end-of-headers */
        if (req_len >= REQ_BUF_SIZE) {
            resp_400(sock);
            return;
        }

        /* Client closed before sending a complete request */
        if (ns.status == TCP_CLOSED) return;
    }

    /* --- Parse request line: METHOD /path HTTP/x.x --- */
    p = req_buf;
    for (i = 0; i < 7 && *p && *p != ' '; i++)
        g_method[i] = *p++;
    g_method[i] = '\0';
    if (*p == ' ') p++;

    for (i = 0; i < PATH_SIZE-1 && *p && *p != ' ' && *p != '\r' && *p != '\n'; i++)
        g_urlpath[i] = *p++;
    g_urlpath[i] = '\0';

    /* Strip query string and fragment */
    p = strchr(g_urlpath, '?');
    if (p) *p = '\0';
    p = strchr(g_urlpath, '#');
    if (p) *p = '\0';

    printf("%s %s\r\n", g_method, g_urlpath);

    /* --- Validate method --- */
    if (strcmp(g_method, "GET") != 0 && strcmp(g_method, "HEAD") != 0) {
        resp_501(sock);
        return;
    }

    /* Default document */
    if (g_urlpath[0] == '\0' || strcmp(g_urlpath, "/") == 0)
        strcpy(g_urlpath, "/index.htm");

    /* Directory traversal guard */
    if (!path_safe(g_urlpath)) {
        resp_404(sock);
        return;
    }

    /* Build filesystem path and serve */
    make_fspath(g_urlpath);
    serve_file(sock, strcmp(g_method, "HEAD") == 0);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    signed char sock;
    unsigned char wfh;
    int c;

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

    if (Net_Init() < 0) {
        printf("Error: network daemon not found\r\n");
        return 1;
    }

    printf("Port    : %u\r\n", (unsigned int)g_port);

    if (g_docroot[0])
        printf("Root    : %s\r\n", g_docroot);
    else
        printf("Root    : (current directory)\r\n");

    /*
     * Keep one file handle open for the server's lifetime.
     * SymbOS performs a "media reload" (re-reads the FAT) before every
     * File_Open on a removeable-media device when no file on that device is
     * currently open.  Holding one handle open suppresses this reload on
     * every HTTP request.  Opening the server's own executable (argv[0])
     * is always safe and guaranteed to be on the same drive.
     */
    g_keep_fh = File_Open(_symbank, argv[0]);
    if (!_fileerr)
        printf("Drive   : live (fh %u)\r\n", (unsigned int)g_keep_fh);
    else
        printf("Drive   : keep-alive failed (%u)\r\n", (unsigned int)_fileerr);

    /* Build path to the default document (index.htm) for data-cluster keepalive.
     * argv[0] File_Read returns 0 immediately (SymbOS skips reads on loaded code);
     * index.htm has real data clusters that actually touch the USB drive. */
    make_fspath("/index.htm");
    strncpy(g_warmpath, g_fspath, FSPATH_SIZE - 1);
    g_warmpath[FSPATH_SIZE - 1] = '\0';
    g_fspath[0] = '\0';
    printf("Warmpath: %s\r\n", g_warmpath);

    /* Read index.htm into RAM while drive is fresh.  Requests are then served
     * from page_buf without any per-request File_Read disk access. */
    wfh = File_Open(_symbank, g_warmpath);
    if (!_fileerr) {
        File_Seek(wfh, 0, SEEK_SET);
        page_size = File_Read(wfh, _symbank, page_buf, FILE_BUF_SIZE);
        File_Close(wfh);
        printf("Cached  : %u bytes\r\n", (unsigned int)page_size);
    }

    printf("Shell   : pid=%u ver=%u\r\n",
           (unsigned int)_shellpid, (unsigned int)_shellver);
    printf("\r\n");

    /* --- Server loop: one connection at a time --- */
    if (_shellver >= 23)
        printf("Press Q to exit.\r\n");
    else
        printf("Exit via task manager (SymShell < 2.3).\r\n");
    printf("Waiting for connection...\r\n");
    for (;;) {
        /*
         * TCP_OpenServer is listen+accept combined: it sends TCPOPN to the
         * daemon, then internally waits for a NET_TCPEVT TCP_OPENED event.
         * If no client connects before its internal timeout it returns -1
         * with _neterr = ERR_CONNECT (24).  Just retry in that case.
         */
        sock = TCP_OpenServer(g_port);
        if (sock < 0) {
            if (_neterr == ERR_CONNECT) {
                /* Open+seek(0)+read+close index.htm to keep USB drive spinning.
                 * SEEK_SET resets per-path position so File_Read hits real data. */
                wfh = File_Open(_symbank, g_warmpath);
                if (!_fileerr) {
                    File_Seek(wfh, 0, SEEK_SET);
                    File_Read(wfh, _symbank, file_buf, FILE_BUF_SIZE);
                    File_Close(wfh);
                }
                c = Shell_CharTest(0, 1);
                if (c == 'Q' || c == 'q') break;
                continue;
            }
            printf("Error: cannot open server socket (err %u)\r\n",
                   (unsigned int)_neterr);
            Net_ErrMsg(0);
            return 1;
        }

        /* TCP_OpenServer already consumed the TCP_OPENED event internally.
         * The connection is established; go straight to reading the request. */
        printf("Connected\r\n");
        handle_request(sock);

        /*
         * We initiated HTTP/1.0 with Connection: close, so we close first.
         * If the client already disconnected, TCP_Disconnect will fail and
         * we fall back to TCP_Close to free the socket.
         */
        if (TCP_Disconnect(sock) != 0)
            TCP_Close(sock);

        printf("Done\r\n\r\n");

        /* check for quit after each served request too */
        c = Shell_CharTest(0, 1);
        if (c == 'Q' || c == 'q') break;
    }

    File_Close(g_keep_fh);
    printf("Bye.\r\n");
    return 0;
}
