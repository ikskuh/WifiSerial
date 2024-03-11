#!/bin/sh

set -e

which nc stty > /dev/null

if [ -z "${WIRELESS_UART_HOST}" ]; then 
    echo "Please set WIRELESS_UART_HOST to your target ip or host name." >&2
    exit 1
fi

baud="115200"
mode="8n1"

case $# in
    0)
        ;;
    1)
        baud="$1"
        ;;
    2)
        baud="$1"
        mode="$2"
        ;;
    *)
        echo "$0 [baudrate] [mode]" >&2
        exit 1
        ;;
esac

if [ "${baud}" = "-h" ] || [ "${baud}" = "--help" ]; then
    echo "$0 [baudrate] [mode]" >&2
    exit 0
fi

echo "Connecting to ${WIRELESS_UART_HOST} with ${baud} baud, mode=${mode}" >&2

{
    echo "baud=${baud}"
    echo "mode=${mode}"
} | nc "${WIRELESS_UART_HOST}" 1337 -w1

stty raw icrnl isig

nc -v "${WIRELESS_UART_HOST}" 23

stty sane

echo "TTY is back to sane state" >&2

