# THBR reset diagnostics and restart-path audit

Scope: `Flachzange/THBR-ETH` W5500 standalone profile, reviewed against
`main` at `665ec8311452edc36c1b89ed6a933f843ed60e2e` (2026-11-14).
The bundled REST implementation is pinned in `main/idf_component.yml` to
`tostmann/esp-thread-br` commit
`4702be7882b6abc5d10d5934ce7ff4ec108e9119`.
ESP-IDF version in the tested configuration: 6.0.2.

## Persistent boot diagnostics

The firmware calls `info_server_init_boot_diagnostics()` once after NVS
initialization. `GET http://<device-ip>:8082/status` exposes:

| Field | Meaning |
| --- | --- |
| `uptime_s` | Seconds since this boot; resets to zero on every restart |
| `boot_count` | Persistent, monotonically increasing count of boots since the diagnostic NVS key was created |
| `boot_count_valid` | Whether updating the count in NVS succeeded on this boot |
| `reset_reason` | ESP-IDF `ESP_RST_*` name returned by `esp_reset_reason()` |
| `reset_reason_code` | Numeric ESP-IDF reset reason |
| `reboot_requested` | True only if a software reset followed a persisted `POST /reboot` marker |

The existing status fields (`fw`, `heap`, `heap_min`, `link`,
`br`, `role`) remain unchanged. This is a read-only GET endpoint;
polling `/status` does not request a reboot.

NVS namespace: `thbr_diag`; keys `boots` (u32) and `api_reboot` (u8).
The counter is updated once per boot, rather than once per HTTP poll.
The reboot marker is written just before the explicit `esp_restart()`
and consumed on the next boot. Diagnostics do not abort if NVS
operations fail. A failed marker write can cause a false negative for
`reboot_requested`; a failed boot-counter write is indicated by
`boot_count_valid=false`. Do not use `erase_flash` or erase NVS while
investigating a reboot: OpenThread also stores its network dataset in NVS.

Examples of interpretation:

- `ESP_RST_SW`, `reboot_requested=true`: request to the info API caused
  the software restart.
- `ESP_RST_SW`, `reboot_requested=false`: software restart without a
  confirmed API marker. This does not, by itself, identify the caller.
- `ESP_RST_TASK_WDT` or `ESP_RST_INT_WDT`: watchdog path; investigate
  blocked tasks and locks, not just the watchdog itself.
- `ESP_RST_PANIC`: panic/exception/abort path; a fresh core dump might
  be present, but confirm its ELF SHA matches the running firmware.
- `ESP_RST_BROWNOUT`, `ESP_RST_PWR_GLITCH`, `ESP_RST_POWERON`: power
  or reset-related indications. They do not, by themselves, prove
  which component of the PoE power chain is responsible.

The reset reason identifies the *kind* of reset, not necessarily its
underlying root cause. A stale core dump can survive later ordinary
restarts and must not be attributed to the newest boot without
matching the ELF hash and other evidence.

## Audit of ways the device can restart or stop serving Thread

| Code path | Trigger and behavior | Relevant to standalone W5500? |
| --- | --- | --- |
| `main/info_server.c`: `reboot_post`, `reboot_task` | `POST /reboot` to info API port 8082; after 500 ms explicitly invokes `esp_restart()` | Yes, if *any* reachable client sends this request |
| `addon/thbrctl.py`: `reboot_stick` | Host-side recovery sends `POST /reboot` after repeated failed mesh reachability checks (polling interval 300 s, preliminary link-recovery attempt) | Only if that optional host controller is actually running and targets this device |
| `addon/webui.py`: `api/reboot` | Host-side UI button forwards a reboot request | Only if that optional host web UI is in use |
| `main/thbr_main.c`: startup `ESP_ERROR_CHECK` | Errors from NVS, logging, event loop, OpenThread/mDNS setup or W5500 `backbone_start()` cause `abort()`, handled by ESP-IDF panic policy | Yes, chiefly during initialization; this is not a recovery loop intentionally rebooting in steady state |
| `main/thbr_main.c`: heartbeat task | Main task is enrolled in task watchdog; it feeds the watchdog every ~10 s but subsequently waits for OpenThread locks with `portMAX_DELAY` | Yes. With the observed 30 s task-watchdog timeout and panic enabled, lock starvation is a plausible reset mechanism; not proven |
| Pinned `esp_ot_br_server/src/esp_br_web.c`: `esp_br_web_start()` | `ESP_ERROR_CHECK` on registration of two IP event handlers can panic on failure | Yes, during REST-server initialization, not normal OTA transfer |
| External ESP-IDF/OpenThread components and hardware | Unhandled assertions, stack corruption, interrupt WDT, brownout, power glitches, CPU lockup and other causes | Possible; this audit cannot exclude these without a fresh reset reason, relevant logs or a matching dump |

Relevant negative findings:

- No `esp_restart()` or `abort()` invocation was found in the reviewed
  `main/w5500_link.c`, `main/ble_proxy.c`, `main/eth_link.c`,
  `main/ppp_link.c`, `main/heap_probe.c`, or `main/log_sink.c`.
- An error from a W5500 setup operation propagates through
  `ESP_RETURN_ON_ERROR` and reaches `ESP_ERROR_CHECK(backbone_start())`
  at startup; this is an indirect panic path, not a reconnect/restart timer.
- `POST /ble_proxy` stores a setting for the *next* boot; it deliberately
  does not restart the firmware.
- The pinned REST server's `DELETE /node` endpoint disables Thread/IP6
  and calls `otInstanceErasePersistentInfo()`. This can destroy the
  active Thread dataset and make the mesh disappear, but it is **not**
  an `esp_restart()` in the reviewed implementation.
- The existing FHEM HTTPMOD monitor uses `GET /status`, so its polling
  cannot directly invoke the reboot endpoint.

## Security finding: unauthenticated reboot endpoint

The `POST /reboot` handler currently performs **no authentication**.
Any client with TCP access to port 8082 can request a device reboot.
This audit does not establish that any such request occurred.

This feature branch does not change access control, because doing so
could break existing management clients. A separate decision is needed
on whether to restrict port 8082 at the network layer or introduce an
authenticated reboot action.

## What still requires validation

- An ESP-IDF 6.0.2 W5500 build with the actual local
  `sdkconfig.w5500` (`THBR_TRANSPORT_W5500=y`,
  `LWIP_PPP_SUPPORT=n`), followed by the hardware test.
- Confirm that `/status` returns the new reset diagnostics and that
  `boot_count_valid` is true.
- Preserve the resulting `thbr.elf` alongside the flashed binary.
- Correlate the next observed boot-counter increment with `reset_reason`,
  `reboot_requested`, Matter.js OTA logs and, for a panic, a *new*
  matching core dump.

The exact binary behavior of Espressif's prebuilt OpenThread Border
Router library was not disassembled or exhaustively audited here.

Source references:

- [THBR firmware entry point](../main/thbr_main.c)
- [THBR info API](../main/info_server.c)
- [Host recovery logic](../addon/thbrctl.py)
- [Host web UI](../addon/webui.py)
- [Pinned REST dependency](../main/idf_component.yml)
- [ESP-IDF 6.0.2 reset-reason enum](https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_system/include/esp_system.h)
