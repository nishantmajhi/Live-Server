# Live Server (C edition) — Right-Click Context Menu

Right-click a folder in Explorer, choose **Start Live Server Here**, and that
folder is being served at `http://localhost:5500/` in your browser. Edit a file
and the page reloads itself.

That is VS Code's Live Server extension, without VS Code — one 500 KB
executable, no Node, no editor, no project. Written in C against raw Winsock.

## How it works

Four moving parts, and the interesting one is the third.

**Serving.** A listening socket on `127.0.0.1`, and a thread per connection.
One request per connection, then close. That is a real simplification and the
code says so; for a machine serving one developer it is the right trade.

**Injection.** Any page served as `text/html` gets a small `<script>` inserted
before its `</body>`. The script long-polls one endpoint and calls
`location.reload()` when the answer changes.

**Watching.** `ReadDirectoryChangesW` on the served folder, recursive. Every
change bumps a counter. A poll request reads the counter, then sleeps in 250 ms
steps for up to thirty seconds waiting for it to change, and answers with
whatever it holds at the end. So a change is noticed within about a quarter of
a second, and an idle page costs one parked thread rather than a request every
few hundred milliseconds.

**Ports.** Starts at the port given on the command line, or 5500, and tries the
next twenty if that one is taken — so several projects can be served at once.

## What it will not do

- **Reach the network.** It binds `INADDR_LOOPBACK`, not `INADDR_ANY`. Nothing
  outside this machine can connect, whatever the firewall says.
- **Serve anything outside the folder you clicked.** A `..` *component* in a
  request is refused — a filename that merely contains two dots, like
  `notes..backup.txt`, is fine — the path is URL-decoded before that check so
  `%2e%2e` is caught, and then the resolved path is canonicalised with
  `GetFullPathNameA` and checked to be under the canonicalised root. Also
  refused: a colon, which on Windows opens an alternate data stream or names a
  drive.
- **Answer anything but GET and HEAD.** Everything else is 405.
- **Send a body for a HEAD request.** It used to.
- **Speak HTTPS,** or keep a connection alive. It says `Connection: close` and
  means it.

## Building

MinGW-w64:

```
cd src
build.bat
```

or by hand, from `src/`:

```
gcc -O2 -o liveserver.exe liveserver.c http.c -lws2_32 -lshell32
```

MSVC, from a Developer Command Prompt: `build_msvc.bat`. The GitHub Actions
release workflow uses MSVC; both are supported and both are exercised —
`build.bat` locally, MSVC in CI.

### Tests

From the repository root:

```
tests\build.bat
```

Plain C, no framework, because the project has no dependencies and should not
gain one to be testable. 85 checks over the request handling: the poll
endpoint, request-line parsing, URL decoding, `..` rejection, path
containment, where the script gets injected, and content types.

They exist because the reload feature had never worked in any release. The
endpoint was matched with

```c
strncmp(path, "/__liveserver_poll", 19)
```

against a literal eighteen characters long, before the query string was
stripped — and the injected script always requests
`/__liveserver_poll?ts=...`. So the nineteenth byte compared was `?` against
the literal's terminating NUL, the match always failed, every poll got a 404,
no page ever reloaded, and each open tab retried three times a second for ever,
spawning a thread per attempt.

Nothing could have caught that, because none of this logic could be called
without opening a socket. `src/http.c` is now that logic with no networking in
it, and `src/liveserver.c` is the sockets and threads.

## Project layout

```
Live Server/
├── .github/workflows/release.yml   builds, tests and publishes a release
├── src/
│   ├── liveserver.c    sockets, threads, file serving, main
│   ├── http.c, http.h  everything that decides something about a request
│   ├── liveserver.rc   embeds app_icon.ico into liveserver.exe
│   ├── app_icon.ico
│   ├── installer.iss   Inno Setup script
│   ├── build.bat       local build (MinGW)
│   └── build_msvc.bat  local build (MSVC)
├── tests/
│   ├── http_test.c     the request-handling tests
│   └── build.bat       builds and runs them
└── README.md
```

### Custom app icon

