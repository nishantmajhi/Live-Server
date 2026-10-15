/*
 * Tests for src/http.c.
 *
 *   gcc -std=c11 -Wall -Wextra -o tests/http_test.exe tests/http_test.c src/http.c
 *   tests/http_test.exe
 *
 * No framework. The project has no dependencies and does not want any; this is
 * forty lines of harness and then the checks.
 *
 * The reason this file exists is the poll-endpoint comparison. The headline
 * feature of this server had never worked, and could not have been noticed,
 * because there was no way to call any of that logic without opening a socket.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../src/http.h"

/* ---------- harness ---------- */

static int checks = 0;
static int failures = 0;
static const char *current = "";

static void group(const char *name) {
    current = name;
    printf("\n%s\n", name);
}

static void ok(int condition, const char *what) {
    checks++;
    if (condition) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s  [%s]\n", what, current);
    }
}

static void eq_str(const char *actual, const char *expected, const char *what) {
    checks++;
    if (strcmp(actual, expected) == 0) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s  [%s]\n        expected \"%s\"\n        got      \"%s\"\n",
               what, current, expected, actual);
    }
}

static void eq_size(size_t actual, size_t expected, const char *what) {
    checks++;
    if (actual == expected) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s  [%s]  expected %zu, got %zu\n", what, current, expected, actual);
    }
}

/* A mutable copy, since most of these functions work in place. */
static char *dup_of(const char *s) {
    char *copy = (char *)malloc(strlen(s) + 1);
    strcpy(copy, s);
    return copy;
}

/* ---------- the bug this file exists for ---------- */

static void test_poll_endpoint(void) {
    group("The live-reload endpoint");

    /* The injected script requests exactly this. The old code did
       strncmp(path, "/__liveserver_poll", 19) — nineteen bytes of an
       eighteen-character literal — against the target *before* the query
       string was stripped, so it compared the literal's NUL against '?' and
       never matched. Every poll fell through to a 404, no page ever reloaded,
       and each open tab retried three times a second for ever. */
    ok(is_poll_target("/__liveserver_poll?ts=1757000000000"),
       "the URL the injected script actually requests is recognised");

    /* Proof that the old comparison was wrong, kept as an assertion so the
       reason for the fix cannot be lost. */
    ok(strlen(POLL_PATH) == 18, "the endpoint literal is eighteen characters, not nineteen");
    ok(strncmp("/__liveserver_poll?ts=1", POLL_PATH, 19) != 0,
       "comparing nineteen bytes against the real request fails, as it always did");

    ok(is_poll_target("/__liveserver_poll"), "the bare path is recognised");
    ok(is_poll_target("/__liveserver_poll?"), "an empty query is recognised");
    ok(is_poll_target("/__liveserver_poll#x"), "a fragment is recognised");

    ok(!is_poll_target("/__liveserver_pollute"),
       "a longer path that starts the same is not the endpoint");
    ok(!is_poll_target("/__liveserver_pol"), "a shorter path is not the endpoint");
    ok(!is_poll_target("/index.html"), "an ordinary path is not the endpoint");
    ok(!is_poll_target("/"), "the root is not the endpoint");
    ok(!is_poll_target(""), "an empty target is not the endpoint");
    ok(!is_poll_target("/a/__liveserver_poll"), "the endpoint is not matched inside a path");
}

/* ---------- request line ---------- */

static void test_request_line(void) {
    group("Parsing the request line");

    char method[16];
    char target[256];

    ok(parse_request_line("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n",
                          method, sizeof(method), target, sizeof(target)),
       "an ordinary request parses");
    eq_str(method, "GET", "the method");
    eq_str(target, "/index.html", "the target");

    ok(parse_request_line("HEAD / HTTP/1.1\r\n", method, sizeof(method), target, sizeof(target)),
       "a HEAD request parses");
    eq_str(method, "HEAD", "the method");
    eq_str(target, "/", "the target");

    ok(parse_request_line("GET /a%20b.txt?x=1 HTTP/1.1\r\n",
                          method, sizeof(method), target, sizeof(target)),
       "a target with an escape and a query parses");
    eq_str(target, "/a%20b.txt?x=1", "the target is not decoded here");

    ok(!parse_request_line("", method, sizeof(method), target, sizeof(target)),
       "an empty request is refused");
    ok(!parse_request_line("GET\r\n", method, sizeof(method), target, sizeof(target)),
       "a request line with no target is refused");
    ok(!parse_request_line("PROPFINDPROPFIND / HTTP/1.1\r\n",
                           method, sizeof(method), target, sizeof(target)),
       "an over-long method is refused rather than truncated");

    /* The old code used sscanf("%15s %4095s"), which silently truncates. A
       truncated target would then have been resolved against the filesystem
       as though it were the whole request. */
    char small[8];
    ok(!parse_request_line("GET /a-much-longer-path HTTP/1.1\r\n",
                           method, sizeof(method), small, sizeof(small)),
       "an over-long target is refused rather than truncated");
}

