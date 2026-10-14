# Hardware and wiring

[Documentation](README.md)

**Parts**

- ESP32-C3 SuperMini board (other ESP32-C3 boards work with different pin numbers in `include/config.h`).
- Vishay TFBS4711 IrDA transceiver.
- 100 nF ceramic capacitor (recommended, close to the transceiver).
- For the stronger emitter supply (Option B below): one 15 Ω resistor and one 4.7 µF - 10 µF capacitor.
- USB-C cable.

**On the Palm**

- The connection type **"IR to a PC/Handheld"** must be available (Palm OS 3.3 and later include it).
- For Internet access: a browser or other network application that connects directly over TCP/IP.

## ESP32-C3 SuperMini Pinout & Connections

The **ESP32-C3 SuperMini** board layout makes connecting the TFBS4711 very simple, as GPIO 5, 6, and 7 are adjacent on the right header:

```
                  ESP32-C3 SuperMini Pinout
                       +-----------+
                       |  [USB-C]  |
             5V   --- [|           |] ---  GPIO 5  -------> TFBS4711 Pin 4 (SD)
            GND   --- [|           |] ---  GPIO 6  -------> TFBS4711 Pin 2 (TXD)
            3V3   --- [|           |] ---  GPIO 7  <------- TFBS4711 Pin 3 (RXD)
         GPIO 0   --- [|           |] ---  GPIO 8  (Onboard Blue LED, Active Low)
         GPIO 1   --- [|           |] ---  GPIO 9  (Boot Button)
         GPIO 2   --- [|           |] ---  GPIO 10
         GPIO 3   --- [|           |] ---  GPIO 20
         GPIO 4   --- [|           |] ---  GPIO 21
                       +-----------+
```

## Vishay TFBS4711 Pinout

| Pin | Name | Description | Connection |
|:---:|:----:|:------------|:-----------|
| **1** | **VCC2 / IRED Anode** | IR Emitter Power | **3V3** directly (Option A), or **5V** through a 15 Ω resistor (Option B). **Do NOT connect to 5V without a resistor!** |
| **2** | **TXD** | Transmitter Data Input | **GPIO 6** (Active HIGH optical pulse input) |
| **3** | **RXD** | Receiver Data Output | **GPIO 7** (Uses ESP32 internal pull-up, no external resistor required) |
| **4** | **SD** | Shutdown (Active HIGH) | **GPIO 5** (Firmware drives LOW to activate transceiver) |
| **5** | **VCC1** | Logic Supply (2.4V - 3.6V) | **3V3** |
| **6** | **GND** | Ground | **GND** |

## Option A: Minimal Wiring (no external parts)

Six wires and nothing else: both supply pins of the transceiver go to 3V3.

```
                 +-----------------------------+
                 |     ESP32-C3 SuperMini      |
                 |                             |
                 |      GPIO 5 (Right Pin 1)   |--------> SD   (Pin 4)
                 |      GPIO 6 (Right Pin 2)   |--------> TXD  (Pin 2)
                 |      GPIO 7 (Right Pin 3)   |<-------- RXD  (Pin 3)
                 |                             |
                 |         3V3 (Left Pin 3)    |---+----> VCC1 (Pin 5)
                 |                             |   +----> VCC2 / IRED Anode (Pin 1)
                 |                             |
                 |         GND (Left Pin 2)    |--------> GND  (Pin 6)
                 +-----------------------------+
```

## Option B: Separate Emitter Supply (recommended with Wi-Fi)

The IR emitter draws short, high current pulses. Feeding it from the 5V pin through a resistor, with a capacitor next to the transceiver, keeps those pulses off the board's small 3.3 V regulator, which also has to supply the Wi-Fi radio.

```
                 +-----------------------------+
                 |     ESP32-C3 SuperMini      |
                 |                             |
                 |      GPIO 5 (Right Pin 1)   |--------> SD  (Pin 4)
                 |      GPIO 6 (Right Pin 2)   |--------> TXD (Pin 2)
                 |      GPIO 7 (Right Pin 3)   |<-------- RXD (Pin 3)
                 |                             |
                 |         3V3 (Left Pin 3)    |---+----> VCC1 (Pin 5)
                 |                             |   |
                 |                             |  --- 100nF Ceramic
                 |                             |  ---
                 |                             |   |
                 |         GND (Left Pin 2)    |---+----> GND  (Pin 6)
                 +-----------------------------+   |
                                                   |
                     5V (Left Pin 1)               |
                          |                        |
                        [ 15 Ohm ]                 |
                          |                        |
                          +------------------------+----> VCC2 / IRED Anode (Pin 1)
                          |                        |
                        ===== 4.7uF - 10uF         |
                          |                        |
                         GND                      GND
```

## Status LED

| LED | Meaning |
|:----|:--------|
| Slow blink (once per second) | Idle, waiting for the Palm |
| Solid on | IR link to the Palm is established |
| Flicker | Frames or data are being transferred |

## Related guides

- [Building and flashing](building.md)
- [Configuration](configuration.md)
- [Troubleshooting](troubleshooting.md)
