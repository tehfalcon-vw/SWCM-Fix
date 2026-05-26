STM32 Nucleo-C542RC firmware for translating VW multifunction steering
wheel control module CAN button events into Audi compatible steering wheel commands.

Example: VW 1K0 SWCM installed in Audi A3 8P

## Hardware

- STM32 Nucleo-C542RC
- 5V Buck Converter

## Installation

Connect STM32 CanH/CANL at CANBUS Gateway (J533) on Comfort CAN. 

Connect 12v Power to 5v Buck Converter to Terminal 15 (ign) power source, then 5v buck converter to the STM32.

Stable Firmware to Flash to the STM32 is located on the releases page.

## Limitations
VW SWCM firmware is unable to recognize the roller switch inputs.

## Current Mappings

| VW input | Audi output |
| --- | --- |
| `0x5C1 01000001` mode | `0x5C3 3907` volume down |
| `0x5C1 04000001` voice | `0x5C3 3906` volume up |
| `0x5C1 08000001` mute | `0x5C3 3902` track previous |
| `0x5C1 02000001` ok | `0x5C3 3903` track next |

Each Audi command is sent twice, then released with `0x5C3 3900`.

## Build

This project is a Zephyr application. Set up a Zephyr workspace next to this
repository as `zephyr-workspace`, create a Python environment at `.venv`, and
install `west` plus the Zephyr Python requirements.

Build:

```sh
./scripts/build-stm32.sh
```

Flash:

```sh
./.venv/bin/pyocd flash -t STM32C542RCT6 build/can_sniff_inject/zephyr/zephyr.hex
```

Monitor:

```sh
./scripts/monitor-stm32.sh /dev/cu.usbmodemXXXX
```

## Serial Commands

- `stats`
- `sniff on`
- `sniff off`
- `translate on`
- `translate off`
- `send <std-id-hex> <data-hex>`

Translation is enabled at boot. Sniff printing is disabled at boot.