/* ---------- URL decoding ---------- */

static void test_url_decode(void) {
    group("URL decoding");

    char *s;

    s = dup_of("/a%20b.txt");   url_decode(s); eq_str(s, "/a b.txt", "a percent escape"); free(s);
    s = dup_of("/a+b.txt");     url_decode(s); eq_str(s, "/a b.txt", "a plus"); free(s);
    s = dup_of("/%2e%2e/x");    url_decode(s); eq_str(s, "/../x", "an encoded parent reference"); free(s);
    s = dup_of("/%2E%2E/x");    url_decode(s); eq_str(s, "/../x", "in upper case"); free(s);
    s = dup_of("/plain.txt");   url_decode(s); eq_str(s, "/plain.txt", "nothing to decode"); free(s);
    s = dup_of("");             url_decode(s); eq_str(s, "", "an empty string"); free(s);
    s = dup_of("/%");           url_decode(s); eq_str(s, "/%", "a trailing percent"); free(s);
    s = dup_of("/%4");          url_decode(s); eq_str(s, "/%4", "a truncated escape"); free(s);

    /* The old decoder started its value at zero and only assigned when a hex
       digit matched, so "%zz" decoded to a NUL byte — which truncates the
       path and hides everything after it from every later check. */
    s = dup_of("/a%zzb.txt");
    url_decode(s);
    eq_str(s, "/a%zzb.txt", "a malformed escape is left alone, not decoded to NUL");
    free(s);

    /* And "%00" did the same by design: "secret.key%00.png" would pass an
       extension check and open "secret.key". */
    s = dup_of("/secret.key%00.png");
    url_decode(s);
    eq_str(s, "/secret.key%00.png", "an encoded NUL is left alone");
    ok(strlen(s) == strlen("/secret.key%00.png"), "and the path is not truncated");
    free(s);
}

/* ---------- query stripping ---------- */

static void test_strip_query(void) {
    group("Stripping the query string");

    char *s;

    s = dup_of("/a.html?x=1&y=2"); strip_query(s); eq_str(s, "/a.html", "a query"); free(s);
    s = dup_of("/a.html");         strip_query(s); eq_str(s, "/a.html", "no query"); free(s);
    s = dup_of("/a.html?");        strip_query(s); eq_str(s, "/a.html", "an empty query"); free(s);
    s = dup_of("?x=1");            strip_query(s); eq_str(s, "", "a query and nothing else"); free(s);

    /* A fragment never reaches a server from a browser, but a hand-written
       client can send one and it must not become part of a filename. */
    s = dup_of("/a.html#section"); strip_query(s); eq_str(s, "/a.html", "a fragment"); free(s);
}

/* ---------- parent components ---------- */

static void test_parent_components(void) {
    group("Rejecting parent references");

    ok(has_parent_component("/../etc/passwd"), "a leading parent reference");
    ok(has_parent_component("/a/../../b"), "one in the middle");
    ok(has_parent_component("/a/.."), "one at the end");
    ok(has_parent_component("/a\\..\\b"), "with backslashes, which Windows also accepts");
    ok(has_parent_component(".."), "on its own");

    ok(!has_parent_component("/index.html"), "an ordinary path");
    ok(!has_parent_component("/a/b/c.txt"), "a nested path");
    ok(!has_parent_component("/"), "the root");
    ok(!has_parent_component(""), "an empty path");

    /* The old check was strstr(url_path, ".."), which refuses these too. */
    ok(!has_parent_component("/notes..backup.txt"),
       "a filename that merely contains two dots is allowed");
    ok(!has_parent_component("/v1..v2/readme.md"), "a directory name containing two dots");
    ok(!has_parent_component("/...hidden"), "three dots is not a parent reference");
    ok(!has_parent_component("/a/./b"), "a single dot is not");
}

/* ---------- containment ---------- */

static void test_containment(void) {
    group("Keeping resolved paths under the root");

    char root[LS_MAX_PATH];
    GetCurrentDirectoryA(sizeof(root), root);

    char inside[LS_MAX_PATH];
    snprintf(inside, sizeof(inside), "%s\\some\\file.txt", root);
    ok(path_within_root(root, inside), "a path below the root");

    ok(path_within_root(root, root), "the root itself");

    char escaped[LS_MAX_PATH];
    snprintf(escaped, sizeof(escaped), "%s\\..\\elsewhere.txt", root);
    ok(!path_within_root(root, escaped),
       "a path that resolves outside the root, even though it starts inside it");

    /* The check that a plain prefix comparison gets wrong. */
    char sibling[LS_MAX_PATH];
    snprintf(sibling, sizeof(sibling), "%s-other\\file.txt", root);
    ok(!path_within_root(root, sibling), "a sibling directory whose name extends the root's");

    ok(!path_within_root(root, "C:\\Windows\\System32\\drivers\\etc\\hosts"),
       "an absolute path elsewhere");
}

