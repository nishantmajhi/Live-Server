# Code Review — Live Server (C edition)

First reviewed 2026-09-04. Remediated 2026-09-05 on branch `cleanup/2026-09-05`.

## Verdict

The original review's judgement holds. The structure is good and binding
`INADDR_LOOPBACK` rather than `INADDR_ANY` is exactly the right decision.

And the headline feature had never worked, in any release.

## The one that matters

**The live-reload endpoint never matched.**

```c
int is_poll_endpoint = (strncmp(path, "/__liveserver_poll", 19) == 0);
```

The literal is eighteen characters. Comparing nineteen bytes compares the
literal's terminating NUL against `path[18]` — and this ran *before* the query
string was stripped, while the injected script always requests
`/__liveserver_poll?ts=...`. So `path[18]` was `?`, the comparison always
failed, every poll fell through to the static handler and got a 404, the
client's `xhr.status === 200` check never passed, `lastSeen` was never set, and
no page ever reloaded. The `setTimeout(poll, 300)` retry then hammered the
server three times a second per open tab, spawning a thread for each request.

Verified against the original binary rather than reasoned about. Built from
`git show HEAD:src/liveserver.c`:

```
ORIGINAL, the URL the injected script actually requests:   404
ORIGINAL, the bare path (which the script never requests): times out — it matched
```

And after the fix, end to end: a poll returns `3`, a file is touched, the next
poll returns `4`. Different values is exactly what the client compares, so the
reload fires.

The review said a unit test on the string comparison would not have caught
this, because the root cause is that the query string arrives attached. That is
right, and it is why `tests/http_test.c` asserts on the URL the script really
sends, and keeps `strncmp(..., 19) != 0` as an assertion of its own so the
reason for the fix cannot be lost.

## Also fixed

**Three unchecked allocations and an unchecked read in `handle_file`.** A
failed `malloc` crashed the whole server rather than failing one request, and
`fread`'s result was ignored — so a short read (the file truncated between the
seek and the read, or a special file) sent the uninitialised tail of a heap
buffer to the browser. All four are checked and answer 500.

**`ftell` on a large file.** It returns `long`, so on a 32-bit build a file over
2 GB makes `size` negative and `malloc(size + 1)` a wild allocation.
`_ftelli64` now, with a 64 MB cap above which an HTML file is streamed rather
than read whole — it loses the reload script and keeps working, which beats
either refusing it or exhausting memory.

**HEAD requests got a body.**

**Keep-alive was advertised and then violated.** The poll response said
`Connection: keep-alive` and the caller closed the socket immediately. It says
`close` now, because that is what it does.

**Dead code in `handle_poll`.** `InterlockedCompareExchange(&g_changeCounter,
0, -999999)`, commented "just read", assigned to `start` and discarded with
`(void)start`. It compared against a value the counter can never hold, so it
never wrote — a no-op that read like a race-condition fix.

**The traversal guard rejected valid filenames.** `strstr(url_path, "..")`
refuses `notes..backup.txt`. It now looks at path *components*.

**Nothing verified the resolved path stayed under the root.** The `..` check
was the only defence. `GetFullPathNameA` on both, then a prefix comparison that
includes the separator — without it, `C:\site-old\x` counts as inside
`C:\site`.

**`%00` and malformed escapes decoded to NUL.** The old decoder started its
value at zero and only assigned on a hex-digit match, so `%zz` produced a NUL
byte; and `%00` produced one by design. Either truncates the path and hides
everything after it from every subsequent check —
`secret.key%00.png` would pass an extension check and open `secret.key`. Both
are now left in the path as written.

**A colon in the path.** On Windows that opens an alternate data stream
(`index.html:hidden`) or names a drive. Refused.

**`sscanf("%15s %4095s")` truncated silently.** A request target longer than
the buffer was cut, and the truncated remainder resolved against the
filesystem as though it were the whole request. `parse_request_line` refuses
rather than truncates.

**No `recv` timeout.** A client that connected and said nothing held a thread
for ever — with one thread per connection, and the runaway polling above, that
adds up. Fifteen seconds now, on both directions.

**`ReadDirectoryChangesW` wrote into a `char buffer[4096]`.** The API requires
DWORD alignment. It is a `DWORD` array, correct by construction.

**Leaked handles.** The watcher thread's `CreateThread` result was discarded
without `CloseHandle`, and `hDir` was never closed. Also, a failed
`CreateThread` in the accept loop leaked the accepted socket.

**`SO_REUSEADDR` removed.** On Windows it does not mean what it means on Unix:
it lets a second process bind a port another process is already listening on,
and whichever bound last silently receives the connections. For a server that
finds its own free port anyway, that turns a clear "port in use" into a
confusing one.

## Tests

`tests\build.bat` — 85 checks, plain C, no framework. Also wired into the
release workflow, before the build: this project shipped a broken headline
feature in every release because nothing exercised it.

The split that made them possible: `src/http.c` holds everything that decides
something about a request and imports no networking; `src/liveserver.c` is the
sockets, threads and file I/O. Both are in every build script and in CI.

## Still open

**No directory listing.** A folder with no `index.html` is a 404. A listing
would make this much more useful as a general local server, and it is a
feature rather than a fix, so it is left for you.

**The injection point is a text search.** A `</body>` inside a comment, a
string literal or a `<pre>` block is matched. Documented in the README rather
than fixed, because parsing HTML to place a development-tool script is not a
trade worth making.

**One thread per connection.** Fine for its purpose, and the loopback bind
means it cannot be mistaken for a production server. Worth revisiting only if
the poll design changes.

**A `LICENSE` exists now.** The copyright name is Manish Majhi, taken from the
author section of the README in `C/LexiGone` — the GitHub handle is not the
name.

## Notes for a future session

Anything that inspects a request belongs in `src/http.c`, not in
`client_thread`. That is the rule that made the poll-endpoint bug findable, and
the only reason the 85 checks can run without a socket.

When changing the reload path, change `POLL_PATH` in `http.h` — the injected
script is built from that macro now, so the client and the server cannot
disagree about it again.

## Licence, 2026-10-02

The author chose one licence for all of their projects: free for personal,
non-commercial use, modifiable for the user's own use, and not to be
distributed, original or modified. No standard licence says that, so `LICENSE`
is a short custom text, the same in every project apart from the year and what
it excludes. The README's licence section says the same. Anything above that
calls the licence an open question, or calls the project MIT, is superseded.

## CI, 2026-10-02

`.github/workflows/ci.yml` runs `tests\build.bat` with the runner's MinGW gcc
on `windows-latest`, on every push. `release.yml` still runs the same tests
with MSVC before a tagged release. Nothing has been pushed, so it has not run
on GitHub yet. Every job was run before committing, from a fresh clone of this
branch, the way a runner would run it: Windows jobs here, in PowerShell 7 or
the shell the step names, with only the tools the job sets up on `PATH`. 85.