`liveserver.exe` and the installer both use `src/app_icon.ico` instead of the
generic Windows icon:

- `liveserver.rc` embeds it into `liveserver.exe` at build time (`windres` for
  MinGW, `rc.exe` for MSVC), so it appears in Explorer, the taskbar and
  Alt-Tab.
- The context-menu entries point at `"{app}\liveserver.exe,0"`, so they pick up
  the same embedded icon with no separate step.
- `installer.iss` uses it as `SetupIconFile`, so the installer, its Start Menu
  shortcut and its uninstaller entry all show it too.

To change it, replace `src/app_icon.ico` with another `.ico` of the same name —
ideally with several sizes from 16×16 to 256×256 — and push a new tag.

## Known limitations

- **No directory listing.** A folder with no `index.html` is a 404, not an
  index of its contents.
- **The injection point is found by text search.** A `</body>` inside a
  comment, a string literal or a `<pre>` block is matched. For a development
  tool that is acceptable; it is worth knowing when a page reloads oddly.
- **An HTML file over 64 MB is streamed without the reload script** rather than
  read into memory to inject into. It will still serve; it just will not
  auto-reload.
- **One thread per connection, one request per connection.** Fine for one
  developer and a handful of tabs. Not a production server, and it binds to
  loopback partly so it cannot be mistaken for one.


## Install (recommended — fully automated)

1. Go to the **Releases** page of this repo and download the latest
   `LiveServerSetup.exe`.
2. Run it. The installer will:
   - Ask where you want it installed.
   - Ask what default port to use (just a text box — nothing to edit in
     the registry by hand).
   - Copy `liveserver.exe` there, add that folder to your system `PATH`
     (so `liveserver` works from any terminal, in any folder, without
     typing the install path), and register **"Start Live Server Here"**
     on the right-click menu for you, both:
     - on a folder itself, and
     - inside a folder's empty background space.
3. That's it. Right-click any folder → **Start Live Server Here**.

To uninstall, use **Settings → Apps** (or Control Panel) like any other
Windows program — it removes the files and registry entries cleanly.

## Changing the port later

You entered a default port during install, but you're never locked into it
— it's just passed to `liveserver.exe` as a plain command-line argument
(`argv[2]`), not baked into the registry as a fixed value you'd need
`regedit` to change. Two ways to use a different port:

- **One-off**: the installer also adds itself to your system `PATH`, so
  you don't need to remember or type the install location — open any
  terminal, `cd` to a project folder, and run:
  ```
  liveserver . 8080
  ```
  (`.` means "serve the current folder"; you can also give any folder
  path instead of `.`.) If you just installed and `liveserver` isn't
  recognized yet, close and reopen your terminal — Windows only refreshes
  `PATH` for new windows.
- **New default**: re-run `LiveServerSetup.exe` — it will re-register the
  context menu entries with whatever port you enter.

---

## First-time guide: publishing a GitHub Release

This walks through everything from scratch — creating the repo, pushing
this code, and getting `LiveServerSetup.exe` built and published
automatically. You only need to do the one-time setup (Steps 1–4) once;
after that, shipping a new version is just Step 5 every time.

### What you need first