static void test_build_fs_path(void) {
    group("Building a filesystem path");

    char root[LS_MAX_PATH];
    GetCurrentDirectoryA(sizeof(root), root);

    char out[LS_MAX_PATH];
    char expected[LS_MAX_PATH];

    ok(build_fs_path(root, "/index.html", out, sizeof(out)), "an ordinary file is allowed");
    snprintf(expected, sizeof(expected), "%s\\index.html", root);
    eq_str(out, expected, "and resolves under the root");

    ok(build_fs_path(root, "/", out, sizeof(out)), "the root serves index.html");
    eq_str(out, expected, "the same path");

    ok(build_fs_path(root, "/css/site.css", out, sizeof(out)), "a nested file");
    snprintf(expected, sizeof(expected), "%s\\css\\site.css", root);
    eq_str(out, expected, "with separators converted");

    ok(!build_fs_path(root, "/../secrets.txt", out, sizeof(out)), "a parent reference is refused");
    ok(!build_fs_path(root, "/a/../../b", out, sizeof(out)), "a deeper one is refused");

    /* On Windows a colon opens an alternate data stream, and a drive letter is
       an absolute path. Neither belongs in a URL path. */
    ok(!build_fs_path(root, "/index.html:hidden", out, sizeof(out)),
       "an alternate data stream is refused");
    ok(!build_fs_path(root, "/C:/Windows/win.ini", out, sizeof(out)),
       "an absolute path is refused");

    ok(build_fs_path(root, "/notes..backup.txt", out, sizeof(out)),
       "a filename containing two dots is still allowed");

    char tiny[8];
    ok(!build_fs_path(root, "/a-path-far-longer-than-the-buffer.html", tiny, sizeof(tiny)),
       "a path that will not fit is refused rather than truncated");
}

/* ---------- script injection ---------- */

static void test_injection_offset(void) {
    group("Where the reload script goes");

    const char *with_body = "<html><body>hi</body></html>";
    eq_size(injection_offset(with_body, strlen(with_body)),
            (size_t)(strstr(with_body, "</body>") - with_body),
            "before </body> when there is one");

    const char *no_body = "<html>hi</html>";
    eq_size(injection_offset(no_body, strlen(no_body)),
            (size_t)(strstr(no_body, "</html>") - no_body),
            "before </html> when there is no body");

    const char *fragment = "<p>just a fragment</p>";
    eq_size(injection_offset(fragment, strlen(fragment)), strlen(fragment),
            "at the end when there is neither");

    eq_size(injection_offset("", 0), 0, "an empty document");

    /* The result is an offset into the document, so the caller can copy the
       prefix, the script and the remainder without recomputing anything. This
       is what the original did with three memcpy calls and a pointer, and it
       is kept because a document with no </body> took a different branch that
       was easy to get subtly wrong. */
    const char *two_bodies = "<body>a</body><body>b</body>";
    eq_size(injection_offset(two_bodies, strlen(two_bodies)), (size_t)7,
            "the first closing tag wins");
}

/* ---------- MIME types ---------- */

static void test_mime(void) {
    group("Content types");

    eq_str(get_mime_type("index.html"), "text/html", "html");
    eq_str(get_mime_type("INDEX.HTM"), "text/html", "case is ignored");
    eq_str(get_mime_type("site.css"), "text/css", "css");
    eq_str(get_mime_type("app.js"), "application/javascript", "js");
    eq_str(get_mime_type("data.json"), "application/json", "json");
    eq_str(get_mime_type("logo.svg"), "image/svg+xml", "svg");
    eq_str(get_mime_type("photo.jpeg"), "image/jpeg", "jpeg");
    eq_str(get_mime_type("archive.tar.gz"), "application/octet-stream", "an unknown extension");
    eq_str(get_mime_type("Makefile"), "application/octet-stream", "no extension");

    /* Only text/html gets the reload script, and the server compares the
       result exactly, so a type that merely contains "html" must not be
       returned for anything else. */
    eq_str(get_mime_type("page.xhtml"), "application/octet-stream",
           "xhtml is not text/html, so it is not injected into");
}

int main(void) {
    printf("Live Server — request handling\n");

    test_poll_endpoint();
    test_request_line();
    test_url_decode();
    test_strip_query();
    test_parent_components();
    test_containment();
    test_build_fs_path();
    test_injection_offset();
    test_mime();

    printf("\n%d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
