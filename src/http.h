/*
 * http.h
 *
 * The parts of Live Server that decide things, separated from the parts that
 * talk to sockets and Windows.
 *
 * The reason for the split is a bug. The live-reload endpoint was matched with
 *
 *     strncmp(path, "/__liveserver_poll", 19)
 *
 * where the literal is eighteen characters. Comparing nineteen bytes compares
 * the literal's terminating NUL against path[18] — and this ran before the
 * query string was stripped, while the injected script always requests
 * "/__liveserver_poll?ts=...". So path[18] was '?', the comparison never
 * matched, no page ever reloaded, and every open tab polled a 404 three times
 * a second forever, spawning a thread each time.
 *
 * The headline feature had never worked. Nothing could have noticed, because
 * none of this logic could be called without opening a socket. It can now, and
 * tests/http_test.c does.
 *
 * Everything here is a pure function of its arguments except path_within_root,
 * which asks Windows to canonicalise a path.
 */

#ifndef LIVESERVER_HTTP_H
#define LIVESERVER_HTTP_H

#include <stddef.h>

/* The URL the injected reload script polls. */
#define POLL_PATH "/__liveserver_poll"

/* Windows MAX_PATH is 260, which a project directory can exceed easily. */
#define LS_MAX_PATH 4096

/* Content-Type for a file, by extension. */
const char *get_mime_type(const char *path);

/* URL-decode in place: %XX and +. */
void url_decode(char *s);

/* Cut a request target at '?', returning it. Safe on a target with no query. */
void strip_query(char *target);

/*
 * Is this request target the reload endpoint?
 *
 * Takes the raw target, query string and all, because that is what arrives and
 * because getting that wrong is the bug this file exists for.
 */
int is_poll_target(const char *target);

/*
 * Parse a request line into method and target.
 *
 * Returns 1 on success. Fails rather than truncating when either field is
 * longer than the buffer given for it: a truncated target would be resolved
 * against the filesystem as though it were the whole thing.
 */
int parse_request_line(const char *request, char *method, size_t method_size,
                       char *target, size_t target_size);

/*
 * Does this URL path contain a ".." component?
 *
 * The original test was strstr(url_path, ".."), which also rejects a
 * legitimate file named "notes..backup.txt". This looks at components between
 * separators, so only an actual parent reference is refused.
 */
int has_parent_component(const char *url_path);

/*
 * Turn a URL path into a filesystem path under root.
 *
 * Returns 1 on success, 0 if the path is refused. Callers must already have
 * URL-decoded the path: "%2e%2e" has to become ".." before it can be
 * recognised as one.
 */
int build_fs_path(const char *root, const char *url_path, char *out, size_t out_size);

/*
 * Is `path` the directory `root`, or inside it?
 *
 * Both are canonicalised with GetFullPathNameA first, so a path assembled from
 * pieces that individually look harmless is still checked as what it resolves
 * to. build_fs_path's component check is the first line of defence; this is
 * the one that does not depend on having thought of every spelling.
 */
int path_within_root(const char *root, const char *path);

/*
 * Where the reload script should be inserted in an HTML document: the offset
 * of "</body>", else of "</html>", else the end of the document.
 *
 * Matching on the raw text means a "</body>" inside a comment, a string
 * literal or a <pre> block is matched too. For a development tool that is an
 * acceptable simplification, and it is the behaviour that was there before.
 */
size_t injection_offset(const char *html, size_t length);

#endif
