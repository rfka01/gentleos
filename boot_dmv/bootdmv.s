; ---------------------------------------------------------------------------
; bootdmv.s - DMV-native boot sector for GentleOS/16 (no DOS underneath)
;
; VERIFIED entry contract (from DMV 8088 card ROM DMV_int_8088_PIC_33473.bin):
;   The ROM loads sector 0 to physical 0x2000 (segment 0x200, offset 0), then:
;       xor ax,ax ; mov ds,ax ; mov es,ax ; mov ss,ax ; mov sp,ax
;       jmp 0x200:0x0
;   So on entry: CS=0x200, IP=0, DS=ES=SS=0, SP=0.
;
; Therefore this sector is assembled with org 0 (IP starts at 0) and sets up
; its own DS=CS so that data labels (small offsets) address correctly.
;
; Disk I/O uses the i8272 FDC (ports 0x50/0x51) via the am9517 DMA controller,
; FDC on DMA channel 3 (addr 0x26, count 0x27, single-mask 0x2a, mode 0x2b).
; The DMA programs only a 16-bit address, so it can target ONLY the low 64 KB.
; We therefore DMA each run of sectors into a low scratch buffer and then copy
; it up (far rep movsw) to the real destination, which may be above 64 KB. This
; lets us load BOTH the kernel (0x1000:0x0100) and the 128 KB initrd
; (0x3000:0x0000) that the current GentleOS expects, matching the two-stage PC
; loader's memory map.
;
; Disk layout (512-byte sectors, CHS 40trk x 2head x 9sect):
;   sector 0            : this boot sector
;   sectors 1..K        : kernel .com  -> 0x1000:0x0100
;   sectors K+1..K+I    : initrd GT16.DAT -> 0x3000:0x0000
; The sector counts are patched in by mkdisks_dmv.pl after the "SECC"/"INIT"
; markers.
; ---------------------------------------------------------------------------

[cpu 8086]
[bits 16]
[org 0x0000]                        ; CS=0x200, IP starts at 0

SPT             equ 9
HEADS           equ 2

; Destination segments (match kernel/mem.c: main=0x1000 -> heap=0x2000,
; initrd=0x3000) and the upstream boot2 loader.
KERNEL_SEG      equ 0x1000
KERNEL_OFS      equ 0x0100
INITRD_SEG      equ 0x3000
INITRD_OFS      equ 0x0000

; Low scratch buffer for DMA (segment:offset, physical 0x4000). One track-run
; (max 9 sectors = 4608 bytes) always fits below the kernel/initrd.
SCRATCH_SEG     equ 0x0400
SCRATCH_OFS     equ 0x0000
SCRATCH_PHYS    equ SCRATCH_SEG*16 + SCRATCH_OFS   ; 0x4000

start:
    jmp short boot_main
    nop
    ; OS-ID field at offset 0x03, 7 bytes. The DMV mainboard ROM (routine at
    ; 0x0779) compares these 7 bytes against its OS-ID table and only boots the
    ; floppy on an exact match. "16BIT  " (5 chars + 2 spaces) is the 16-bit-OS
    ; identifier; without it the ROM rejects the disk and falls through to the
    ; hard disk. The 8th byte (offset 0x0a) is not checked.
    db "16BIT  "                    ; offset 0x03-0x09: REQUIRED boot signature
    db 0x00                         ; offset 0x0a: padding (unchecked)

; ---- runtime state -------------------------------------------------------
cur_lba         dw 0                ; current source LBA
sectors_left    dw 0                ; sectors still to read for this region
run_len         dw 0                ; sectors in the current track-run
dst_seg         dw 0                ; current copy-up destination segment
dst_ofs         dw 0                ; current copy-up destination offset

; Patched by mkdisks_dmv.pl. After "SECC" comes the kernel sector count; after
; "INIT" comes the initrd sector count.
                db 'SECC'
kernel_sectors  dw 95
                db 'INIT'
initrd_sectors  dw 256

; READ DATA command template (9 bytes). We use 0x46 (MFM, no MT): each READ
; command stays on a single head, which matches our per-head run_len logic.
;   +1 = (head<<2) | unit    +2 = track   +3 = head   +4 = sector
;   +5 = N (2 = 512 bytes)    +6 = EOT     +7 = GPL (0x1B)   +8 = DTL (0xFF)
fdc_cmd         db 0x46, 0x00, 0x00, 0x00, 0x00, 0x02, SPT, 0x1B, 0xFF
; SEEK command template (3 bytes): 0F, (head<<2)|unit, track
seek_cmd        db 0x0F, 0x00, 0x00
; SENSE INTERRUPT STATUS (1 byte)
sense_cmd       db 0x08

