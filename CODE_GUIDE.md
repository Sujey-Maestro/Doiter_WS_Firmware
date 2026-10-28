# ESP32-S3 Door Controller: Complete Guide

This guide describes the project as a working system: what hardware it expects, how it starts, how a network client talks to it, and what the controller does over time. You can use it without reading the C++ source.

## 1. What the system does

The firmware runs on an ESP32-S3 PoE development board. It watches two door sensors, accepts one TCP client over Ethernet, and switches two outputs through a TCA9554 I²C output expander. Those outputs are intended to pulse door lock or release hardware. Two ordinary external LEDs show status.

In short:

1. Door sensors report whether door 1 and door 2 are closed.
2. A computer or other controller connects to the ESP32 over Ethernet and sends text commands.
3. The ESP32 answers with text lines, reports door changes, and pulses lock outputs when asked.
4. The firmware also controls red/green status LEDs and emits an occasional `Alive` line during a specific idle window.

The firmware does not implement authentication, encryption, a web page, or automatic IP configuration. Anyone able to reach TCP port 5111 can issue commands.

## 2. Project files and build settings

- `src/main.cpp` contains all firmware logic.
- `platformio.ini` selects the `esp32-s3-devkitc-1` board, Arduino framework, USB serial settings, and the Ethernet library dependency.
- `EXTERNAL_LEDS.md` describes the external LED wiring and behavior.
- `include`, `lib`, and `test` contain PlatformIO template readmes; there are no additional project modules or tests in them.

Build and upload with PlatformIO using the `[env:esp32-s3-poe]` environment. Serial diagnostics use 115200 baud. The two USB build flags enable USB CDC at boot so the serial monitor can connect over native USB.

## 3. Hardware map

| Function | ESP32 pin / address | Meaning |
|---|---:|---|
| Door sensor 1 | GPIO 4 | `HIGH` means closed; `LOW` means open |
| Door sensor 2 | GPIO 5 | `HIGH` means closed; `LOW` means open |
| Ethernet chip select | GPIO 16 | W5500 SPI chip select |
| Ethernet SPI clock | GPIO 15 | W5500 SCLK |
| Ethernet SPI MISO | GPIO 14 | W5500 MISO |
| Ethernet SPI MOSI | GPIO 13 | W5500 MOSI |
| Ethernet reset | GPIO 39 | Reset driven low then high at startup |
| Red status LED | GPIO 6 | Active-high external LED |
| Green status LED | GPIO 7 | Active-high external LED |
| I²C SDA / SCL | GPIO 42 / GPIO 41 | TCA9554 bus |
| TCA9554 address | `0x20` | I²C address used by firmware |
| Lock output 1 | TCA9554 P0 | Bit 0; controlled as door 1 output |
| Lock output 2 | TCA9554 P1 | Bit 1; controlled as door 2 output |

Each external LED needs its own current-limiting resistor and a common ground. Do not connect an LED directly to a GPIO. The door inputs are configured as plain `INPUT`, without internal pull-ups or pull-downs. The sensor wiring must therefore provide a stable logic level; a floating input can produce false door changes.

The output expander is configured with all eight pins as outputs, even though this firmware only uses P0 and P1. Check the connected circuitry before using the remaining pins. The lock outputs are logic outputs, not a power source for a lock: use suitable driver/relay hardware and a separate appropriately rated supply where needed.

## 4. Network setup

The Ethernet controller is a W5500 connected by the SPI pins above. On startup, firmware resets it, starts SPI, selects the W5500, and configures a static network identity:

- IP address: `192.168.1.76`
- MAC address: `DE:AD:BE:EF:FE:ED`
- TCP server port: `5111`

The client must be on a network that can route to that IP. The firmware does not configure a subnet mask, gateway, or DNS server in its `Ethernet.begin` call, so the library's defaults apply. If you deploy another device with the same settings, assign it a unique IP and MAC to avoid conflicts.

The serial log reports whether Ethernet hardware is found and whether the cable link is on. Those messages are diagnostic: the code still calls `server.begin()` after reporting a missing chip or disconnected cable.

