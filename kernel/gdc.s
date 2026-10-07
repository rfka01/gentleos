; ---------------------------------------------------------------------------------------
; Copyright (c) 2026 luke8086 (original), DMV port 2026
; Distributed under the terms of GPL-2 License
; ---------------------------------------------------------------------------------------
; File: gdc.s - Fast uPD7220 data streaming for the NCR Decision Mate V
;
; The GDC flush is CPU-bound: measured in MAME, the GDC FIFO is never full, the
; time goes into the per-byte C path (a function call and a port helper per
; byte). This is the inner loop in assembler, the way Hoppler's BlitAsm feeds
; the GDC: each source byte is bit-reversed with XLAT (backbuffer pixels are
; MSB-first, a GDC display word is LSB-first), optionally inverted, and
; written to the parameter port.
;
; FIFO handling: before every block of 16 bytes wait for "FIFO empty" (status
; bit 2). An empty 16-byte FIFO then takes the whole block without further
; checks. ("FIFO not full" would only guarantee one free slot.)
; ---------------------------------------------------------------------------------------

[cpu 8086]

GDC_STAT        equ 0xA0        ; read: status
GDC_PARAM       equ 0xA0        ; write: parameter / data
GDC_ST_EMPTY    equ 0x04        ; status bit 2: FIFO empty

section _TEXT class=CODE

; void krn_gdc_stream(const uint8_t far *src, uint16_t count,
;                     uint16_t xor_mask, const uint8_t *bitrev)
;
;   src       far pointer to the first source byte
;   count     number of bytes to send (an even number for whole words)
;   xor_mask  0x00 = as is, 0xFF = inverted (low byte used)
;   bitrev    near pointer (DGROUP) to the 256-byte bit-reversal table
;
; cdecl, small model: [bp+4] src offset, [bp+6] src segment, [bp+8] count,
; [bp+10] xor_mask, [bp+12] bitrev.
global _krn_gdc_stream
_krn_gdc_stream:
    push bp
    mov bp, sp
    push bx
    push si
    push di
    push es

    les si, [bp+4]              ; ES:SI = source
    mov cx, [bp+8]              ; byte count
    mov ah, [bp+10]             ; xor mask
    mov bx, [bp+12]             ; DS:BX = bit-reversal table
    mov dx, GDC_STAT
    jcxz .done

.block:
    ; wait until the FIFO is empty - then 16 bytes fit without checking
.wait:
    in al, dx
    test al, GDC_ST_EMPTY
    jz .wait

    mov di, 16
.byte:
    mov al, [es:si]
    inc si
    xlat                        ; al = bitrev[al]
    xor al, ah
    out dx, al                  ; DX = 0xA0 = parameter port
    dec cx
    jz .done
    dec di
    jnz .byte
    jmp .block

.done:
    pop es
    pop di
    pop si
    pop bx
    pop bp
    ret
