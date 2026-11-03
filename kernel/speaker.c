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
 * on IBF from the timer ISR; we only start a command when IBF is already clear,
 * and otherwise queue the tone (one deep) and retry it on the next timer tick
 * (the same approach Hoppler's SndPump uses).
 *
 * GentleOS's engine calls krn_speaker_set_freq() on every note change and holds
 * the tone by leaving the frequency set. The 8741 instead needs the length up
 * front, so each song note is sent with its own duration; an open-ended tone
 * (a piano key) gets one short stroke. A rest or stop sends nothing - the
 * current tone simply ends.
 */

#include <kernel.h>

enum {
    KBD_DATA = 0x40,   /* 8741 data register   */
    KBD_CMD  = 0x41,   /* 8741 command/status  */
    KBD_IBF  = 0x02,   /* status bit 1: input buffer full */

    SND_CMD_TONE = 0x06,
};

/*
 * Length of an open-ended tone (ticks == 0xffff, e.g. a piano key struck on the
 * DMV, which has no key-up), in ~20 ms units (+0x1F bias applied below). Kept
 * short so it is a clean, finite stroke rather than a drone. Song notes use
 * their own duration instead.
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
 * Tone transmission - a small non-blocking state machine, the protocol
 * Hoppler's SndPump uses on real hardware.
 *
 * The 8741 TONE command is three bytes: 0x06 to the command port, then the
 * pitch and length bytes to the data port. Two facts make this delicate:
 *
 *  - While the 8741 plays a tone it does not read its input buffer at all, yet
 *    IBF (status bit 1) is already 0 again. A byte written then is latched and
 *    only consumed when the tone has ended. Writing several bytes in a row at
 *    that time overwrites the latch - the 8741 then sees a command without its
 *    data and waits for a data byte forever, keyboard included.
 *  - Once the 8741 has taken the 0x06 it sits in its TONE routine and actively
 *    waits for the two data bytes; it reads each one at once.
 *
 * So: when IBF is 0, write only 0x06 (state CMD_SENT). When IBF is 0 again,
 * the 8741 has taken the command and is waiting for data - write pitch and
 * length back to back. A command once started is always completed (as a pause
 * if the queued tone was cancelled meanwhile). krn_speaker_pump() never waits
 * longer than the few loops the 8741 needs between the two data bytes, so it
 * is safe in the timer ISR; it runs on every timer tick.
 */
enum {
    SND_IDLE = 0,
    SND_CMD_SENT = 1,
};

static int snd_state = SND_IDLE;

/* One-deep queue: the tone to send next (newest request wins). */
static int pending_tone;
static uint8_t pending_n;
static uint8_t pending_units;

static int
krn_speaker_ibf(void)
{
    return (krn_inb(KBD_CMD) & KBD_IBF) != 0;
}

static void
krn_speaker_pump(void)
{
    uint8_t pitch, len;
    int guard;

    if (snd_state == SND_IDLE) {
        if (!pending_tone || krn_speaker_ibf()) {
            return;
        }
        krn_outb(SND_CMD_TONE, KBD_CMD);
        snd_state = SND_CMD_SENT;
        return;
    }

    /* SND_CMD_SENT: wait until the 8741 has taken the command byte */
    if (krn_speaker_ibf()) {
        return;
    }

    if (pending_tone) {
        pitch = (uint8_t)(pending_n + 0x20);
        len = (uint8_t)(pending_units + 0x1F);
    } else {
        pitch = 0x20;           /* n = 0: pause */
        len = 1 + 0x1F;         /* shortest length */
    }
    pending_tone = 0;
    snd_state = SND_IDLE;

    krn_outb(pitch, KBD_DATA);
    /* the 8741 is actively waiting now and reads the byte within microseconds */
    guard = 0x4000;
    while (krn_speaker_ibf() && --guard)
        ;
    krn_outb(len, KBD_DATA);
}

/* Queue a tone and push it out as far as the 8741 allows right now. */
static void
krn_speaker_send(uint8_t n, uint8_t units)
{
    pending_tone = 1;
    pending_n = n;
    pending_units = units;

    krn_speaker_pump();
    krn_speaker_pump();     /* second step, if the command was taken at once */
}

/*
 * Start a tone of `hz` lasting `ticks` timer ticks (10 ms each on the DMV).
 * The 8741 plays tones of a fixed length by itself, so the length is sent up
 * front: a song note gets its own duration (a hair shorter, so the 8741 is
 * free again when the next note is due); an open-ended tone (ticks == 0xffff,
 * e.g. a piano key - the DMV keyboard has no key-up) gets one short stroke.
 * REST_PITCH is a rest (drops a queued tone), 0 means stop; neither sends
 * anything - the current tone simply ends.
 */
static void
krn_speaker_set_freq(uint16_t hz, uint16_t ticks)
{
    uint8_t n;
    uint16_t units;

    if (hz == REST_PITCH) {
        /* a rest inside a song: a late queued note must not sound into it */
        pending_tone = 0;
        return;
    }
    if (hz == 0) {
        /*
         * stop / end of song: leave a queued tone alone. On the DMV every key
         * press is followed at once by a synthesized key-up, and the piano
         * stops on key-up - clearing here would drop the tone just queued.
         */
        return;
    }

    n = krn_speaker_hz_to_tone(hz);
    if (n == 0) {
        return;
    }

    if (ticks == 0xffff) {
        units = DMV_TONE_UNITS;
    } else {
        /* 1 unit = 20.48 ms = ~2 ticks; length byte max 0xFF = 224 units */
        units = (ticks > 2) ? (uint16_t)((ticks - 1) / 2) : 1;
        if (units > 224) {
            units = 224;
        }
    }

    krn_speaker_send(n, (uint8_t)units);
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
        krn_speaker_set_freq(0, 0);
        return;
    }

    krn_speaker_state.note_ticks_left = note->ticks;

    krn_speaker_set_freq(note->pitch ? note->pitch : REST_PITCH, note->ticks);
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
        krn_speaker_set_freq(REST_PITCH, 0);
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
            ? krn_speaker_state.note->pitch : REST_PITCH,
            krn_speaker_state.note_ticks_left);
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
    krn_speaker_set_freq(0, 0);

    krn_unlock(lock);
}

/* Must be called in interrupt context (or, on a K230, from the timer poll) */
global void
krn_speaker_on_tick(void)
{
    /* advance a tone the busy 8741 could not take yet */
    krn_speaker_pump();

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
