#!/bin/sh
echo "1" > /sys/bus/pci/devices/0000:61:00.0/reset

# echo "1" > /sys/bus/pci/devices/0000:42:00.0/power
# sleep 1
# sudo echo "1" > sudo /sys/bus/pci/rescan