- A free [GitHub account](https://github.com/join).
- [Git](https://git-scm.com/downloads) installed on your computer.
  To check if you already have it, open a terminal (Command Prompt,
  PowerShell, or Git Bash) and run:
  ```
  git --version
  ```
  If it prints a version number, you're set. If not, install it and
  restart your terminal.

You do **not** need Windows, Visual Studio, or Inno Setup installed
locally — GitHub Actions does all the building on Microsoft's own
Windows servers, for free, every time you tag a release.

### Step 1 — Create the repository on GitHub

1. Go to [github.com/new](https://github.com/new).
2. Repository name: `live-server` (or anything you like).
3. Leave it **Public** or set it **Private** — either works, but if it's
   private, only you (and anyone you invite) can see or download releases.
4. **Do not** check "Add a README" or ".gitignore" — this project already
   has its own files, and an extra auto-generated README would conflict.
5. Click **Create repository**. GitHub will show you a page with setup
   instructions and a URL like:
   ```
   https://github.com/YOUR-USERNAME/live-server.git
   ```
   Keep this page open — you'll need that URL in Step 3.

### Step 2 — Get this project folder onto your computer

If you downloaded this as a `.zip`, extract it somewhere convenient, e.g.
`C:\Projects\Live Server` (Windows) or `~/Projects/Live Server` (Mac/Linux).
You should see the `.github`, `src`, and `README.md` items directly inside
that folder.

### Step 3 — Push it to GitHub

Open a terminal **inside the `Live Server` folder** (the one containing
`.github` and `src`) and run these commands one at a time:

```bash
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/YOUR-USERNAME/live-server.git
git push -u origin main
```

Replace the URL in the `git remote add` line with the one GitHub showed
you in Step 1. The first time you push, GitHub may ask you to sign in —
follow the on-screen prompts (a browser window will usually pop up).

When it finishes, refresh your repository's page on GitHub — you should
see the `.github`, `src`, and `README.md` files there.

> **Note on git and file paths:** if your project folder is literally
> named `Live Server` with a space in it, that's fine for git — just make
> sure your terminal is `cd`'d into that exact folder before running the
> commands above.

### Step 4 — Understand what happens next

This repo includes a GitHub Actions workflow
(`.github/workflows/release.yml`) that's already configured to:

1. Spin up a temporary Windows machine.
2. Compile `src/liveserver.c` into `liveserver.exe`.
3. Compile `src/installer.iss` (using free tool "Inno Setup", which the
   workflow installs automatically) into `LiveServerSetup.exe`.
4. Publish `LiveServerSetup.exe` as a downloadable **GitHub Release**.

This workflow runs automatically whenever you **push a version tag**
(like `v1.0.0`) — that's the trigger. You don't need to click anything in
the GitHub website for it to start (though there's also a manual button,
covered in Step 5b).

### Step 5 — Trigger your first release

You have two options. Option A (tags) is the standard way and is what
you'll use most of the time. Option B (manual) is a handy fallback if you
ever want to rebuild without creating a new tag.

#### Option A — Push a version tag (recommended)

Still inside your project folder in the terminal:

```bash
git tag v1.0.0
git push origin v1.0.0
```

That's it. This tells GitHub "build and release version 1.0.0."

#### Option B — Trigger it manually from the website

1. Go to your repository on GitHub.
2. Click the **Actions** tab (top menu bar of the repo).
3. In the left sidebar, click **Build and Release Live Server**.
4. Click the **Run workflow** dropdown button (top right of the list),
   type a version number like `1.0.0`, and click the green **Run
   workflow** button.

### Step 6 — Watch it build

1. Go to the **Actions** tab of your repository.
2. You'll see a new run in progress (a yellow dot = running, green check
   = success, red X = failed).
3. Click into it to watch the live logs if you're curious. A full build
   typically takes 2–4 minutes.

If it fails, click into the failed step to read the error message — the
most common first-time issue is a typo introduced while editing
`installer.iss` or `release.yml`. Fix it, commit, and push a new tag
(e.g. `v1.0.1`) to try again — you can't reuse a tag that already ran.

### Step 7 — Download your release

1. Once the run finishes with a green check, go to your repository's main
   page.
2. On the right-hand sidebar, click **Releases** (or go to
   `https://github.com/YOUR-USERNAME/live-server/releases`).
3. You'll see **Live Server v1.0.0** with `LiveServerSetup.exe` attached
   as a downloadable file. That's the installer described at the top of
   this README — download it and run it.

### Shipping updates later

Whenever you change `liveserver.c` or anything else:

```bash
git add .
git commit -m "Describe what changed"
git push
git tag v1.0.1
git push origin v1.0.1
```

Each new tag (bump the version number) produces a fresh
`LiveServerSetup.exe` published under its own release automatically —
no manual building, zipping, or uploading required.

## Licence

Free for personal, non-commercial use. You may use, study and modify this
software for yourself, but you may not use it commercially, and you may not
share or publish it, in its original form or modified. See [LICENSE](LICENSE)
for the full terms.
