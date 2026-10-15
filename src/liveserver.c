/*
 * liveserver.c
 *
 * A tiny standalone "Live Server" for Windows.
 * - Serves static files from a given folder (defaults to current directory).
 * - Injects a small auto-reload script into HTML pages.
 * - Watches the folder for changes (ReadDirectoryChangesW) and notifies
 *   connected pages via long-polling, so the browser reloads automatically.
 * - Opens the default browser at http://localhost:<port>/ on startup.
 *
 * Designed to be launched from the Windows Explorer right-click context
 * menu ("Start Live Server Here"), passing "%V" (the clicked folder) as
 * argv[1].
 *
 * This file is the sockets, threads and Windows plumbing. Every decision it
 * makes about a request — how to parse it, whether it is the poll endpoint,
 * what filesystem path it names, whether that path is allowed — is a function
 * in http.c, which has no networking in it and is covered by
 * ../tests/http_test.c. See http.h for why.
 *
 * Build (MinGW):
 *   gcc -O2 -o liveserver.exe liveserver.c http.c -lws2_32 -lshell32
 *
 * Build (MSVC Developer Command Prompt):
 *   cl /O2 liveserver.c http.c ws2_32.lib shell32.lib
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define DEFAULT_PORT 5500
#define BUF_SIZE 65536

/* How long a client may hold a thread without finishing its request headers.
   Without this a connection that opens and says nothing parks a thread for
   ever, and this server uses one thread per connection. */
#define RECV_TIMEOUT_MS 15000

/* Above this, a file is streamed rather than read whole. Only HTML is read
   whole, so this is a guard against a pathological HTML file rather than a
   real limit — but ftell returns long, and on a 32-bit build a file over 2 GB
   makes it negative and malloc(size + 1) becomes a wild allocation. */
#define MAX_HTML_BYTES (64 * 1024 * 1024)

static char g_root[LS_MAX_PATH];
static volatile LONG g_changeCounter = 0; /* bumped whenever a file changes */
static int g_port = DEFAULT_PORT;

/* ---------- The reload-client script injected into HTML pages ---------- */
static const char *RELOAD_SCRIPT =
"<script>\n"
"(function(){\n"
"  var lastSeen = null;\n"
"  function poll(){\n"
"    var xhr = new XMLHttpRequest();\n"
"    xhr.open('GET', '" POLL_PATH "?ts=' + Date.now(), true);\n"
"    xhr.timeout = 35000;\n"
"    xhr.onload = function(){\n"
"      if (xhr.status === 200) {\n"
"        var v = xhr.responseText;\n"
"        if (lastSeen === null) { lastSeen = v; }\n"
"        else if (v !== lastSeen) { location.reload(); return; }\n"
"      }\n"
"      setTimeout(poll, 300);\n"
"    };\n"
"    xhr.onerror = function(){ setTimeout(poll, 1000); };\n"
"    xhr.ontimeout = function(){ setTimeout(poll, 300); };\n"
"    xhr.send();\n"
"  }\n"
"  poll();\n"
"})();\n"
"</script>\n";

/* ---------- Directory watcher thread ---------- */
static DWORD WINAPI watch_thread(LPVOID param) {
    (void)param;
    HANDLE hDir = CreateFileA(
        g_root, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);

    if (hDir == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Warning: could not watch directory for changes (error %lu)\n", GetLastError());
        return 1;
    }

    /* ReadDirectoryChangesW writes FILE_NOTIFY_INFORMATION structures here and
       requires the buffer to be DWORD-aligned. A plain char array is not
       guaranteed to be; a DWORD array is, by construction. */
    DWORD buffer[4096 / sizeof(DWORD)];
    DWORD bytesReturned;

    while (1) {
        BOOL ok = ReadDirectoryChangesW(
            hDir, buffer, (DWORD)sizeof(buffer), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned, NULL, NULL);

        if (!ok) {
            Sleep(500);
            continue;
        }

        InterlockedIncrement(&g_changeCounter);

        /* Debounce: coalesce rapid successive changes (e.g. editor save
           writing multiple times) into one reload signal. */
        Sleep(150);
    }

    CloseHandle(hDir);
    return 0;
}

/* ---------- Networking helpers ---------- */
static int send_all(SOCKET s, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return sent;
}

/*
 * Connection is always "close".
 *
 * It used to say "keep-alive" on the poll response while the caller closed the
 * socket immediately afterwards, which is a promise the server does not keep.
 * This server reads one request per connection and says so.
 */
static void send_header(SOCKET s, int status, const char *status_text,
                        const char *content_type, long long content_length) {
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, content_length);
    send_all(s, header, n);
}

