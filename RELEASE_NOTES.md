## Sleepy Package Installer v1.06

Install `.pkg` files on a jailbroken PS4 from your PC. Install the attached `.pkg` on the console, open it, and use the Sleepy PC client (or any sender) with the IP and port shown on screen.

### Fixed in 1.06
- **"Unable to load system file object for package"** on every package. The cause was the build, not your packages: the OpenOrbis 0.5.3 headers describe the file `stat` structure incorrectly, so the installer read wrong file sizes. The installer no longer uses `stat` anywhere (param.sfo loading, file serving, temp-file cleanup).
- Errors about a package's `param.sfo` now say exactly what is wrong (sizes, offsets, signature) instead of a generic message.
- param.sfo files whose entries report a smaller maximum size than the stored size are accepted.

### Changed in 1.05
- The default port is now 12800 (was 12801). If it is taken the next free port is used; set a preferred port in `/data/rpi.port`.

### Fixed in 1.04
- The installer works even if the PC-side server ignores HTTP Range requests, the package icon can no longer fail an install, and a bad or missing `param.sfo` reports exactly what was received.

### Since 1.03
- On-screen status: IP and port, server state, uptime, and every download with progress, speed, ETA and error codes.
- Status page in the browser at `http://<ip>:<port>/` and JSON at `/api/status`.
- Installing an already installed package no longer returns `task_id: -1` as a success.
- Stability: no more 100% CPU loop, leaked threads/buffers or SIGPIPE crashes; socket/HTTP timeouts and limits.
- Faster package metadata fetch, HEAD requests for piece sizes, `-O2` build.
