# HotSync

[Documentation](README.md)

Run Linux commands from the repository root.

## Linux host setup

### 1. Configure udev Rules

By default, Linux's `ModemManager` attempts to probe newly attached `/dev/ttyACM*` serial devices with AT commands. This will corrupt the Palm HotSync handshake.

Install the provided udev rule to disable ModemManager probing and create a convenient `/dev/palm-irda` symlink:

```bash
sudo cp udev/99-esp32-irda.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Ensure your user is part of the `dialout` group:
```bash
sudo usermod -aG dialout $USER
```
*(Log out and back in if you were not already in the dialout group).*

### 2. Verify Connection

Plug in the ESP32-C3 and check `dmesg`:
```bash
dmesg | tail -n 20
```
You should see:
```text
usb 1-1: new full-speed USB device number ... using xhci_hcd
cdc_acm 1-1:1.0: ttyACM0: USB ACM device
```
And `/dev/palm-irda` should point to the device:
```bash
ls -l /dev/palm-irda
```
Without the udev rule, use `/dev/ttyACM0` in the commands below.

## Performing a HotSync with Palm IIIx

### 1. Configure the Palm IIIx

1. On your Palm IIIx, open the **HotSync** app from the launcher.
2. Tap the connection dropdown below the HotSync icon (default is usually "Cradle / Cable").
3. Select **"IR to a PC/Handheld"**.

### 2. Start pilot-link on Linux

Start the command first; it waits for the Palm:
```bash
pilot-xfer -p /dev/palm-irda -b ./palm_backup
```
`pilot-xfer` will display:
```text
Listening for incoming connection on /dev/palm-irda...
```

### 3. Initiate HotSync

1. Align the Palm IIIx infrared port with the TFBS4711 transceiver (distance between 10 cm and 30 cm).
2. Tap the **HotSync icon** on the Palm screen.
3. The Palm and ESP32-C3 will discover each other (`XID`), connect (`SNRM` / `UA`), negotiate IrCOMM, and begin transferring databases.
4. The onboard LED on the ESP32-C3 will turn solid ON and flicker during active data transfer.

### Other Useful Commands

Each one is started the same way: run the command, then tap HotSync on the Palm.

| Task | Command |
|:-----|:--------|
| Back up everything | `pilot-xfer -p /dev/palm-irda -b ./palm_backup` |
| Install an application or database | `pilot-xfer -p /dev/palm-irda -i game.prc` |
| List what is on the Palm | `pilot-xfer -p /dev/palm-irda -l` |
| Restore a backup | `pilot-xfer -p /dev/palm-irda -r ./palm_backup` |

## Standalone sync over Wi-Fi

### Set the HotSync user name from the Palm

Open **wifi.setup → HotSync settings (user name)**, enter your name and tap **Save**. Disconnect **Prefs → Network**, then start HotSync with no desktop waiting. The adapter writes the name to an unnamed Palm before Memo Pad, Note Pad, Expenses or normal Drive sync runs. No desktop setup program is required. The setting also works when application conduits are off.

This is one shared setting for all conduits. It preserves the Palm's numeric user ID and desktop sync metadata, so working Tasks/Contacts links remain attached to the same identity. An already-named Palm keeps its name. The page shows the user detected at the last standalone HotSync.

Use up to **40 Palm characters**, in Western or Cyrillic encoding. For non-ASCII names, match the name encoding to the text settings of the conduits using it. Leave the configured name empty to disable automatic name setup. Older Drive-only fallback names are reused when they fit this limit.

For a reset Palm with existing backups, enter the **original backup user name** here and choose **Drive backup → Restore saved backups once**. That explicit recovery restores the saved numeric identity first, before automatic name setup. Changing an adapter setting does not rename a named Palm or move/rebind existing Google folders.

### Run the sync

The adapter gives a listening USB desktop priority. If no desktop answers within 1.5 seconds, it applies the shared user-name setup, restores/installs incoming Google Drive files, runs the enabled Mail, Tasks, Calendar, Contacts, Memo Pad, Note Pad and Expenses conduits in that order, then backs up the resulting Palm databases to Drive. An explicit full restore recovers identity before the user-name setup. Each service has its own switch in the Palm portal. A saved Wi-Fi network and account setup are required, but a desktop is not needed during sync.

Disconnect **Prefs → Network** before starting HotSync; the infrared link is shared. To sync with a desktop, start `pilot-xfer` or `jpilot` before tapping HotSync. Results appear in the Palm's HotSync log and the corresponding portal page.

## Related guides

- [Google Drive backup and restore](google/drive.md)
- [Mail](mail.md)
- [Google services](google/README.md)
- [Internet connection](internet.md)
- [Troubleshooting](troubleshooting.md)
