<p align="center">
  <img width="100%" alt="PS5 BD-Mute Daemon Banner" src="https://github.com/user-attachments/assets/78816a49-8f7f-43f5-ad27-f98e1c00dff6" />
</p>

# PS5 BD-Mute Daemon

A lightweight background service (daemon) for jailbroken PS5 consoles that automatically manages the power state of the Blu-ray drive when a game starts or exits to reduce noise and save energy.

## 🎯 Why I Created This / Background

I installed games like *GTA* via physical disc and still need to use the disc to launch them. Since I didn't want to go through the hassle of dumping the game to the internal storage, I keep using the CD. However, to eliminate the constant drive noise and save energy during long gaming sessions, I wanted a way to automatically put the Blu-ray drive to sleep while playing. This daemon solves exactly that problem!

## 💡 Features

- **Automatic Detection:** Monitors active applications via their Title-IDs (`CUSA` / `PPSA`).
- **Drive Power Management:** Powers down the Blu-ray drive (`sceKernelIccControlBDPowerState`) while a game is running. This eliminates optical drive noise and helps save power during long gaming sessions.
- **Auto-Restore:** Automatically re-enables the drive once you return to the home screen.
- **Notifications & Logging:** Sends on-screen system notifications and writes activity logs to `/data/bd_mute.log`.

## ⚠️ Known Issues

- **Persistent Disc Icon:** After re-enabling the Blu-ray drive (`BD on`), the disc symbol may remain visible in the top of the screen for a few minutes before the system refreshes its optical status.

## 🛠️ Technical Details

- Written in C.
- Uses `sysctl` to scan active processes (`sceKernelGetAppInfo`).
- Interfaces directly with PS5 system libraries (`libkernel.sprx` / `libkernel_web.sprx`).

## 📥 Installation & Usage

A pre-compiled version (`bd-mute.elf`) is already included in the repository, so you don't need to compile it yourself!

Transfer and execute the binary using one of the following methods:
- **Netcat (`nc`):** Send the payload directly to your PS5's IP address on the designated port (e.g., `nc <PS5_IP> <port> < bd-mute.elf`).
- **Payload Manager:** Load the pre-compiled payload through your preferred homebrew loader/manager interface on the console.
- **FTP:** Upload the file manually to `/data/`.

## 📝 Logging
You can check runtime logs and status updates directly on the console at:
`/data/bd_mute.log`

## 👨‍💻 Author
- **Akut0**