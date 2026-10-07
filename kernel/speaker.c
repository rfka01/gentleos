/*
 * Copyright (c) 2026 luke8086 (original), DMV port 2026
 * Distributed under the terms of GPL-2 License
 *
 * File: speaker.c - Sound driver for the NCR Decision Mate V (8741 keyboard MCU)
 *
 * The DMV has no PC speaker (no PIT channel 2, no 8255 port B gate). Instead the
 * mainboard keyboard controller (an 8741 on ports 0x40/0x41) owns the speaker
 * and plays a single tone of a fixed length on command (hw: NCR keyboard
 * interface listing, SOUND-GENERATOR / TONE at 0x246):
 *
 *   OUT 0x41, 0x06        ; TONE command
 *   OUT 0x40, tone_byte   ; pitch:  n + 0x20, n = 1..42 (A 110 Hz .. D 1175 Hz)
 *   OUT 0x40, len_byte    ; length: units + 0x1F, 1 unit ~= 20.48 ms
 *                         ;   length 0 -> 256 units (~5 s): avoid
 *
 * Before each write the Input Buffer Full status (port 0x41 bit 1) must be 0.
 * While a tone is playing the 8741 does not poll, so a command written then
 * stays latched in IBF until the tone finishes. We must therefore NEVER block
 * on IBF from the timer ISR; we only write when IBF is already clear and skip
 * otherwise (the same approach Hoppler's SndPump uses).
 *
 * GentleOS's engine calls krn_speaker_set_freq() on every note change and holds
 * the tone by leaving the frequency set. We map that onto the 8741 by issuing a
 * TONE command whose length comfortably outlasts one note step; the engine
 * re-issues it on the next note. A rest/stop issues nothing (the previous tone
 * simply decays).
 */

#include <kernel.h>

enum {
    KBD_DATA = 0x40,   /* 8741 data register   */
    KBD_CMD  = 0x41,   /* 8741 command/status  */
    KBD_IBF  = 0x02,   /* status bit 1: input buffer full */

    SND_CMD_TONE = 0x06,
};

/*
 * Tone length written to the 8741, in ~20 ms units (+0x1F bias applied below).
 * The default timer tick is 10 ms, so a few units outlast one or two note
 * steps; the engine refreshes it on each note. Kept short so a held "note"
 * (e.g. a piano key struck on the DMV, which has no key-up) is a clean, finite
 * stroke rather than a drone.
 */
#define DMV_TONE_UNITS 12   /* ~240 ms */

/*
 * The engine uses this sentinel pitch for a "rest" note (instead of 0) so the
 * PC speaker wouldn't glitch. On the DMV a rest simply writes no tone.
 */
#define REST_PITCH 59659U

/* Not used on the DMV (no 8255 port B), kept for the keyboard ISR's reference. */
global uint8_t krn_speaker_ppi_bits;

static volatile speaker_state_st krn_speaker_state;

/*
 * Convert a frequency in Hz to the 8741 tone index n, rounded to the nearest
 * chromatic semitone: f = 110 * 2^((n-1)/12), so n = 1 + 12*log2(f/110). We
 * avoid floating point (no FPU assumptions) by walking the 42 tone frequencies
 * and picking the closest. Returns 0 for "no playable tone" (silence/too low).
 */
static uint8_t
krn_speaker_hz_to_tone(uint16_t hz)
{
    /* f(n) = round(110 * 2^((n-1)/12)) for n = 1..42, precomputed. */
    static const uint16_t tone_hz[42] = {
        110, 117, 123, 131, 139, 147, 156, 165, 175, 185, 196, 208,
        220, 233, 247, 262, 277, 294, 311, 330, 349, 370, 392, 415,
        440, 466, 494, 523, 554, 587, 622, 659, 698, 740, 784, 831,
        880, 932, 988, 1047, 1109, 1175
    };
    int best = 0;
    uint16_t best_err = 0xFFFF;
    int i;

    if (hz == 0) {
        return 0;
    }

    for (i = 0; i < 42; ++i) {
        uint16_t err = (tone_hz[i] > hz) ? (tone_hz[i] - hz) : (hz - tone_hz[i]);
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }

    return (uint8_t)(best + 1);   /* n is 1-based */
}

/*
 * Non-blocking 8741 tone write. Returns without doing anything if the input
 * buffer is still full (a previous tone/command is in flight) - safe to call
 * from the timer ISR. hz == 0 is a rest: nothing is written (the current tone
 * finishes on its own), which is what the engine wants between notes.
 */
