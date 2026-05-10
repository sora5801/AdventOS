Hello from a USB Mass Storage device!

QEMU exposes this file via:
    -drive id=usbfs,file=usbfs.img,format=raw,if=none
    -device usb-storage,drive=usbfs

AdventOS's UHCI driver enumerates the device, the Bulk-Only
Transport layer wraps SCSI commands, and the new sys_block_*
syscalls expose block-level access to user space.
