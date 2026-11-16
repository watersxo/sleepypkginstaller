## Sleepy Package Installer v1.09

### Fixed in 1.09
- **Updates no longer look stuck at 98-99%.** Updates for an installed game finish installing by themselves. A download that reaches 97% or more and stops moving (or whose task the system removes) is now reported as complete, and the "Close this app, then Notifications > Downloads > Install" message is gone.

## Sleepy Package Installer v1.08

Install `.pkg` files on a jailbroken PS4 from your PC. Install the attached `.pkg` on the console, open it, and use the Sleepy PC client (or any sender) with the IP and port shown on screen.

### Fixed in 1.08
- **Clean shutdown.** The screen and download-monitor threads are now stopped, and the display memory released, before the app exits or the system closes it. Closing the installer (which the PS4 asks for when you press Install on an update in Notifications) previously left those threads running while the system and network libraries were being torn down.
- **Updates.** Updates are registered as packages: they download to about 99% and are then finished from the PS4 (close this app, then Notifications > Downloads > Install). The on-screen status and the PC client now say so instead of looking stuck, and the PC client marks the update as ready so the queue carries on.

### Fixed in 1.07
- Crash (CE-34878-0) as soon as a download starts: the BGFT progress structure was too small.

### Fixed in 1.06
- "Unable to load system file object for package": file sizes were read wrongly because of a bad `stat` definition in the toolchain headers.

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
