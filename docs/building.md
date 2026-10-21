# Building and flashing

[Documentation](README.md)

Run commands from the repository root.

## Software requirements

- [PlatformIO Core (CLI)](https://platformio.org/install/cli) or VS Code with the PlatformIO IDE extension, to build and flash.
- For HotSync: a Linux computer with `pilot-link` (`sudo apt-get install pilot-link`) or `jpilot`.

## 1. Build the Firmware
```bash
pio run
```
The first build downloads ESP-IDF and compiles it from source (see `sdkconfig.defaults`), which takes a few minutes; the Wi-Fi gateway needs PPP and NAT support that the prebuilt Arduino SDK does not include. Later builds are fast.

## 2. Flash to ESP32-C3

Connect the ESP32-C3 to your computer via USB:
```bash
pio run -t upload
```
If the upload cannot reach the board, hold the **BOOT** button while plugging in the USB cable and try again.

After flashing, the blue LED blinks slowly: the adapter is waiting for the Palm.

## Related guides

- [Hardware and wiring](hardware.md)
- [Configuration](configuration.md)
- [Host-side tests](development.md#host-side-tests)
