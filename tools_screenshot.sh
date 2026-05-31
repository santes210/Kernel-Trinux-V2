#!/bin/sh
# Headless boot + screenshot helper (requires qemu + python3-pil).
mkfifo /tmp/qmon 2>/dev/null
( sleep 4; echo "screendump /tmp/mykernel.ppm"; sleep 1; echo "quit" ) > /tmp/qmon &
qemu-system-i386 -kernel mykernel.bin -m 64M -display none -serial null -monitor stdio < /tmp/qmon
rm -f /tmp/qmon
echo "Screenshot saved to /tmp/mykernel.ppm"
