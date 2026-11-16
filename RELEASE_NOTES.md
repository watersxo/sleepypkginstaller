## Sleepy Package Installer v1.03

Install `.pkg` files on a jailbroken PS4 from your PC. Install the attached `.pkg` on the console, open it, and point your PKG sender at the IP and port shown on screen.

### New
- On-screen status: IP and port, server state, uptime, and every download with progress, speed, ETA and error codes.
- Status page in the browser at `http://<ip>:<port>/` and JSON at `/api/status`.
- If port 12801 is taken the next free port is used; set a preferred port in `/data/rpi.port`.

### Fixed
- Installing an already installed package no longer returns `task_id: -1` as a success.
- 100% CPU busy loop, leaked connection threads and buffers, SIGPIPE crashes when a client disconnects.
- Large (many-piece) installs, truncated GET data, silently swallowed HTTP errors.

### Faster
- Package metadata is read from a single prefetched chunk instead of one HTTP(S) connection per item.
- HEAD requests for piece sizes, bulk request parsing, 64 KB file chunks, `-O2` build.

### Safer
- Socket and HTTP timeouts, connection and request size limits.
- `/static/` only serves the installer's temporary `tmp_*` files.
