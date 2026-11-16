# Sleepy Package Installer: PC client

A native Windows app (WPF, no browser, no runtime to install) that pairs with **Sleepy Package Installer** on your PS4.

- **Scan folders** for `.pkg` files (recursive and cached, so rescans only read files that changed). Pick folders with the standard Windows folder dialog, or drag them onto the window.
- **Games are aggregated**: you only see base games. The newest update and all DLC are attached automatically, and "Contents" lets you tick exactly what to send.
- **Bulk send** with an ordered queue (game → update → DLC): pause, resume, cancel, retry.
- **Live progress**, speed and time left, straight from the console.
- **Connect** button: *Find my console* scans your network, or type the IP shown on the console. It reconnects automatically next time.
- Shows which games are **already installed** on the console (installed games only get their update and DLC).
- **Fast LAN transfer**: the console pulls packages straight from your PC using HTTP range requests, keep-alive and large socket buffers.
- One ~0.5 MB `SleepyClient.exe`. Needs only .NET Framework 4.8, which is part of Windows 10/11.

## Use

1. Open **Sleepy Package Installer** on the PS4.
2. Run `SleepyClient.exe`, click **Connect** (top right) and press **Find my console** (or type the IP).
3. Click **Folders** and add the folder(s) with your packages (or drop a folder on the window).
4. Tick games (or press **Send** on a card), then **Send to console**.

Windows may ask to allow the app through the firewall the first time. Allow it on **private networks**, otherwise the console cannot download from your PC.

Closing the window while transfers are running asks for confirmation, because the console downloads from this PC.

## Notes

- Packages are grouped by title ID using `param.sfo` (`gd` = game, `gp` = update, `ac` = DLC).
- Incomplete or split packages (file smaller than the size in the package header) are flagged and skipped.
- The package server only answers devices on your local network.
- Settings and the library cache live in `%APPDATA%\SleepyPackageInstaller`.

## Build

Needs the .NET SDK (8.0 or newer):

```
cd client
dotnet build -c Release
```

The exe is `bin\Release\net48\SleepyClient.exe`.
