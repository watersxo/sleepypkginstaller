## Sleepy Package Installer v1.14

### Fixed in 1.14
- **Installed update version.** `/api/installed` now reads the version from the title's app metadata as well as the patch folder, so the PC client can tell that an update is already installed.
- **Free space.** The free space of the console's drive is logged at every registration and heartbeat and reported in `/api/status` (`free_bytes`), so downloads that die with 0x80990019 can be matched to a full drive.

## Sleepy Package Installer v1.13

### Added in 1.13
- **`/api/installed`** reports the installed patch version and add-on content of a title (read from `/user/patch` and `/user/addcont`), so the PC client can skip updates and DLC the console already has.

## Sleepy Package Installer v1.12

### Fixed in 1.12
- **The log stopped part-way through a session.** The log file is now opened for every line instead of holding a file descriptor open, so it cannot be cut off if a descriptor is closed or reused elsewhere. Logs are at `/data/sleepy-installer.log` (and `.log.old`).

## Sleepy Package Installer v1.11

### Fixed in 1.11
- **Memory corruption when checking an installed game.** The size lookup for an installed title used a 4-byte variable although the system writes 8 bytes, overwriting the stack. This could cause random crashes, especially after the PC client checked many installed games.

## Sleepy Package Installer v1.10

### Added in 1.10
- **Log file for crashes.** The installer now writes `/data/sleepy-installer.log` (older part in `.old`). Every request, BGFT call, task change and a heartbeat every 30 s is written as it happens, so the last lines show what the app was doing even if it crashed. A crash also records the signal, fault address and call stack, and the next start notes that the previous run did not exit cleanly. The PC client can fetch it (Settings > Copy console log), or open `http://<ip>:<port>/api/log`.
- Invalid-request errors now record the exact body that was received.

### Changed in 1.10
- The BGFT download service gets an 8 MB heap (was 1 MB) so many queued or leftover tasks cannot exhaust it.
- All calls into the BGFT and AppInstUtil system services run one at a time instead of from several threads at once.

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
