# STM32 RNS-E CAN translator

Target: `NUCLEO-C542RC`

Default mode:

- Classic CAN, 100000 bit/s
- Onboard FDCAN transceiver on CN18
- Translation enabled at boot
- Live sniff printing disabled at boot
- Serial sniff output uses an SLCAN-like form:
  - `C:t1238...` for standard IDs
  - `C:T000001238...` for extended IDs

Current production mappings:

| VW input | RNS-E output |
| --- | --- |
| `0x5C1 01000001` mode | `0x5C3 3907` volume down |
| `0x5C1 04000001` voice | `0x5C3 3906` volume up |
| `0x5C1 08000001` mute | `0x5C3 3902` track previous |
| `0x5C1 02000001` play/pause | `0x5C3 3903` track next |

Each RNS-E command is sent twice about 100 ms apart, then released with
`0x5C3 3900`.

Ignored inputs:

- `0x3C1` is intentionally not used. It was observed changing during normal bus
  activity and caused false triggers.
- NAV is intentionally unmapped. It did not produce a reliable `0x5C1` signal in
  the captures.

Serial commands:

- `stats`
- `sniff on`
- `sniff off`
- `translate on`
- `translate off`
- `send <std-id-hex> <data-hex>`

Example:

```text
send 5C1 01020304
```

Build:

```sh
../scripts/build-stm32.sh
```

Flash:

```sh
../.venv/bin/pyocd flash -t STM32C542RCT6 ../build/can_sniff_inject/zephyr/zephyr.hex
```
