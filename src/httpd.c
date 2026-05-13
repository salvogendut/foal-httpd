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

/* Build filesystem path: g_docroot + url_path (/ -> \) into g_fspath */
static void make_fspath(const char *url_path)
{
    unsigned int n;
    char *p;

    if (g_docroot[0]) {
        strncpy(g_fspath, g_docroot, FSPATH_SIZE - 1);
        g_fspath[FSPATH_SIZE - 1] = '\0';
        n = (unsigned int)strlen(g_fspath);
        if (n > 0 && g_fspath[n-1] != '\\' && n < FSPATH_SIZE - 2) {
            g_fspath[n]   = '\\';
            g_fspath[n+1] = '\0';
        }
        /* skip leading '/' from URL path */
        p = (char *)url_path;
        if (*p == '/') p++;
        strncat(g_fspath, p, FSPATH_SIZE - (unsigned int)strlen(g_fspath) - 1);
    } else {
        /* No docroot: use URL path relative to current directory */
        p = (char *)url_path;
        if (*p == '/') p++;
        strncpy(g_fspath, p, FSPATH_SIZE - 1);
        g_fspath[FSPATH_SIZE - 1] = '\0';
    }

    /* Forward slashes -> backslashes for SymbOS filesystem */
    for (p = g_fspath; *p; p++)
        if (*p == '/') *p = '\\';
}

/* ------------------------------------------------------------------ */
/* HTTP responses                                                       */
/* ------------------------------------------------------------------ */

static void resp_400(signed char sock)
{
    tcp_puts(sock, "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n");
}

static void resp_404(signed char sock)
{
    static const char body[] =
        "<html><body><h1>404 Not Found</h1></body></html>\r\n";
    char hdr[128];
    sprintf(hdr,
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        (unsigned int)strlen(body));
    tcp_puts(sock, hdr);
    tcp_puts(sock, body);
}

static void resp_501(signed char sock)
{
    tcp_puts(sock, "HTTP/1.0 501 Not Implemented\r\nConnection: close\r\n\r\n");
}

/* Open g_fspath, send headers + body (or headers only if head_only). */
static void serve_file(signed char sock, int head_only)
{
    unsigned char fh;
    unsigned short n;
    char hdr[128];

    printf("open: %s\r\n", g_fspath);
    fh = File_Open(_symbank, g_fspath);
    if (_fileerr) {
        printf("open err %u\r\n", (unsigned int)_fileerr);
        resp_404(sock);
        return;
    }

    /* HTTP/1.0 + Connection: close: body ends when connection closes.
     * No Content-Length needed; avoids File_Seek(SEEK_END) unreliability. */
    sprintf(hdr,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n\r\n",
        mime_type(g_fspath));
    tcp_puts(sock, hdr);

    if (!head_only) {
        while ((n = File_Read(fh, _symbank, file_buf, FILE_BUF_SIZE)) > 0) {
            if (TCP_Send(sock, _symbank, file_buf, n) != 0)
                break;
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
        strcpy(g_urlpath, "/index.html");

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

    printf("\r\n");

    /* --- Server loop: one connection at a time --- */
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
            if (_neterr == ERR_CONNECT) continue;   /* timeout, keep waiting */
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
    }

    return 0;
}
