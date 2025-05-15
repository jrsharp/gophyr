# Gophyr - A Gopher Protocol Client for Zephyr RTOS

Gophyr is a modern implementation of the Gopher protocol (RFC 1436) for Zephyr RTOS, with support for Wi-Fi connectivity and ASCII art image rendering.

## Features

* Full Gopher protocol implementation (RFC 1436)
* Shell-based interface for easy navigation
* Image rendering with ASCII art conversion (WiP)
* Support for ESP32's SPIRAM for processing larger images
* Navigation history with 'back' command
* Color terminal output
* Directory browsing
* Text file viewing
* Image viewing (WiP)

## Building and Running

### Prerequisites

* Zephyr RTOS development environment
* Board with Wi-Fi support

### Building

To build the Gopher client for your board:

```
west build -b <board_name> .
```

Example for ESP32:
```
west build -b esp32s3_devkitm/esp32s3/procpu .
```

### Flashing

```
west flash
```

## Shell Commands

Once the application is running, the following commands are available:

```
gopher ip                - Display network information
gopher connect <host>    - Connect to a Gopher server
gopher get [selector]    - Request a document or directory
gopher view <index>      - View an item from the directory
gopher back              - Navigate back to previous item
gopher search <idx> <q>  - Search using a search server
gopher help              - Display help information
```

You can also use the shorter alias `g` instead of `gopher`:

```
g 1                      - View the first item in the directory
```

## Configuration

Configuration is done through the Kconfig system. Main configuration is in `prj.conf` with board-specific overrides in:
- `boards/*.conf`: Board-specific configurations
- `socs/*.conf`: SoC-specific configurations
- `nxp/overlay_*.conf`: NXP-specific overlay configurations

Some key configurations:
- `CONFIG_WIFI`: Enables Wi-Fi support
- `CONFIG_NET_L2_WIFI_SHELL`: Enables Wi-Fi shell module
- `CONFIG_NET_SHELL`: Enables networking shell commands
- `CONFIG_ESP_SPIRAM`: Enables SPIRAM support for ESP32

## SPIRAM Usage

For ESP32 targets, the client can utilize SPIRAM for image processing, allowing larger images to be processed. This is enabled by the following configuration options:

```
CONFIG_ESP_SPIRAM=y
CONFIG_ESP_SPIRAM_SIZE=8388608
CONFIG_ESP_SPIRAM_HEAP_SIZE=4096000
CONFIG_SHARED_MULTI_HEAP=y
```

When SPIRAM is available, the image processing memory limit is increased from 200KB to 3MB.

## License

Apache License 2.0
