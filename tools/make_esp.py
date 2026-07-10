#!/usr/bin/env python3
# make_esp.py - Build a FAT16 "EFI System Partition" image containing
# /EFI/BOOT/BOOTX64.EFI. This image is embedded into the ISO as the UEFI
# El Torito boot image, and can also be used directly as a virtual disk.
#
# We generate the filesystem by hand (no mtools/mkfs needed) so the whole
# project builds with only Python + NASM + MSVC + xorriso.
#
# usage: make_esp.py <BOOTX64.EFI> <out.img>
import sys, struct

SECTOR = 512

def main():
    if len(sys.argv) != 3:
        print("usage: make_esp.py <BOOTX64.EFI> <out.img>")
        return 1
    efi_path, out_path = sys.argv[1], sys.argv[2]
    with open(efi_path, "rb") as f:
        efi = f.read()

    # ---- FAT16 geometry ----------------------------------------------------
    bps          = 512      # bytes per sector
    spc          = 1        # sectors per cluster (512-byte clusters)
    reserved     = 4        # reserved sectors (boot sector + padding)
    num_fats     = 2
    root_entries = 512
    total_sectors = 40960   # 20 MiB - comfortably in the FAT16 cluster range

    root_dir_sectors = (root_entries * 32 + bps - 1) // bps          # 32
    tmp1 = total_sectors - (reserved + root_dir_sectors)
    tmp2 = 256 * spc + num_fats
    fat_sz = (tmp1 + tmp2 - 1) // tmp2                               # sectors/FAT
    fat_start  = reserved
    root_start = reserved + num_fats * fat_sz
    data_start = root_start + root_dir_sectors
    data_sectors = total_sectors - data_start
    count_clusters = data_sectors // spc
    assert 4085 <= count_clusters < 65525, "cluster count not FAT16: %d" % count_clusters

    img = bytearray(total_sectors * SECTOR)

    # ---- Boot sector / BIOS Parameter Block --------------------------------
    img[0:3]   = b"\xEB\x3C\x90"          # jmp short + nop
    img[3:11]  = b"MINISVM "              # OEM name (8 bytes)
    struct.pack_into("<H", img, 11, bps)
    img[13]    = spc
    struct.pack_into("<H", img, 14, reserved)
    img[16]    = num_fats
    struct.pack_into("<H", img, 17, root_entries)
    struct.pack_into("<H", img, 19, total_sectors)   # fits in 16 bits here
    img[21]    = 0xF8                     # media descriptor (fixed disk)
    struct.pack_into("<H", img, 22, fat_sz)          # FATSz16
    struct.pack_into("<H", img, 24, 32)              # sectors per track
    struct.pack_into("<H", img, 26, 2)               # heads
    struct.pack_into("<I", img, 28, 0)               # hidden sectors
    struct.pack_into("<I", img, 32, 0)               # large total sectors (unused)
    img[36]    = 0x80                     # drive number
    img[38]    = 0x29                     # extended boot signature
    struct.pack_into("<I", img, 39, 0x1DE5A17)       # volume id
    img[43:54] = b"MINISVM ESP"           # volume label (11 bytes)
    img[54:62] = b"FAT16   "              # filesystem type (8 bytes)
    struct.pack_into("<H", img, 510, 0xAA55)         # boot signature

    # ---- Cluster allocation ------------------------------------------------
    # cluster 2 = /EFI directory, 3 = /EFI/BOOT directory, 4.. = the EFI file.
    CL_EFI, CL_BOOT, CL_FILE = 2, 3, 4
    clus_bytes = spc * bps
    file_clusters = max(1, (len(efi) + clus_bytes - 1) // clus_bytes)

    fat_entries = fat_sz * SECTOR // 2
    fat = [0] * fat_entries
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    fat[CL_EFI]  = 0xFFFF                  # directory: single cluster, end-of-chain
    fat[CL_BOOT] = 0xFFFF
    for i in range(file_clusters):        # chain the file's clusters
        c = CL_FILE + i
        fat[c] = 0xFFFF if i == file_clusters - 1 else (c + 1)

    # Write both FAT copies.
    for n in range(num_fats):
        base = (fat_start + n * fat_sz) * SECTOR
        for i, v in enumerate(fat):
            struct.pack_into("<H", img, base + i * 2, v & 0xFFFF)

    # ---- Directory entries -------------------------------------------------
    def dirent(name11, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return e

    def name83(name, ext=""):
        return (name.upper().ljust(8)[:8] + ext.upper().ljust(3)[:3]).encode("ascii")

    def clus_off(cluster):
        return (data_start + (cluster - 2) * spc) * SECTOR

    ATTR_DIR, ATTR_ARC = 0x10, 0x20

    # Root directory: one entry, the EFI subdirectory.
    img[root_start * SECTOR : root_start * SECTOR + 32] = \
        dirent(name83("EFI"), ATTR_DIR, CL_EFI, 0)

    # /EFI directory: ".", "..", "BOOT".
    off = clus_off(CL_EFI)
    img[off + 0:off + 32]  = dirent(b".          ", ATTR_DIR, CL_EFI, 0)
    img[off + 32:off + 64] = dirent(b"..         ", ATTR_DIR, 0, 0)   # .. of root = 0
    img[off + 64:off + 96] = dirent(name83("BOOT"), ATTR_DIR, CL_BOOT, 0)

    # /EFI/BOOT directory: ".", "..", "BOOTX64.EFI".
    off = clus_off(CL_BOOT)
    img[off + 0:off + 32]  = dirent(b".          ", ATTR_DIR, CL_BOOT, 0)
    img[off + 32:off + 64] = dirent(b"..         ", ATTR_DIR, CL_EFI, 0)
    img[off + 64:off + 96] = dirent(name83("BOOTX64", "EFI"), ATTR_ARC, CL_FILE, len(efi))

    # ---- File data ---------------------------------------------------------
    off = clus_off(CL_FILE)
    img[off:off + len(efi)] = efi

    with open(out_path, "wb") as f:
        f.write(img)
    print("wrote %s (%d bytes, FAT16, %d file clusters)" %
          (out_path, len(img), file_clusters))
    return 0

if __name__ == "__main__":
    sys.exit(main())
