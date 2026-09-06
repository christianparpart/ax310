#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -e

SOFTWARE_DIR="$HOME/Downloads/AX310_Software"
SOFTWARE_ISO="$HOME/Downloads/AX310_Software.iso"
WIN11_ISO="$HOME/Downloads/iso/Win11_English_x64v1.iso"
VIRTIO_ISO="$HOME/Downloads/iso/virtio-win-0.1.215.iso"
XML_SCRIPT="$(dirname $0)/autounattend.xml"

if [ ! -f "$WIN11_ISO" ]; then
    echo "Error: Windows 11 ISO not found at $WIN11_ISO"
    exit 1
fi

echo "Creating ISO from $SOFTWARE_DIR..."
mkisofs -o "$SOFTWARE_ISO" -J -R "$SOFTWARE_DIR"

echo "Creating virtual floppy for unattend..."
dd if=/dev/zero of=/tmp/unattend.img bs=1k count=1440
mkfs.vfat /tmp/unattend.img
mcopy -i /tmp/unattend.img "$XML_SCRIPT" ::/autounattend.xml

# Ensure old VM is undefind if it exists
virsh destroy AX310-Capture-VM 2>/dev/null || true
virsh undefine AX310-Capture-VM --remove-all-storage 2>/dev/null || true

echo "Starting Windows 11 installation..."
virt-install \
  --name AX310-Capture-VM \
  --os-variant win11 \
  --memory 8192 \
  --vcpus 4 \
  --disk size=64,bus=sata \
  --disk path=/tmp/unattend.img,device=floppy \
  --cdrom "$WIN11_ISO" \
  --disk path="$VIRTIO_ISO",device=cdrom \
  --disk path="$SOFTWARE_ISO",device=cdrom \
  --network default,model=virtio \
  --graphics spice \
  --tpm backend.type=emulator,backend.version=2.0,model=tpm-crb \
  --boot uefi \
  --hostdev 07ca:1310 \
  --hostdev 07ca:0310 \
  --noautoconsole

echo "VM creation started. Use 'virt-viewer AX310-Capture-VM' to connect to the console."