static void send_error(SOCKET s, int status, const char *status_text, int want_body) {
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "<html><body><h1>%d %s</h1></body></html>", status, status_text);

    send_header(s, status, status_text, "text/html", n);
    if (want_body) send_all(s, body, n);
}

/* Handle the long-poll endpoint used by injected JS to detect file changes */
static void handle_poll(SOCKET s, int want_body) {
    /* Wait up to ~30s for a change, checking every 250ms, then respond
       with the current change counter value either way.

       The function used to open with

           long start = InterlockedCompareExchange(&g_changeCounter, 0, -999999);

       commented "just read", whose result was then discarded with (void)start.
       It compared against a value the counter can never hold, so it never
       wrote — a no-op dressed as a race-condition fix. The plain read below is
       what the function actually uses. */
    LONG baseline = g_changeCounter;
    int waited = 0;

    while (waited < 30000) {
        if (g_changeCounter != baseline) break;
        Sleep(250);
        waited += 250;
    }

    char body[32];
    int n = snprintf(body, sizeof(body), "%ld", (long)g_changeCounter);

    send_header(s, 200, "OK", "text/plain", n);
    if (want_body) send_all(s, body, n);
}

/* Serve a file, injecting the reload script if it's HTML */
static void handle_file(SOCKET s, const char *fs_path, int want_body) {
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        send_error(s, 404, "Not Found", want_body);
        return;
    }

    /* _ftelli64 rather than ftell: on a 32-bit build ftell returns long, and a
       file over 2 GB makes it negative, after which malloc(size + 1) is a wild
       allocation. */
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_error(s, 500, "Internal Server Error", want_body);
        return;
    }

    long long size = _ftelli64(f);

    if (size < 0 || _fseeki64(f, 0, SEEK_SET) != 0) {
        fclose(f);
        send_error(s, 500, "Internal Server Error", want_body);
        return;
    }

    const char *mime = get_mime_type(fs_path);
    int is_html = (strcmp(mime, "text/html") == 0);

    if (is_html && size > MAX_HTML_BYTES) {
        /* Too large to hold in memory for injection. Stream it instead — the
           page will not auto-reload, which is better than refusing to serve
           it or exhausting memory trying. */
        is_html = 0;
    }

    if (!is_html) {
        char *buf = (char *)malloc(BUF_SIZE);

        /* Every one of the allocations in this function used to be used
           without a check, so a failed allocation crashed the whole server
           rather than failing one request. */
        if (!buf) {
            fclose(f);
            send_error(s, 500, "Internal Server Error", want_body);
            return;
        }

        send_header(s, 200, "OK", mime, size);

        if (want_body) {
            size_t n;
            while ((n = fread(buf, 1, BUF_SIZE, f)) > 0) {
                if (send_all(s, buf, (int)n) < 0) break;
            }
        }

        free(buf);
        fclose(f);
        return;
    }

    char *content = (char *)malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        send_error(s, 500, "Internal Server Error", want_body);
        return;
    }

    /* fread's result used to be ignored. On a short read — the file was
       truncated between the seek and the read, or it is a special file — the
       tail of the buffer is uninitialised heap, and the whole buffer was then
       sent to the browser. */
    size_t got = fread(content, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size) {
        free(content);
        send_error(s, 500, "Internal Server Error", want_body);
        return;
    }

    content[size] = '\0';

    size_t script_len = strlen(RELOAD_SCRIPT);
    size_t prefix_len = injection_offset(content, (size_t)size);
    long long total_len = size + (long long)script_len;

    char *out = (char *)malloc((size_t)total_len + 1);
    if (!out) {
        free(content);
        send_error(s, 500, "Internal Server Error", want_body);
        return;
    }

    memcpy(out, content, prefix_len);
    memcpy(out + prefix_len, RELOAD_SCRIPT, script_len);
    memcpy(out + prefix_len + script_len, content + prefix_len, (size_t)size - prefix_len);
    out[total_len] = '\0';

    send_header(s, 200, "OK", "text/html", total_len);
    if (want_body) send_all(s, out, (int)total_len);

    free(content);
    free(out);
}

