<p align="center">
  <img width="100%" alt="PS5 BD-Mute Daemon Banner" src="https://github.com/user-attachments/assets/78816a49-8f7f-43f5-ad27-f98e1c00dff6" />
</p>

# PS5 BD-Mute Daemon

A lightweight background service (daemon) for jailbroken PS5 consoles that automatically manages the power state of the Blu-ray drive when a game starts or exits — reducing noise and saving energy.

## 🎯 Why I Created This

I installed games like *GTA* via physical disc and still need the disc to launch them.  
Since I didn't want to dump everything to internal storage, I keep using the optical drive.  
To get rid of the constant drive noise and save power during long sessions, I wanted a way to automatically put the Blu-ray drive to sleep while playing.  
This daemon does exactly that.

## 💡 Features

- **Automatic Detection** – Monitors running applications via Title-IDs (`CUSA` / `PPSA`).
- **Drive Power Management** – Powers down the Blu-ray drive (`sceKernelIccControlBDPowerState`) while a game is running.
- **Auto-Restore** – Re-enables the drive when you return to the home screen.
- **Boot-time Mute** – Optional file `/data/bd_mute_enabled` forces the drive **OFF** immediately after the daemon starts.
- **Title-ID Exclusion List** – Ignore specific games via `/data/bd_mute_exclude.txt`.
- **TCP Control** – Full remote control via simple text commands on port **9033**.
- **Notifications & Logging** – On-screen system notifications + log file at `/data/bd_mute.log`.

## 📡 TCP Commands (Port 9033)

| Command   | Description                                      |
|-----------|--------------------------------------------------|
| `ENABLE`  | Activate automatic mute logic                    |
| `DISABLE` | Deactivate automatic mute logic (forces BD ON)   |
| `TOGGLE`  | Switch auto-mute on/off                          |
| `STATUS`  | Show current state                               |
| `BD_ON`   | Force Blu-ray drive power **ON**                 |
| `BD_OFF`  | Force Blu-ray drive power **OFF**                |
| `QUIT`    | Shut down the daemon                             |

**Example:**

    echo STATUS  | nc YOUR_PS5_IP 9033
    echo DISABLE | nc YOUR_PS5_IP 9033
    echo BD_OFF  | nc YOUR_PS5_IP 9033

## 📁 Boot-time Mute File

Create the file to force the drive off right after the payload starts:

    touch /data/bd_mute_enabled

- File **present** → BD is powered **OFF** immediately on daemon start.
- File **missing** → BD power state is left untouched at startup.

You can still turn the drive back on at any time with the TCP command `BD_ON`.

## 🚫 Title-ID Exclusion List

Games listed here are ignored by the auto-mute logic (BD stays untouched while they run).

Create / edit the file:

    /data/bd_mute_exclude.txt

One Title-ID per line. Comments and empty lines are allowed:

    # Games that should not trigger BD mute
    CUSA12345
    PPSA67890

- Max. 64 entries
- Lines starting with `#` or `;` are treated as comments
- Changes take effect after restarting the daemon

`STATUS` also reports how many titles are currently excluded (`exclude=N`).

## ⚠️ Known Issues

- **Persistent Disc Icon** – After re-enabling the Blu-ray drive (`BD on`), the disc symbol may remain visible at the top of the screen for a few minutes until the system refreshes its optical status.

## 🛠️ Technical Details

- Written in pure C.
- Uses `sysctl` + `sceKernelGetAppInfo` to detect running games.
- Dynamically resolves symbols from `libkernel.sprx` / `libkernel_web.sprx`.
- Lightweight TCP server (non-blocking) for remote control.
- Exclusion list loaded from file at startup (optional hardcoded defaults supported).

## 📥 Installation & Usage

A pre-compiled binary (`bd-mute.elf`) is included in the repository.

Load it with any of these methods:

- **Netcat:** `nc YOUR_PS5_IP PORT < bd-mute.elf`
- **Payload Manager / Homebrew Loader** – upload to `/data/pldmgr/payloads`
- **FTP** – upload to `/data/` and execute via your preferred method

## 📝 Logging

Runtime logs are written to:

    /data/bd_mute.log

## 👨‍💻 Author

- **Akut0**  
  [![Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/akut0)
