## Sleepy Package Installer v1.07

Install `.pkg` files on a jailbroken PS4 from your PC. Install the attached `.pkg` on the console, open it, and use the Sleepy PC client (or any sender) with the IP and port shown on screen.

### Fixed in 1.07
- **Crash (CE-34878-0) as soon as a download starts.** The OpenOrbis header describes the BGFT progress structure with 32-bit fields (48 bytes) while the console writes 64-bit fields (64 bytes), so every progress query overwrote the stack. Progress is now read into a correctly sized buffer. This also fixes the downloaded/total sizes, which were garbage before.

### Fixed in 1.06
- "Unable to load system file object for package": the toolchain headers describe the file `stat` structure incorrectly, so file sizes were read wrongly. The installer no longer uses `stat` anywhere.
- `param.sfo` problems now say exactly what is wrong.

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