/* Handle a single client connection (one request; we close after) */
static DWORD WINAPI client_thread(LPVOID param) {
    SOCKET s = (SOCKET)(intptr_t)param;

    DWORD timeout = RECV_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    char request[8192];
    int total = 0;

    while (total < (int)sizeof(request) - 1) {
        int n = recv(s, request + total, sizeof(request) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        request[total] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }

    if (total == 0) {
        closesocket(s);
        return 0;
    }

    char method[16];
    char target[LS_MAX_PATH];

    if (!parse_request_line(request, method, sizeof(method), target, sizeof(target))) {
        send_error(s, 400, "Bad Request", 1);
        closesocket(s);
        return 0;
    }

    int is_get = (strcmp(method, "GET") == 0);
    int is_head = (strcmp(method, "HEAD") == 0);

    if (!is_get && !is_head) {
        send_error(s, 405, "Method Not Allowed", 1);
        closesocket(s);
        return 0;
    }

    /* HEAD used to be accepted and then answered with a full body. */
    int want_body = is_get;

    /* Asked before the query string is removed, and asked of a function that
       knows the endpoint may carry one. The original compared nineteen bytes
       of an eighteen-character literal against the raw target, so path[18] was
       the '?' of "?ts=..." and the comparison never matched. The reload
       endpoint has therefore never worked: every poll fell through to a 404,
       the client never set lastSeen, no page ever reloaded, and each open tab
       retried three times a second for ever, spawning a thread per request. */
    int is_poll = is_poll_target(target);

    strip_query(target);
    url_decode(target);

    /* Decoding happens before the path is examined, so "%2e%2e" is seen as
       "..". Do not reorder these two. */
    if (is_poll) {
        handle_poll(s, want_body);
        closesocket(s);
        return 0;
    }

    char fs_path[LS_MAX_PATH];
    if (!build_fs_path(g_root, target, fs_path, sizeof(fs_path))) {
        send_error(s, 404, "Not Found", want_body);
        closesocket(s);
        return 0;
    }

    handle_file(s, fs_path, want_body);
    closesocket(s);
    return 0;
}

/* Find a free port starting at g_port, trying a few subsequent ports */
static SOCKET create_listen_socket(int *chosen_port) {
    for (int attempt = 0; attempt < 20; attempt++) {
        SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET) return INVALID_SOCKET;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */
        addr.sin_port = htons((u_short)(g_port + attempt));

        /* SO_REUSEADDR is deliberately not set. On Windows it does not mean
           what it means on Unix: it lets a second process bind a port another
           process is already listening on, and whichever bound last silently
           receives the connections. For a server that finds its own free port
           anyway, that trades a clear "port in use" for a confusing one. */

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            if (listen(listen_sock, SOMAXCONN) == 0) {
                *chosen_port = g_port + attempt;
                return listen_sock;
            }
        }

        closesocket(listen_sock);
    }

    return INVALID_SOCKET;
}

int main(int argc, char **argv) {
    /* Determine root folder: argv[1] if given, else current directory */
    if (argc > 1) {
        strncpy(g_root, argv[1], sizeof(g_root) - 1);
        g_root[sizeof(g_root) - 1] = '\0';
    } else {
        GetCurrentDirectoryA(sizeof(g_root), g_root);
    }

    /* Canonicalise once, at startup. Every request is checked against this,
       so it has to be the resolved form: a root given as ".\site" or with a
       trailing "\." would otherwise never match a resolved request path and
       the server would 404 everything. */
    char resolved[LS_MAX_PATH];
    DWORD n = GetFullPathNameA(g_root, (DWORD)sizeof(resolved), resolved, NULL);
    if (n > 0 && n < sizeof(resolved)) {
        strcpy(g_root, resolved);
    }

    size_t rl = strlen(g_root);
    while (rl > 1 && g_root[rl - 1] == '\\' && g_root[rl - 2] != ':') {
        g_root[--rl] = '\0';
    }

    if (argc > 2) {
        g_port = atoi(argv[2]);
        if (g_port <= 0 || g_port > 65535) g_port = DEFAULT_PORT;
    }

    printf("Live Server (C edition)\n");
    printf("Root folder: %s\n", g_root);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int chosen_port = 0;
    SOCKET listen_sock = create_listen_socket(&chosen_port);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "Failed to bind to any port near %d\n", g_port);
        WSACleanup();
        return 1;
    }

    printf("Serving at: http://localhost:%d/\n", chosen_port);
    printf("Press Ctrl+C in this window to stop the server.\n\n");

    /* Start directory watcher. The handle is closed immediately; the thread
       runs for the life of the process and nothing waits on it. */
    HANDLE watcher = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (watcher) CloseHandle(watcher);

    /* Open default browser */
    char url[256];
    snprintf(url, sizeof(url), "http://localhost:%d/", chosen_port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);

    /* Accept loop: one thread per connection (simple, fine for local dev use) */
    while (1) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) continue;

        HANDLE h = CreateThread(NULL, 0, client_thread, (LPVOID)(intptr_t)client, 0, NULL);
        if (h) {
            CloseHandle(h);
        } else {
            closesocket(client);
        }
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
