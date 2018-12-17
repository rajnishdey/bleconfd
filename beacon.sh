#!/bin/sh
hciconfig hci0 down
hciconfig hci0 up
hciconfig hci0 class 3a0430
hciconfig hci0 noleadv
hciconfig hci0 name XPI-SETUP
#hcitool cmd 0x08 0x0008 1f 02 01 06 03 03 aa fe 17 16 aa fe 00 e7 43 f2 ac d1 4a 82 8e a1 2c 30 11 11 11 11 11 11 00 00

# namespace (first 10 bytes are generated by taking hash of URI string
# echo "xfinity.com" | sha1sum | cut -b -20
# 36c8807bf460cb41d145                                           ####
# next six bytes are just all zeros with a 1 at end
hcitool cmd 0x08 0x0008 1f 02 01 06 03 03 aa fe 17 16 aa fe 00 e7 36 c8 80 7b f4 60 cb 41 d1 45 00 00 00 00 00 01 00 00
# not sure what this is needed for
#hcitool cmd 0x08 0x0006 A0 00 A0 00 00 00 00 00 00 00 00 00 00 07 00
hciconfig hci0 leadv