; ---------------------------------------------------------------------------
boot_main:
    cli
    ; ROM entered with CS=0x200, DS=ES=SS=0, SP=0. Establish our own world:
    ; make DS = CS so data labels resolve, and set a safe stack.
    mov ax, cs                      ; ax = 0x200
    mov ds, ax
    ; Stack high in our segment, above the 512-byte sector.
    mov ss, ax
    mov sp, 0x0FF0                  ; SS:SP = 0x200:0x0FF0 = phys 0x2FF0 (free)
    ; Interrupts stay disabled for the whole boot. We poll the FDC/DMA directly
    ; and never rely on IRQs; the IVT still holds the monitor's/garbage entries,
    ; so leaving IF clear avoids a stray interrupt vectoring into junk mid-read.

    ; Ensure the floppy motor is running and give it a brief settle.
    xor al, al
    out 0x14, al
    mov cx, 0x4000
.motor_wait:
    loop .motor_wait

    ; LED: light the bottom LED (label 8) to show we reached the load loop.
    mov al, 0xFE
    out 0x00, al

    ; ---- region 1: kernel -> KERNEL_SEG:KERNEL_OFS ----------------------
    mov word [cur_lba], 1
    mov ax, [kernel_sectors]
    mov [sectors_left], ax
    mov word [dst_seg], KERNEL_SEG
    mov word [dst_ofs], KERNEL_OFS
    call load_region
    jc .error

    ; LED: two lit (labels 8 and 7) after the kernel is in place.
    mov al, 0xFC
    out 0x00, al

    ; ---- region 2: initrd -> INITRD_SEG:INITRD_OFS ----------------------
    ; initrd source LBA = 1 + kernel_sectors (cur_lba already points there)
    mov ax, [initrd_sectors]
    mov [sectors_left], ax
    mov word [dst_seg], INITRD_SEG
    mov word [dst_ofs], INITRD_OFS
    call load_region
    jc .error

    ; LED: progress mask before handoff (labels 8,7,6 lit).
    mov al, 0xF8
    out 0x00, al

.handoff:
    cli
    mov ax, KERNEL_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFF
    ; Enter the kernel with interrupts DISABLED. The kernel fills the IVT with
    ; IRET traps and sets up the DMV 8259/8253 itself before enabling them
    ; (see kernel/main.c, kernel/timer.c). The boot never gates on the kernel
    ; magic number - the kernel self-verifies it in krn_check_load - so a
    ; successful read is enough to hand off, independent of the kernel's size.
    jmp KERNEL_SEG:KERNEL_OFS

.error:
    ; Read failure: light all 8 LEDs (0x00, active-low = all on) and halt.
    xor al, al
    out 0x00, al
    jmp $

; ---------------------------------------------------------------------------
; load_region: read [sectors_left] sectors starting at [cur_lba] and place them
; at [dst_seg]:[dst_ofs], advancing all three. Reads a track-run at a time into
; the low scratch buffer, then copies the run up to the destination. CF set on
; error.
load_region:
.loop:
    cmp word [sectors_left], 0
    jbe .done
    call read_track_run             ; -> scratch, sets run_len; CF on error
    jc .fail
    call copy_run_up                ; scratch -> dst_seg:dst_ofs
    ; advance destination by run_len*512 bytes (handle offset carry into seg)
    mov ax, [run_len]
    mov cl, 9
    shl ax, cl                      ; run_len * 512
    add [dst_ofs], ax
    jnc .no_carry
    add word [dst_seg], 0x1000      ; offset wrapped 64 KB -> +0x1000 paras
.no_carry:
    ; advance source LBA and remaining count
    mov ax, [run_len]
    add [cur_lba], ax
    sub [sectors_left], ax
    jmp .loop
.done:
    clc
    ret
.fail:
    stc
    ret

; ---------------------------------------------------------------------------
; copy_run_up: copy run_len*512 bytes from SCRATCH_SEG:SCRATCH_OFS to
; [dst_seg]:[dst_ofs] via far rep movsw. Preserves nothing of interest.
copy_run_up:
    push ax
    push cx
    push si
    push di
    push ds
    push es

    mov ax, SCRATCH_SEG
    mov ds, ax
    mov si, SCRATCH_OFS
    mov es, [cs:dst_seg]
    mov di, [cs:dst_ofs]

    mov ax, [cs:run_len]
    mov cl, 8
    shl ax, cl                      ; run_len * 256 words = run_len*512 bytes
    mov cx, ax
    cld
    rep movsw

    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

; ---------------------------------------------------------------------------
; read_track_run: read a run of consecutive sectors that lie on one track/head
; into the low scratch buffer, in a single i8272 READ DATA command. Reads from
; cur_lba up to the end of the current track (or sectors_left, whichever is
; smaller). Sets run_len. CF set on error.
read_track_run:
    push ax
    push bx
    push cx
    push dx

    ; LBA -> CHS
    mov ax, [cur_lba]
    xor dx, dx
    mov bx, SPT*HEADS
    div bx                          ; ax=track, dx=rem-within-cylinder
    mov ch, al                      ; track
    mov ax, dx
    xor dx, dx
    mov bx, SPT
    div bx                          ; ax=head, dx=sector-1
    mov dh, al                      ; head
    mov cl, dl
    inc cl                          ; sector (1-based)

    ; run length = min(sectors_left, SPT - (sector-1))
    mov al, SPT
    sub al, dl                      ; sectors from this sector to end of track
    xor ah, ah
    mov bx, [sectors_left]
    cmp ax, bx
    jbe .have_run
    mov ax, bx                      ; clamp to sectors_left
.have_run:
    mov [run_len], ax               ; sectors to read this pass (>=1)

    ; head<<2 | unit
    mov al, dh
    mov bl, al
    shl bl, 1
    shl bl, 1                       ; head<<2

    mov [fdc_cmd+1], bl             ; (head<<2)|unit
    mov [fdc_cmd+2], ch             ; track
    mov [fdc_cmd+3], dh             ; head
    mov [fdc_cmd+4], cl             ; start sector
    ; EOT = SPT+1 (one past the last sector): the FDC never matches
    ; current-sector == EOT within a real track, so the DMA terminal count
    ; alone bounds the transfer.
    mov byte [fdc_cmd+6], SPT+1

    mov [seek_cmd+1], bl            ; (head<<2)|unit
    mov [seek_cmd+2], ch            ; track

    ; --- SEEK + acknowledge (SENSE INTERRUPT STATUS) ----------------------
    mov cx, 3
    mov bx, seek_cmd
    call send_cmd
.wait_seek:
    in al, 0x13
    test al, 0x08                   ; bit3 = FDC interrupt (seek complete)
    jz .wait_seek
    mov cx, 1
    mov bx, sense_cmd
    call send_cmd
    call wait_fdc
    in al, 0x51
    call wait_fdc
    in al, 0x51

    ; --- program DMA channel 3 for run_len*512 bytes into scratch ---------
    mov al, 0x47                    ; mode: single, read (write-to-mem), ch3
    out 0x2b, al
    mov ax, SCRATCH_PHYS
    out 0x26, al                    ; addr low
    mov al, ah
    out 0x26, al                    ; addr high
    ; byte count = run_len*512 - 1
    mov ax, [run_len]
    mov cl, 9
    shl ax, cl                      ; run_len * 512
    dec ax                          ; count-1
    out 0x27, al                    ; count low
    mov al, ah
    out 0x27, al                    ; count high
    mov al, 0x03                    ; unmask ch3
    out 0x2a, al

    ; --- READ DATA --------------------------------------------------------
    mov cx, 9
    mov bx, fdc_cmd
    call send_cmd

.wait_done:
    in al, 0x13
    test al, 0x04                   ; FDD ready? (drop = error)
    jz .read_fail
    in al, 0x50
    test al, 0x80                   ; RQM set => result phase
    jz .wait_done

    ; mask ch3, drain 7 result bytes (RQM-gated before each)
    mov al, 0x07
    out 0x2a, al
    mov cx, 7
    mov bx, .result_buf
.res:
    call wait_fdc
    in al, 0x51
    mov [bx], al
    inc bx
    loop .res

    ; ST0 IC (top two bits): 00 = normal, 01 = terminated by TC (fine for a
    ; DMA read the terminal count stops). Only 1x is a hard failure.
    mov al, [.result_buf]
    and al, 0xC0
    cmp al, 0x80
    jae .read_fail

    clc
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.read_fail:
    stc
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.result_buf     times 7 db 0

; ---------------------------------------------------------------------------
send_cmd:
    call wait_fdc
    mov al, [bx]
    out 0x51, al
    inc bx
    loop send_cmd
    ret

wait_fdc:
    in al, 0x50
    test al, 0x80
    jz wait_fdc
    ret

times 512 - ($ - $$) db 0