static void
krn_speaker_set_freq(uint16_t hz)
{
    uint8_t n;

    if (hz == 0 || hz == REST_PITCH) {
        return;
    }

    n = krn_speaker_hz_to_tone(hz);
    if (n == 0) {
        return;
    }

    /* Only issue the command if the 8741 can accept it right now. */
    if (krn_inb(KBD_CMD) & KBD_IBF) {
        return;
    }
    krn_outb(SND_CMD_TONE, KBD_CMD);

    if (krn_inb(KBD_CMD) & KBD_IBF) {
        return;
    }
    krn_outb((uint8_t)(n + 0x20), KBD_DATA);           /* pitch byte  */

    if (krn_inb(KBD_CMD) & KBD_IBF) {
        return;
    }
    krn_outb((uint8_t)(DMV_TONE_UNITS + 0x1F), KBD_DATA); /* length byte */
}

/* Must be called while locked or in interrupt context */
static void
krn_speaker_start_note(void)
{
    const note_st far *note = krn_speaker_state.note;

    if (note == NULL || note->ticks == 0) {
        /* Keep song, owner and elapsed time for inspection */
        krn_speaker_state.state = SPEAKER_STATE_STOPPED;
        krn_speaker_state.note = NULL;
        krn_speaker_set_freq(0);
        return;
    }

    krn_speaker_state.note_ticks_left = note->ticks;

    krn_speaker_set_freq(note->pitch ? note->pitch : REST_PITCH);
}

global void
krn_speaker_get_state(speaker_state_st *out)
{
    krn_lock_t lock;

    lock = krn_lock();

    memcpy(out, (const void *)&krn_speaker_state, sizeof(*out));

    krn_unlock(lock);
}

global void
krn_speaker_play_song(const note_st far *notes, void *owner)
{
    krn_lock_t lock;

    lock = krn_lock();

    krn_speaker_state.state = SPEAKER_STATE_PLAYING;
    krn_speaker_state.song = notes;
    krn_speaker_state.song_owner = owner;
    krn_speaker_state.song_elapsed_ticks = 0;
    krn_speaker_state.note = krn_speaker_state.song;

    krn_speaker_start_note();

    krn_unlock(lock);
}

global void
krn_speaker_play_freq(uint16_t hz, void *owner)
{
    static note_st notes[2];
    krn_lock_t lock;

    lock = krn_lock();

    notes[0].pitch = hz;
    notes[0].ticks = 0xffff;
    notes[1].pitch = 0;
    notes[1].ticks = 0;

    krn_speaker_play_song(notes, owner);

    krn_unlock(lock);
}

global void
krn_speaker_pause(void *owner)
{
    krn_lock_t lock;

    lock = krn_lock();

    if (krn_speaker_state.song_owner != owner) {
        krn_unlock(lock);
        return;
    }

    if (krn_speaker_state.state == SPEAKER_STATE_PLAYING) {
        krn_speaker_state.state = SPEAKER_STATE_PAUSED;
        krn_speaker_set_freq(REST_PITCH);
    }

    krn_unlock(lock);
}

global void
krn_speaker_resume(void *owner)
{
    krn_lock_t lock;

    lock = krn_lock();

    if (krn_speaker_state.song_owner != owner) {
        krn_unlock(lock);
        return;
    }

    if (krn_speaker_state.state == SPEAKER_STATE_PAUSED) {
        krn_speaker_state.state = SPEAKER_STATE_PLAYING;

        krn_speaker_set_freq(krn_speaker_state.note->pitch
            ? krn_speaker_state.note->pitch : REST_PITCH);
    }

    krn_unlock(lock);
}

global void
krn_speaker_stop(void *owner)
{
    krn_lock_t lock;

    lock = krn_lock();

    if (krn_speaker_state.song_owner != owner) {
        krn_unlock(lock);
        return;
    }

    krn_speaker_state.state = SPEAKER_STATE_STOPPED;
    krn_speaker_state.song = NULL;
    krn_speaker_state.song_owner = NULL;
    krn_speaker_state.song_elapsed_ticks = 0;
    krn_speaker_state.note = NULL;
    krn_speaker_state.note_ticks_left = 0;
    krn_speaker_set_freq(0);

    krn_unlock(lock);
}

/* Must be called in interrupt context */
global void
krn_speaker_on_tick(void)
{
    if (krn_speaker_state.state != SPEAKER_STATE_PLAYING) {
        return;
    }

    ++krn_speaker_state.song_elapsed_ticks;
    --krn_speaker_state.note_ticks_left;

    if (krn_speaker_state.note_ticks_left > 0) {
        return;
    }

    ++krn_speaker_state.note;
    krn_speaker_start_note();
}

global void
krn_speaker_deinit(void)
{
    krn_lock_t lock = krn_lock();

    krn_speaker_stop(krn_speaker_state.song_owner);

    krn_unlock(lock);
}
