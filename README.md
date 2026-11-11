# GentleOS/16

A fantasy operating system for vintage 16-bit PCs,
built for tinkering with old hardware on the bare metal.

You can find more information on its [website](https://luke8086.dev/gentleos/16).

<img src="https://luke8086.dev/gentleos/images/gentleos16-photo-t1100fd-nobg.webp" width="300">

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
  (ports 0x82/0x83, 500 kHz clock). With the K235 CPU card it interrupts through
  the 8259 at ports 0x90/0x91; the K230 has no interrupt controller, so
  `kernel/timer.c` detects that and polls the counter instead (and still reports
  `system_info.timer_available`). Under DOS it uses the PC PIT path.
- **Interrupt vectors** — on a native boot the mainboard leaves the IVT full of
  garbage, so `kernel/main.c` fills all 256 vectors with an IRET trap before
  enabling interrupts (this is what cured the old instant-reboot loop).
- **Keyboard** — natively the 8741 MCU on ports 0x40/0x41, translated in
  `kernel/bios.c`; under DOS, INT 16h. Neither reports key-up, so one is
  synthesised (`system_info.keyboard_native` stays 0). The keyboard's language
  code is read from the 8741, so the Sounds piano follows key positions.
- **Sound** — there is no PC speaker; tones go to the 8741 (TONE command 06h).
- **Clock** — the RTC is never written (matching upstream), so GentleOS keeps
  its own running time without clashing with the DOS date encoding.

### Building the DMV images

Build `GT16.COM` and `GT16.DAT` with the normal toolchain (below), then:

```sh
sh tools/mkdmv.sh        # -> GT16DMV.IMG (native, no-DOS 360K floppy)
```

The standard `wmake` build already produces the PC images, and `GT16.COM`
itself is the DOS-launched build for the DMV.

## Attributions

- Assets in [vendor/icons8](vendor/icons8) have been sourced from
  [Icons8](https://icons8.com/) using the
  [free license](https://web.archive.org/web/20260325111643/https://icons8.com/license)
  and modified

- Assets in [vendor/mona](vendor/mona) have been extracted from the
  [Mona Font](https://github.com/MonadABXY/mona-font) and modified
  ([LICENSE](vendor/mona/LICENSE.txt))

- Assets in [vendor/int10h](vendor/int10h) have been extracted from the
  [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/)
  and modified ([LICENSE](vendor/int10h/LICENSE.txt))

- Sokoban levels in [vendor/sokoban](vendor/sokoban) have been created by
  David W. Skinner and obtained from the
  [Sasquatch collection](https://web.archive.org/web/20040725085419/https://users.bentonrea.com/~sasquatch/sokoban/)
  ([README](vendor/sokoban/README.txt))

- Assets in [vendor/misc](vendor/misc) have been found online and modified
  ([README](vendor/misc/README.txt))

## Contributors

-  Alexander Rau ([l00nix](https://github.com/l00nix)) -
   Tested and made a release for [HP 200LX](https://github.com/l00nix/gentleos-hp200lx)

## License

Except where otherwise noted, GentleOS/16 is licensed under [GPLv2](LICENSE).
