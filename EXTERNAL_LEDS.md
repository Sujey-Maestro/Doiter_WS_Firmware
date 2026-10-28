# External Status LEDs

The onboard RGB NeoPixel has been removed from the firmware. Two separate external LEDs are controlled by ESP32-S3 GPIO pins.

## Pin assignments

| LED | ESP32-S3 pin | Firmware define |
|---|---:|---|
| Red | GPIO 6 | `RED_LED_PIN` |
| Green | GPIO 7 | `GREEN_LED_PIN` |

Both pins are configured as digital outputs during startup. The firmware initializes both LEDs off (`LOW`). The output is active-high: `HIGH` turns the LED on and `LOW` turns it off.

## Wiring

For each LED, connect:

1. GPIO pin to a suitable current-limiting resistor.
2. Resistor to the LED anode (the longer leg, commonly).
3. LED cathode to ESP32 ground (GND).

Use one resistor per LED. Do not connect an LED directly between a GPIO pin and ground. Confirm the LED current and resistor value are appropriate for the board's 3.3 V GPIO; do not use GPIO pins to power larger lamps or LED strips.

## Firmware behavior

- Receiving the exact network command `OK` turns on the green LED and turns off the red LED.
- Receiving any command containing `REJ` turns on the red LED, turns off the green LED, and pulses lock output 1 for its configured 2 seconds.
- After a `REJ`, a detected change on door sensor 1 that leaves it reading `HIGH` clears the red LED, unless the green status flag is still set.
- After `OK`, any detected change on door sensor 2 clears the green LED, unless the red status flag is still set.
- The LEDs start off after a reboot. They are not automatically turned off by a timer.

Door sensor 1 is GPIO 4 and door sensor 2 is GPIO 5. In this firmware, `HIGH` means the door is closed.

## Files

The assignments and control logic are in `src/main.cpp`. The Adafruit NeoPixel library was removed from `platformio.ini` because the onboard RGB LED is no longer used.
