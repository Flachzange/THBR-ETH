# THBR-ETH — a standalone Thread Border Router over Ethernet

THBR-ETH is a fork of [tostmann/THBR](https://github.com/tostmann/THBR)
that adds a physical W5500 Ethernet backbone to the ESP32-C6 firmware. The
target hardware is an ESP32-C6-Bug with an Eth-Bug-PoE board.

Most of this repository originates in the upstream THBR project: the original
ESP32-C6 Border Router firmware, the USB/TAP and PPP transports, the Home
Assistant add-on, Docker integration, BLE proxy and FHEM worked example were
designed and implemented there. This fork preserves that work and its Git
history. Its own scope is the W5500/Eth-Bug-PoE backbone, the associated build
profiles and standalone startup path, and the Ethernet/OMR diagnostics needed
to validate that configuration.

The complete OpenThread Border Router runs on the ESP32-C6. Ethernet and PoE
make it independent of Home Assistant, Docker, a TAP device or a permanently
connected USB host. USB remains available for building, flashing and the
diagnostic console.

The original USB/TAP and PPP transports are still present in the repository.
The W5500 profile is the primary configuration documented here.

## Current status

The W5500 implementation has been built with ESP-IDF 6.0.2 and tested on real
ESP32-C6-Bug + Eth-Bug-PoE hardware:

- W5500 link, DHCP, IPv4 and IPv6 work.
- The OpenThread Border Router reaches `br=running` and the Thread role
  `leader`.
- The Border Routing Manager advertises an OMR route into the infrastructure
  LAN.
- An OMR address on the ESP32-C6 is reachable from a Windows host over IPv6.
- The web/REST service and the diagnostic API are reachable over Ethernet.
- USB-Serial/JTAG remains attached as a diagnostic console.

End-to-end commissioning of a real Thread/Matter device is the next functional
test and is not claimed as completed yet.

## Architecture

```
Infrastructure LAN
       │
       │ Ethernet / IPv4 / IPv6
       │
    W5500  ←── PoE
       │ SPI
       │
  ESP32-C6-Bug                   optional USB connection
  OpenThread Border Router  ←──  build, flash and console only
       │
       │ IEEE 802.15.4 / Thread
       │
  Thread devices
```

The ESP32-C6 routes between the physical Ethernet interface and the Thread
mesh. A Matter server or other Matter controller is a separate application on
the infrastructure network; it is not part of the border-router firmware.

## Hardware

- ESP32-C6-Bug with ESP32-C6FH4 and 4 MB flash
- Eth-Bug-PoE with W5500 SPI Ethernet controller
- Ethernet connection to the infrastructure LAN
- PoE supply through the Eth-Bug-PoE board
- USB connection only when building, flashing or diagnosing

### W5500 pin assignment

| Signal | ESP32-C6 GPIO |
| --- | ---: |
| MISO | 2 |
| Interrupt | 4 |
| Chip select | 5 |
| SCLK | 6 |
| MOSI | 7 |
| Reset | not connected (`-1`) |

The configured SPI clock is 20 MHz. The DHCP hostname is `thbr-eth`.

## Backbone transports

Exactly one backbone implementation is compiled into a firmware image.

| Transport | Purpose | Permanent host required |
| --- | --- | --- |
| W5500 SPI Ethernet | Standalone physical Ethernet router | No |
| Ethernet over USB/TAP | Original THBR deployment with host TAP | Yes |
| PPP over USB | Point-to-point control/data transport | Yes |

The W5500 profile includes the existing THBR BLE proxy. Its Matter-server
endpoint remains empty by default and is configured at runtime through the
info API; it is not tied to a LAN address in the firmware image.

## Build requirements

The tested development environment is:

- Windows 11
- ESP-IDF 6.0.2
- Espressif VS Code extension 2.2.0
- VS Code C/C++ extension 1.33.8
- CMake 4.0.3
- Ninja 1.12.0
- Git for Windows 2.55.0
- build target `esp32c6`

The component manifest requires ESP-IDF 6.0 or newer. Hardware validation was
performed specifically with ESP-IDF 6.0.2.

## Build profiles

`CMakePresets.json` contains the original USB/TAP baseline and the standalone
W5500 production profile:

| Preset | Configuration |
| --- | --- |
| `thbr-c6-usb` | Original THBR build with USB/TAP, Border Router and BLE proxy |
| `thbr-c6-w5500` | W5500, OpenThread Border Router and Matter BLE proxy |

The W5500 profile layers these configuration files:

```text
sdkconfig.defaults + sdkconfig.defaults.w5500 + sdkconfig.defaults.br + sdkconfig.defaults.ble
```

The generated `sdkconfig.w5500` file and `build-thbr-c6-w5500` directory are
local build output and are ignored by Git.

## Building and flashing with VS Code

1. Open the repository in a VS Code window with the ESP-IDF 6.0.2 environment
   active.
2. Select `thbr-c6-w5500`.
3. Configure the selected CMake preset.
4. Build the project.
5. Flash the resulting image with the ESP-IDF extension.
6. Open the serial monitor at 115200 baud.

The flash baud rate in the presets is 460800 baud. The serial port depends on
the connected machine and must not be assumed to remain `COM7`.

The presets are intended to be selected through VS Code/CMake. When building
from a command line, use an explicit build directory and the same `SDKCONFIG`
and `SDKCONFIG_DEFAULTS` values instead of relying on `idf.py --preset`.

## Expected runtime behaviour

After Ethernet starts, the log reports the W5500 pin assignment, link state,
MAC address and DHCP lease. `br=stopped role=detached` is normal directly
after boot. On the tested hardware the router consistently reached
`br=running role=leader` after approximately 20 seconds.

The diagnostic API listens on port 8082. For example:

```
http://<DHCP-address>:8082/status
```

A healthy response resembles:

```json
{"fw":"0.1.46","uptime_s":295,"heap":353640,"heap_min":353184,"link":"up","br":"running","role":"leader"}
```

The OpenThread web/REST service uses the normal HTTP service on the device's
DHCP address.

## IPv6 and OMR routing

OpenThread distinguishes between addresses intended for the Thread mesh and
addresses reachable from the infrastructure LAN:

- The favored OMR prefix is advertised into the infrastructure network. Hosts
  on that network learn a route to it through the Border Router.
- Mesh-local addresses belong to the Thread mesh and are not advertised as an
  infrastructure route.
- Link-local addresses are valid only on their respective link.

The firmware asks OpenThread for the favored OMR prefix and classifies its
addresses accordingly. It does not infer address roles from a hard-coded ULA
prefix.

On the tested network, Windows learned a route for
`fd8b:eee7:2f81:1::/64` through the ESP32-C6's Ethernet link-local address, and
the OMR address `fd8b:eee7:2f81:1:4a06:b13:ec90:a4bb` answered with roughly
2 ms latency. These values are network-specific examples, not fixed project
configuration.

Useful Windows checks are:

```powershell
Get-NetRoute -AddressFamily IPv6 |
    Where-Object { $_.DestinationPrefix -match "^fd" }

ping -6 <OMR-address-reported-by-THBR>
```

Failure to find a LAN route to a mesh-local prefix is expected and is not a
Border Router fault.

## Important lwIP/OTBR compatibility setting

For the W5500 profile, `CONFIG_LWIP_PPP_SUPPORT` must be disabled.

ESP-IDF 6.0.2 supplies the OpenThread Border Router implementation as a
prebuilt library. Enabling lwIP PPP support changes the layout of lwIP's
`struct netif`, while the prebuilt OTBR library expects the layout without the
PPP client-data pointer. The resulting ABI mismatch makes OTBR read the wrong
infrastructure interface index and produces errors such as:

```
Invalid If index
Failed to get LL address
```

`sdkconfig.defaults.w5500` therefore sets:

```
CONFIG_LWIP_PPP_SUPPORT=n
```

The component CMake file also stops a W5500 build if PPP support is enabled,
so an incompatible firmware cannot be produced silently.

## Known harmless log messages

The following messages have been observed without affecting Border Router
operation:

- `IPv6 multicast is always filtered in by W5500` is driver information.
- Occasional `Dropping unsupported mldv2 record of type 2/6` messages are
  caused by MLDv2 traffic on the infrastructure LAN.

They require no action while the router remains `br=running` and the OMR route
and address are reachable.

## Original USB/TAP deployment

The Home Assistant add-on, Docker container and FHEM worked example are
inherited from the original THBR project and describe its USB/TAP backbone and
BLE-proxy architecture:

- [`addon/README.md`](addon/README.md)
- [`addon/DOCS.md`](addon/DOCS.md)
- [`fhem/README.md`](fhem/README.md)

Those documents do not currently describe the standalone W5500 profile. Use
the W5500 build and network instructions in this README for THBR-ETH.

## Project origin and branches

- Upstream project: <https://github.com/tostmann/THBR>
- THBR-ETH fork: <https://github.com/Flachzange/THBR-ETH>
- Default branch (including W5500 support): `main`
- W5500 development branch: `feature/w5500-backbone`
- Upstream integrated through release: `2026.10.46`

The W5500 implementation was developed incrementally and validated on physical
ESP32-C6-Bug and Eth-Bug-PoE hardware.

## Licence

**PolyForm Noncommercial License 1.0.0** — free for anything that is not
commercial. See [LICENSE](LICENSE).

The components this builds on keep their own licences, which these terms do
not narrow: OpenThread under BSD-3-Clause, the Espressif components under
Apache-2.0, cJSON under MIT, and esptool — shipped unmodified and run as a
separate program — under GPL-2.0-or-later. [NOTICE](NOTICE) lists them.