Only one client is actively handled at a time. When no active client exists, the loop checks for a new connection. A newly accepted client resets handshake and initial-state tracking and starts the idle timer from the connection time.

## 5. Door logic and message vocabulary

The physical input level is interpreted consistently throughout the code:

- `HIGH` = door closed
- `LOW` = door open

For door 1 and door 2, the network state words are `1DoorClosed`, `1DoorOpened`, `2DoorClosed`, and `2DoorOpened`. The combined forms `3DoorClosed` and `3DoorOpened` mean both doors are closed or both are open, respectively. They do not mean a third physical door.

The controller remembers the last sampled level for each input. When a level changes, it sends the corresponding single-door state line. At startup it samples the pins as the initial remembered state, so there is no change event just because the device booted.

After a successful handshake, the controller sends one combined initial-state message:

| Door 1 | Door 2 | Initial line |
|---|---|---|
| Closed | Closed | `3DoorClosed` |
| Open | Open | `3DoorOpened` |
| Open | Closed | `1DoorOpened` |
| Closed | Open | `2DoorOpened` |

## 6. TCP protocol

Commands are plain text, one command per line, terminated by a newline. Incoming lines are trimmed at both ends, so a trailing carriage return from a Windows-style line ending is okay. Replies are also newline-terminated. Command matching is case-sensitive except for the `REJ` search, which looks for those exact uppercase letters anywhere in the line.

### Start-up handshake

The client sends:

```text
MAE
```

If the handshake has not yet completed for this connection, the ESP32 replies:

```text
MAE WELCOME
```

It then sends the initial door state described above. The handshake state resets on a new client connection, not on every later command. Sending `MAE` again on the same connection does not produce another welcome.

### Door control commands

| Client sends | ESP32 action | Reply |
|---|---|---|
| `1DoorOpen` | Turn output 1 on for 2 seconds | `1DoorOpened` immediately |
| `2DoorOpen` | Turn output 2 on for 5 seconds | `2DoorOpened` immediately |
| `3DoorOpen` | Turn both outputs on, each for its own duration | `3DoorOpened` immediately |
| `1DoorClose` | Turn output 1 off and cancel its timer | `1DoorClosed` only if sensor 1 currently reads open (`LOW`) |
| `2DoorClose` | Turn output 2 off and cancel its timer | `2DoorClosed` only if sensor 2 currently reads open (`LOW`) |
| `3DoorClose` | Turn both outputs off and cancel both timers | `3DoorClosed` only if both sensors currently read open (`LOW`) |

The `Close` replies are conditional and may look counterintuitive: they are sent when the sensor says the door is *not* closed. If a sensor reads `HIGH`, the corresponding close reply is suppressed. These commands switch the lock output off; they do not move a door or verify a mechanical latch.

If an `Open` command is received again while its output is active, its timer starts over from the newest command. Door 1 and door 2 timers are independent. The code can therefore keep one output active while turning the other off.

### Status query

The client sends `status`. The ESP32 reads both sensors at that moment and replies as follows:

| Sensor 1 | Sensor 2 | Replies, in order |
|---|---|---|
| Open | Open | `3DoorOpened` |
| Closed | Open | `1DoorClosed`, then `2DoorOpened` |
| Open | Closed | `1DoorOpened`, then `2DoorClosed` |
| Closed | Closed | `3DoorClosed` |

Each reply is its own line. This query reports sensor state, not lock output state.

### Status LEDs

- `OK`: turn green on and red off; set the green status flag.
- Any line containing uppercase `REJ`: turn red on and green off, set the red status flag, and pulse output 1 for 2 seconds.

The flags are cleared by later sensor changes, not by a fixed timeout. A sensor 1 change that leaves sensor 1 `HIGH` clears the red flag and turns LEDs off if green is not flagged. A sensor 2 change clears the green flag and turns LEDs off if red is not flagged. The source checks for a change, so a command by itself does not immediately clear a flag even if the relevant sensor is already at the target level.

The flags are independent bookkeeping. For example, receiving `OK` after `REJ` turns the red LED off, but does not clear the red flag; later LED clearing depends on both flags as described above.

### Unknown input

