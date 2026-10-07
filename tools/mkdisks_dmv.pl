#
# Copyright (c) 2026 luke8086 (original), DMV port 2026
# Distributed under the terms of GPL-2 License.
#
# File: mkdisks_dmv.pl - DMV-native (no-DOS) disk image builder for GentleOS/16
#
# The DMV mainboard ROM autoloads only sector 0 and runs it. Our bootdmv sector
# then reads the kernel and the initrd straight off the floppy (via the i8272
# FDC + am9517 DMA, copying each run up out of the low 64 KB) and hands off to
# the kernel. So the native image is simply:
#
#   sector 0        : bootdmv sector (512 bytes)
#   sectors 1..K    : kernel GT16.COM (flags word patched)   -> 0x1000:0x0100
#   sectors K+1..   : initrd GT16.DAT                         -> 0x3000:0x0000
#
# The exact kernel and initrd sector counts are patched into the boot sector
# after its "SECC" and "INIT" markers so the loader reads precisely what is
# there and no further.
#
# This produces a native image ONLY; the DOS .com build and its images are made
# by the normal tools/mkdisks.pl. Run after building GT16.COM, GT16.DAT and
# build/boot_dmv/bootdmv.bin.
#

require "./tools/common.pl";

my $KRN_FLAG_COLORS_INVERTED = (1 << 1);

sub pad {
    my ($data, $size) = @_;
    return $data if $size == 0;
    my $n = length($data);
    die "Data exceeds padded size ($n > $size)\n" if $n > $size;
    return $data . "\0" x ($size - $n);
}

# Patch a little-endian word into $boot right after a 4-byte ascii marker.
sub patch_after_marker {
    my ($boot_ref, $marker, $value) = @_;
    my $at = index($$boot_ref, $marker);
    die "marker '$marker' not found in boot sector\n" if $at < 0;
    substr($$boot_ref, $at + 4, 2, pack("v", $value));
}

sub make_disk {
    my ($path, $size, $flags) = @_;

    my $boot = pad(slurp("build/boot_dmv/bootdmv.bin"), 512);

    my $kernel = slurp("GT16.COM");
    substr($kernel, 2, 2, pack("v", $flags));   # patch kernel flags word

    my $initrd = slurp("GT16.DAT");

    my $kernel_sectors = int((length($kernel) + 511) / 512);
    my $initrd_sectors = int((length($initrd) + 511) / 512);

    patch_after_marker(\$boot, "SECC", $kernel_sectors);
    patch_after_marker(\$boot, "INIT", $initrd_sectors);

    print "  kernel = $kernel_sectors sectors, initrd = $initrd_sectors sectors\n";

    # Pad each region to a whole sector so the next region is sector-aligned.
    $kernel = pad($kernel, $kernel_sectors * 512);
    $initrd = pad($initrd, $initrd_sectors * 512);

    my $image = pad($boot . $kernel . $initrd, $size);

    print "Creating $path... ";
    spit($path, $image);
    print "Done\n";
}

# 360K and 720K native images. 360K may be too small once the initrd is large;
# the builder dies with a clear "exceeds padded size" message if so, in which
# case use the 720K image.
make_disk("GT16DMV72.IMG", 720 * 1024, 0x00);
make_disk("GT16DMV.IMG",   0,          0x00);   # exact-size image, any capacity
