# GentleOS/16

A hobby operating system for vintage 16-bit PCs,
built for tinkering with old hardware on the bare metal.

You can find more information on its [website](https://luke8086.dev/gentleos/16).

It's a simplified version of [GentleOS/32](https://github.com/luke8086/gentleos32),
targetting i386+ devices.

## NCR Decision Mate V port

This fork adds a port to the **NCR Decision Mate V** (DMV), whose 8088/V20
coprocessor card has no PC-compatible BIOS, display, timer or keyboard. The
same kernel binary runs two ways, chosen at runtime (`krn_is_dos()`):

- **Under the DMV's MS-DOS 2.11** — launched as a `.com`; time comes from DOS,
  keys from INT 16h.
- **Native, no DOS** — booted from a dedicated floppy (`boot_dmv/bootdmv.s`)
  that the mainboard ROM accepts (`16BIT` signature), which loads the kernel and
  initrd off the disk and hands off with no OS underneath.

What the port changes vs. upstream (all documented in
[`doc`](doc) and the project's hardware notes):

- **Display** — the DMV has an NEC µPD7220 GDC at 640×400, not CGA/VGA. The
  framebuffer flush (`gui/surface.c`) and mode set (`kernel/vga.c`) drive the
  GDC through ports 0xA0/0xA1 as three colour bit-planes. Mono vs colour
  machines are detected at runtime from the mainboard ROM version byte
  (`M.07.00` / `C.07.00`); on a mono display colour themes collapse to green.
- **Timer** — natively the DMV is not PC-compatible: the tick is 8253 channel 2
  (ports 0x82/0x83, 500 kHz clock) and the interrupt controller is an 8259 at
  ports 0x90/0x91 in AEOI mode. `kernel/timer.c` programs these directly when
  not under DOS; under DOS it uses the PC PIT path.
- **Interrupt vectors** — on a native boot the mainboard leaves the IVT full of
  garbage, so `kernel/main.c` fills all 256 vectors with an IRET trap before
  enabling interrupts (this is what cured the old instant-reboot loop).
- **Keyboard** — natively the 8741 MCU on ports 0x40/0x41, translated in
  `kernel/bios.c`; under DOS, INT 16h.
- **Clock** — the RTC is never written (matching upstream), so GentleOS keeps
  its own running time without clashing with the DOS date encoding.

### Building the DMV images

Build `GT16.COM` and `GT16.DAT` with the normal toolchain (below), then:

```sh
sh tools/mkdmv.sh        # -> GT16DMV.IMG (native, no-DOS 360K floppy)
```

The standard `wmake` build already produces the PC images, and `GT16.COM`
itself is the DOS-launched build for the DMV.

## Building

The only prerequisite is [DOSBox](https://www.dosbox.com/), the
entire toolchain (OpenWatcom, NASM, Perl) is included in the repo.

To build GentleOS, start DOSBox from the source directory and run
the following commands:

```dos
Z:\>MOUNT C .
Z:\>C:
C:\>ENV
C:\>AUTOGEN
C:\>WMAKE
```

## Development notes

- The official compiler is [OpenWatcom 1.9](https://www.openwatcom.org/),
  because it can be freely distributed, but
  [Turbo C 2.01](https://duckduckgo.com/?q=Turbo+C+2x)
  is also supported and it's much faster.
  You can install it to `C:\TMP\TC` and use with `WMAKE TC=1`

- For faster compilation, you can try setting `cycles=fixed 99999`
  in the `[cpu]` section of your DOSBox config file

- For a quick turnaround, GentleOS can be started as a COM file
  with `GT16.COM`. Pressing `Shift-Q` returns back to DOS.

## Attributions

- All images in [vendor/icons8](vendor/icons8) have been sourced from
  [Icons8](https://icons8.com/) using the
  [free license](https://web.archive.org/web/20260325111643/https://icons8.com/license)
  and modified

- All images in [vendor/mona](vendor/mona) have been extracted from the
  [Mona Font](https://github.com/MonadABXY/mona-font) and modified
  ([LICENSE](vendor/mona/LICENSE.txt))

- All fonts in [vendor/int10h](vendor/int10h) have been extracted from the
  [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/)
  and modified ([LICENSE](vendor/int10h/LICENSE.txt))

## Contributors

-  Alexander Rau ([l00nix](https://github.com/l00nix)) -
   Tested and made a release for [HP 200LX](https://github.com/l00nix/gentleos-hp200lx)

## License

Except where otherwise noted, GentleOS/16 is licensed under [GPLv2](LICENSE).