Unrecognized lines update the receive/idle timer and appear in the serial log, but receive no network reply and cause no command action.

## 7. Lock output timing

The TCA9554 output register is register `0x01`; its configuration register is `0x03`. At startup the firmware writes `0x00` to the configuration register, making all expander pins outputs, and writes `0x00` to the output register, setting them all low/off.

For each output change, the firmware updates a RAM copy of the output byte and writes the whole byte to the expander. Bit 0 is output 1; bit 1 is output 2. Updating one bit preserves the other bits in that RAM copy. If an I²C write fails, a warning is printed, but the code does not retry or stop operation.

Pulse durations:

- Output 1: 2,000 ms
- Output 2: 5,000 ms

The main loop checks elapsed time using `millis()`, which is a rollover-safe pattern for these short durations. It turns an output off once its duration has elapsed. The loop is non-blocking during pulses, so it can continue reading commands and door sensors while a pulse runs.

## 8. What happens from power-on onward

1. Serial starts at 115200 baud; firmware waits up to three seconds for USB serial and then pauses briefly.
2. Door GPIOs are configured as inputs. LED GPIOs are configured as outputs and set off.
3. I²C starts on SDA 42 and SCL 41. The TCA9554 is set to all outputs and all outputs are initialized off.
4. The W5500 is reset, SPI is configured, and the static IP/MAC is assigned.
5. The TCP server begins listening on port 5111. Current door readings become the baseline readings.
6. The main loop periodically prints a “waiting for client” heartbeat while disconnected and accepts a client when available.
7. For an active client, each loop iteration handles available input, checks for door transitions, and manages the idle-alive behavior. Lock timers are checked on every loop, including when no client is connected.

## 9. The `Alive` behavior

Every received line (including an unknown command) sets the last-receive time to now and disables the alive cycle. After 60 seconds with no received line, the controller starts an alive cycle. It sends `Alive` every 30 seconds while that cycle is active.

The active cycle ends 60 seconds after it starts. Because the first send is eligible immediately, the usual sends are around the start and 30 seconds later. Then it stops until another 60-second idle period has passed. This is not a continuous 30-second heartbeat; it is a two-message window after each idle threshold. Any received line, including a reply to an earlier state event, restarts the idle waiting period.

## 10. Runtime details and practical limits

- The main loop is single-threaded: network parsing, door checks, alive messages, and timer checks take turns. There are no RTOS tasks defined by this project.
- The code reads one available newline-terminated client command per call to the client handler. The Arduino `readStringUntil` function may wait for a newline or its stream timeout if the peer sends only a partial line, briefly delaying other loop work.
- Door events are based on sampled digital levels. There is no debounce or filtering, so noisy contacts can generate repeated state messages.
- There is no command authentication, authorization, encryption, or rate limiting.
- There is no explicit recovery/reinitialization sequence if the W5500 or I²C expander fails after startup.
- The controller's `status` response and transition events use the door sensor inputs; neither confirms that a lock physically moved.
- The serial output is diagnostic only. Network commands and replies use the Ethernet TCP connection.

## 11. A typical conversation

Assume both doors are initially closed. A client connects and sends `MAE`:

```text
Client -> ESP32: MAE
ESP32 -> Client: MAE WELCOME
ESP32 -> Client: 3DoorClosed
```

The client requests door 1 to open:

```text
Client -> ESP32: 1DoorOpen
ESP32 -> Client: 1DoorOpened
```

Output 1 goes high for up to two seconds. If the sensor changes to `LOW`, the ESP32 also sends `1DoorOpened` as a sensor-change event. The same text can therefore describe either command acknowledgment or a later physical sensor state.

## 12. Key concepts to remember

1. `HIGH` means closed and `LOW` means open for both sensors.
2. TCP port 5111 carries newline-delimited text commands and responses.
3. `1DoorOpen`/`2DoorOpen` pulse outputs; they do not directly assert that the doors opened.
4. Output 1 times out after 2 seconds; output 2 after 5 seconds.
5. The `Alive` messages happen only during a bounded period after 60 seconds of silence.
6. GPIO door inputs need electrically stable levels, and lock loads need suitable external drivers.

