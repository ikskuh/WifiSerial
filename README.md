# WifiSerial

Unholy combination of Arduino and ESP-IDF to create a primitive network-attached UART.

Features:

* Online reconfiguration
* Standard Port 23 for pure serial communications
* Configuration Port for changing parameters

## Hardware + Pinout

I use a NodeMCU 32S, but it doesn't really matter, which ESP32 board you use.
You can use any ESP32 based hardware with the following pinout:

| ESP GPIO | Function       |
| -------- | -------------- |
| 1        | Debug/Prog TXD |
| 3        | Debug/Prog RXD |
| 16       | UART RXD       |
| 17       | UART TXD       |

## Data Port

* TCP Port 23
* Only a single connection at a time.
* Everything sent is transmitted via the UART.
* Everything received via the UART is sent back to the host.

## Configuration Port

* TCP Port 1337
* Only a single connection at a time.
* Line based protocol, accepts both CR and LF as command separator.
* Syntax is `key=value`
* Query the value by passing `key=?`
* Legal keys are:
  * `baud`, takes an integer, current baud rate
  * `mode`, takes a mode descriptor in the form of `8n1` (databits, parity, stop bits)
    * databits from `5` to `8`
    * parity is `e` (even), `o` (odd), `n` (none)
    * stop bits is `1` or `2`

## Build

Get PlatformIO and do:

```sh-session
[user@host] ~/wireless-uart$ pio run 
…
[user@host] ~/wireless-uart$ 
```

