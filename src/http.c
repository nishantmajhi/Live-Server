#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (!_stricmp(ext, "html") || !_stricmp(ext, "htm")) return "text/html";
    if (!_stricmp(ext, "css")) return "text/css";
    if (!_stricmp(ext, "js") || !_stricmp(ext, "mjs")) return "application/javascript";
    if (!_stricmp(ext, "json")) return "application/json";
    if (!_stricmp(ext, "png")) return "image/png";
    if (!_stricmp(ext, "jpg") || !_stricmp(ext, "jpeg")) return "image/jpeg";
    if (!_stricmp(ext, "gif")) return "image/gif";
    if (!_stricmp(ext, "webp")) return "image/webp";
    if (!_stricmp(ext, "svg")) return "image/svg+xml";
    if (!_stricmp(ext, "ico")) return "image/x-icon";
    if (!_stricmp(ext, "woff")) return "font/woff";
    if (!_stricmp(ext, "woff2")) return "font/woff2";
    if (!_stricmp(ext, "txt") || !_stricmp(ext, "md")) return "text/plain";
    if (!_stricmp(ext, "pdf")) return "application/pdf";
    if (!_stricmp(ext, "mp4")) return "video/mp4";
    if (!_stricmp(ext, "webmanifest")) return "application/manifest+json";
    return "application/octet-stream";
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void url_decode(char *s) {
    char *o = s;

    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            int hi = hex_value((unsigned char)s[1]);
            int lo = hex_value((unsigned char)s[2]);

            /* Two ways this used to produce a NUL byte, both of which
               truncate the path and hide whatever follows from every check
               after this point:

                 - A malformed escape. The old decoder started v at zero and
                   only assigned when a digit matched, so "%zz" decoded to
                   '\0' rather than being rejected or left alone.
                 - A deliberate "%00", which decoded to '\0' by design and let
                   "secret.key%00.png" pass an extension check while opening
                   "secret.key".

               Both are now left in the path as they were written. A browser
               produces neither. */
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                *o++ = *s++;
                continue;
            }

            *o++ = (char)((hi << 4) | lo);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }

    *o = '\0';
}

void strip_query(char *target) {
    char *q = strchr(target, '?');
    if (q) *q = '\0';

    /* A fragment never reaches a server, but a hand-written client can send
       one and it must not become part of a filename. */
    char *h = strchr(target, '#');
    if (h) *h = '\0';
}

int is_poll_target(const char *target) {
    size_t n = strlen(POLL_PATH);

    if (strncmp(target, POLL_PATH, n) != 0) return 0;

    /* The whole target, or the path followed by a query or a fragment.
       "/__liveserver_pollute" must not match. */
    return target[n] == '\0' || target[n] == '?' || target[n] == '#';
}

int parse_request_line(const char *request, char *method, size_t method_size,
                       char *target, size_t target_size) {
    if (method_size == 0 || target_size == 0) return 0;

    method[0] = '\0';
    target[0] = '\0';

    const char *p = request;
    while (*p == ' ') p++;

    const char *method_start = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;

    size_t method_len = (size_t)(p - method_start);
    if (method_len == 0 || method_len >= method_size) return 0;

    memcpy(method, method_start, method_len);
    method[method_len] = '\0';

    while (*p == ' ') p++;

    const char *target_start = p;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;

    size_t target_len = (size_t)(p - target_start);

    /* Refuse rather than truncate. sscanf("%4095s") silently cut a longer
       target, and the truncated remainder would then have been resolved
       against the filesystem as though it were the whole request. */
    if (target_len == 0 || target_len >= target_size) return 0;

    memcpy(target, target_start, target_len);
    target[target_len] = '\0';

    return 1;
}

int has_parent_component(const char *url_path) {
    const char *p = url_path;

    while (*p) {
        while (*p == '/' || *p == '\\') p++;

        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;

        size_t len = (size_t)(p - start);

        if (len == 2 && start[0] == '.' && start[1] == '.') return 1;
    }

    return 0;
}

int build_fs_path(const char *root, const char *url_path, char *out, size_t out_size) {
    if (has_parent_component(url_path)) return 0;

    /* On Windows a colon opens an alternate data stream: "index.html:secret"
       reads a different stream of the same file, and "C:/..." is an absolute
       path. Neither belongs in a URL path. */
    if (strchr(url_path, ':')) return 0;

    char clean[LS_MAX_PATH];
    if (strlen(url_path) >= sizeof(clean)) return 0;
    strcpy(clean, url_path);

    for (char *p = clean; *p; p++) {
        if (*p == '/') *p = '\\';
    }

    if (clean[0] == '\0' || (clean[0] == '\\' && clean[1] == '\0')) {
        if ((size_t)snprintf(out, out_size, "%s\\index.html", root) >= out_size) return 0;
        return path_within_root(root, out);
    }

    if ((size_t)snprintf(out, out_size, "%s%s%s",
                         root,
                         clean[0] == '\\' ? "" : "\\",
                         clean) >= out_size) {
        return 0;
    }

    DWORD attrs = GetFileAttributesA(out);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        char idx[LS_MAX_PATH];
        if ((size_t)snprintf(idx, sizeof(idx), "%s\\index.html", out) >= sizeof(idx)) return 0;
        if (strlen(idx) >= out_size) return 0;
        strcpy(out, idx);
    }

    return path_within_root(root, out);
}

int path_within_root(const char *root, const char *path) {
    char full_root[LS_MAX_PATH];
    char full_path[LS_MAX_PATH];

    DWORD n = GetFullPathNameA(root, (DWORD)sizeof(full_root), full_root, NULL);
    if (n == 0 || n >= sizeof(full_root)) return 0;

    n = GetFullPathNameA(path, (DWORD)sizeof(full_path), full_path, NULL);
    if (n == 0 || n >= sizeof(full_path)) return 0;

    size_t root_len = strlen(full_root);
    while (root_len > 0 && full_root[root_len - 1] == '\\') root_len--;

    if (_strnicmp(full_path, full_root, root_len) != 0) return 0;

    /* The separator matters: without it "C:\site-old\x" counts as inside
       "C:\site". */
    char next = full_path[root_len];

    return next == '\0' || next == '\\';
}

size_t injection_offset(const char *html, size_t length) {
    static const char *const MARKERS[] = { "</body>", "</html>" };

    for (int i = 0; i < 2; i++) {
        const char *found = strstr(html, MARKERS[i]);
        if (found) return (size_t)(found - html);
    }

    return length;
}
