; ---------------------------------------------------------------------------------------
; Copyright (c) 2019-2026 luke8086
; Distributed under the terms of GPL-2 License
; ---------------------------------------------------------------------------------------
; File: isr.s - Interrupt service routines
; ---------------------------------------------------------------------------------------

[cpu 8086]

extern _krn_timer_handle_intr
extern _krn_keyboard_handle_intr

section _TEXT class=CODE

; Bare IRET used to fill unused interrupt vectors on the native DMV boot, where
; the mainboard leaves the IVT full of garbage (hw doc §1). Every vector points
; here until a real handler (e.g. the timer on INT 08h) overwrites it, so a
; stray INT returns harmlessly instead of running through uninitialised memory.
global _krn_isr_trap
_krn_isr_trap:
    iret

global _krn_isr_timer
_krn_isr_timer:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    push cs
    pop ds

    call _krn_timer_handle_intr

    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

global _krn_isr_keyboard
_krn_isr_keyboard:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    push cs
    pop ds

    call _krn_keyboard_handle_intr

    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret
