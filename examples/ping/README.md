# Ping example

A minimal ESP32 Discord bot built on this repository's transport
framework. It registers one slash command, `/ping`, and answers it
through the framework's automatic interaction acknowledgement. The
example is a standalone ESP-IDF project: it carries its own copy of
the board bring-up (config validation, serial provisioning of the
bot token and the Wi-Fi credentials, SNTP, the Wi-Fi station), the
same way `main/` does, and reaches the framework components through
`EXTRA_COMPONENT_DIRS`.

The command needs no privileged intents: the feature registers with
`required_intents = 0`, so the IDENTIFY the gateway sends carries an
empty intent mask.

## How the reply works

The reply to `/ping` is not produced by the feature handler. The
flow is:

1. Discord sends `INTERACTION_CREATE` (type 2, slash command) over
   the gateway WebSocket.
2. The gateway decoder (`discord_ws_decode.c`, `deliver_interaction`)
   parses the interaction, then enqueues a `DiscordAck` into the ACK
   queue BEFORE the interaction handler runs. For a type 2 command
   the ACK kind is `DC_WS_ACK_MESSAGE`, interaction response type 4
   (`CHANNEL_MESSAGE_WITH_SOURCE`).
3. The feature handler (`ping_on_interaction`) runs on the gateway
   connection task and returns immediately. It never blocks: the
   handler runs on the `esp_websocket_client` task, and the
   framework's contract is that it only copies values and calls the
   owning feature's callback.
4. The ACK task (`dc_ack`, priority 16) pops the queue and POSTs
   `{"type":4}` to `/interactions/{id}/{token}` through
   `dc_rest_post_interaction`, the single-shot path with Discord's
   3 second interaction deadline. A failed ACK is dropped, never
   retried: an answer after the deadline is an interaction Discord
   never received.

So the user-visible reply is the ACK itself: an empty
`CHANNEL_MESSAGE_WITH_SOURCE` response. The feature handler could
add work here (queueing a REST message, reading bot state) as long
as it returns immediately; this example deliberately does nothing.

The slash command name is the feature name. The registry routes an
interaction by matching `it->command` against the registered feature
names (`dc_registry_route_interaction`), so the feature's `.name`
must match the command registered on the Discord application side.

## Registering the command

The firmware handles `/ping`; it does not create the command on the
Discord side. Register a slash command named `ping` for the
application in the Discord Developer Portal (or through the
application commands REST API) before testing. The feature name in
`ping.c` and the command name in the portal must match exactly.

## Build and run

The example is self-contained; `IDF_PATH` must point at an ESP-IDF
installation (v6.1 is what the framework is developed against):

    cd examples/ping
    idf.py set-target esp32
    idf.py build
    idf.py flash monitor

On first boot the serial console prompts for the bot token, then the
Wi-Fi SSID and password; both are stored in encrypted NVS and never
appear in source, logs or build artifacts. After the store the
device reboots and picks the credentials up from NVS, so a routine
reflash does not ask again.

Boot milestones on the serial console, in order: the `boot` heap
line, `wi-fi up`, `after wifi` heap line, `sntp synced`, then
`ping example: /ping registered`. The SNTP wait is bounded at 15 s
and the Wi-Fi IP wait at 30 s, both polling at 100 ms; a timeout
stops the boot there.

## Latency record

| Measurement | Deadline (ms) | Measured (ms) |
| --- | --- | --- |
| `/ping` slash command to ACK POST | 3000 | not measured yet |

The measured cell is empty because the number requires hardware: an
ESP32 on Wi-Fi with a provisioned token, a real Discord application
and the serial console. No latency figure is asserted here, and none
should be quoted from this README.

How to measure it: the quantity is the time from the slash command
being issued in Discord to the ACK POST leaving the device. The two
halves are observable on the serial console: the monitor task's
telemetry (the `dc_bot` task logs free heap and the ACK and job
queue depths every 60 s, and the `dc_monitor` task logs the five
stack high-water marks and heap telemetry every 60 s) plus the REST
round trip of the ACK POST. The cleanest instrument is a serial log
line around the ACK path (in the ACK task or `dc_rest_post_interaction`)
if one is added; the framework ships no per-ACK timestamp today.
Record the measured value in the table above after the hardware run.

## Hardware verification

The following can only be checked on real hardware and are not
verified by this repository's stub compile:

- [ ] The real `idf.py build` of this example, including the
      partition table and `sdkconfig.defaults` copies at the example
      root.
- [ ] The SNTP sync sequence: `wi-fi up`, then `sntp synced` within
      the 15 s budget, before `discord init`.
- [ ] The gateway accepts a bare `{"type":4}` response body: `/ping`
      in Discord produces the empty acknowledgement reply within the
      3 second deadline.
- [ ] The measured latency, recorded in the table above.

## Project layout

- `CMakeLists.txt` sets `EXTRA_COMPONENT_DIRS` to the repository's
  `components/` directory, which is what makes the framework visible
  to this standalone project.
- `main/CMakeLists.txt` registers `ping.c` with the same `REQUIRES`
  list as the reference firmware in `main/`.
- `sdkconfig.defaults` and `partitions.csv` are copies of
  the project root and `main/` versions: the example carries
  the same memory constraints (dynamic TLS buffers, six static
  Wi-Fi RX buffers, the custom partition table). One line
  differs from the firmware's copy: the partition table pin
  points at the example's own root, because ESP-IDF resolves
  the filename from the project root and the table sits at
  the example root.
