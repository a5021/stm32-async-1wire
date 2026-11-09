/**
 * @file ds2413.c
 * @brief Non-blocking DS2413 dual-channel addressable switch driver
 *
 * One operation is reset -> presence -> command write -> one-byte read ->
 * done. The command table is built only after the reset capture has been
 * consumed, so it can share storage with it, exactly like the DS2431 driver.
 */

#include "ds2413.h"
#include "onewire.h"
#include "ow_glue.h"

/* Longest command write: Match ROM prefix (1 + 8) + command + state +
 * complement = 12 bytes. The read command is shorter. */
#define DS2413_CMD_MAX_BYTES ((1u + DS2413_ROM_BYTES) + 3u)
#define DS2413_CMD_SLOTS (DS2413_CMD_MAX_BYTES * ONEWIRE_BITS_PER_BYTE)
#define DS2413_READ_BYTES 1u
#define DS2413_READ_SLOTS (DS2413_READ_BYTES * ONEWIRE_BITS_PER_BYTE)

/** @brief Operation phases */
typedef enum {
    DS2413_PH_PRIME = 0,
    DS2413_PH_RESET,
    DS2413_PH_CMD,
    DS2413_PH_READ,
    DS2413_PH_DONE,
    DS2413_PH_IDLE
} ds2413_phase_t;

/** @brief Driver context */
typedef struct {
    uint8_t* out; /**< Read destination (valid until the operation finishes) */
    union {
        ow_pulse_t cmd[DS2413_CMD_SLOTS + 1u]; /**< command write table */
        volatile uint16_t cap[2]; /**< reset + presence capture */
        volatile uint8_t pulse[DS2413_READ_SLOTS]; /**< reply capture */
    } u;
    uint8_t raw[DS2413_READ_BYTES]; /**< decoded reply */
    uint8_t phase;
    uint8_t flags;
    uint8_t command;
    uint8_t data; /**< PIO state byte (already masked with 0xFC) */
    uint8_t last_pio;
    uint8_t attempts;
    uint8_t selected_rom[DS2413_ROM_BYTES];
} ds2413_ctx_t;

/** @brief Match ROM addressing (0 = Skip ROM broadcast) */
#define DS2413_FLAG_ADDRESS (1u << 0)
/** @brief The last operation completed successfully */
#define DS2413_FLAG_OK (1u << 1)
/** @brief The operation finished (or nothing was scheduled) */
#define DS2413_FLAG_FINISHED (1u << 2)

/** @brief Bus retries, matching the Linux w1_ds2413 driver */
#define DS2413_MAX_ATTEMPTS 3u
/** @brief Parasite priming high time before an operation */
#define DS2413_PRIME_US 1000u

static ds2413_ctx_t ctx;

_Static_assert(DS2413_PH_IDLE <= 255, "phase enum must fit uint8_t packing");
_Static_assert(DS2413_CMD_SLOTS <= ONEWIRE_MAX_SLOTS,
               "DS2413 command write must fit one RCR window");
_Static_assert(DS2413_READ_SLOTS <= ONEWIRE_MAX_SLOTS,
               "DS2413 reply must fit one RCR window");
_Static_assert(DS2413_READ_BYTES <= ONEWIRE_MAX_READ_BYTES,
               "DS2413 reply must fit one read window");

static uint8_t ds2413_can_start(void) {
    return (uint8_t)((ctx.phase == DS2413_PH_IDLE ||
                      ctx.phase == DS2413_PH_DONE) &&
                     !onewire_search_active());
}

/** @brief Build the command table and return its slot count */
static uint8_t ds2413_build(void) {
    ow_pulse_t* p = (ow_pulse_t*)ctx.u.cmd;
    uint8_t bytes;
    if ((ctx.flags & DS2413_FLAG_ADDRESS) != 0u) {
        bytes = ow_encode_match_rom(&p, ctx.selected_rom);
    } else {
        bytes = ow_encode_skip_rom(&p);
    }
    onewire_encode_byte(p, ctx.command);
    p += ONEWIRE_BITS_PER_BYTE;
    bytes++;
    if (ctx.command == DS2413_CMD_WRITE) {
        onewire_encode_byte(p, ctx.data);
        p += ONEWIRE_BITS_PER_BYTE;
        bytes++;
        onewire_encode_byte(p, (uint8_t)~ctx.data);
        p += ONEWIRE_BITS_PER_BYTE;
        bytes++;
    }
    const uint8_t slots = (uint8_t)(bytes * ONEWIRE_BITS_PER_BYTE);
    ((ow_pulse_t*)ctx.u.cmd)[slots] = ONEWIRE_RELEASE_PULSE;
    return slots;
}

/** @brief Schedule one operation; 1 = accepted */
static uint8_t ds2413_start(uint8_t command, uint8_t data, uint8_t* out) {
    if (!ds2413_can_start()) {
        return 0;
    }
    ctx.command = command;
    ctx.data = data;
    ctx.out = out;
    ctx.flags &= (uint8_t)~(DS2413_FLAG_OK | DS2413_FLAG_FINISHED);
    ctx.attempts = 0;
    onewire_strong_pullup(1);
    ow_start_wait(DS2413_PRIME_US);
    ctx.phase = DS2413_PH_PRIME;
    return 1;
}

/** @brief Validate a PIO Access Read reply */
static uint8_t ds2413_reply_valid(uint8_t b) {
    if (b == DS2413_NO_RESPONSE) {
        return 0;
    }
    return (uint8_t)((b & 0x0Fu) == ((uint8_t)(~b) >> 4));
}

void ds2413_init(void) {
    onewire_init();
    ctx.flags = DS2413_FLAG_FINISHED;
    ctx.phase = DS2413_PH_IDLE;
    ctx.command = 0;
    ctx.data = 0;
    ctx.last_pio = 0;
}

void ds2413_select(const uint8_t* rom) {
    if (ctx.phase != DS2413_PH_IDLE && ctx.phase != DS2413_PH_DONE) {
        return;
    }
    if (onewire_search_active()) {
        return;
    }
    if (rom == 0) {
        ctx.flags &= (uint8_t)~DS2413_FLAG_ADDRESS;
        return;
    }
    for (uint8_t i = 0; i < DS2413_ROM_BYTES; i++) {
        ctx.selected_rom[i] = rom[i];
    }
    ctx.flags |= DS2413_FLAG_ADDRESS;
}

uint8_t ds2413_read_pio(uint8_t* out) {
    if (out == 0) {
        return 0;
    }
    return ds2413_start(DS2413_CMD_READ, 0, out);
}

uint8_t ds2413_write_pio(uint8_t states) {
    /* Bits 2-7 must be 1 in the state byte sent to the device. */
    return ds2413_start(DS2413_CMD_WRITE,
                        (uint8_t)((states & DS2413_PIO_MASK) | 0xFCu), 0);
}

uint8_t ds2413_last_command_ok(void) {
    return (uint8_t)((ctx.flags & DS2413_FLAG_OK) != 0u);
}

uint8_t ds2413_last_pio(void) {
    return ctx.last_pio;
}

uint8_t ds2413_last_reply(void) {
    return ctx.raw[0];
}

static uint8_t ds2413_poll(void) {
    if ((ctx.flags & DS2413_FLAG_FINISHED) != 0u) {
        return 1;
    }
    if (ctx.phase == DS2413_PH_DONE) {
        ctx.flags |= DS2413_FLAG_FINISHED;
        return 1;
    }
    if (!ow_op_done()) {
        return 0;
    }

    switch (ctx.phase) {
    case DS2413_PH_PRIME:
        onewire_strong_pullup(0);
        onewire_reset((volatile uint16_t*)ctx.u.cap);
        ctx.phase = DS2413_PH_RESET;
        break;

    case DS2413_PH_RESET:
        if (!ow_has_presence(ctx.u.cap)) {
            if (++ctx.attempts < DS2413_MAX_ATTEMPTS) {
                onewire_strong_pullup(1);
                ow_start_wait(DS2413_PRIME_US);
                ctx.phase = DS2413_PH_PRIME;
                break;
            }
            ctx.phase = DS2413_PH_DONE;
            break;
        }
        onewire_write_slots((const ow_pulse_t*)ctx.u.cmd, ds2413_build());
        ctx.phase = DS2413_PH_CMD;
        break;

    case DS2413_PH_CMD:
        onewire_read_data((volatile uint8_t*)ctx.u.pulse, DS2413_READ_BYTES);
        ctx.phase = DS2413_PH_READ;
        break;

    case DS2413_PH_READ:
        onewire_decode_pulses(ctx.raw, (const volatile uint8_t*)ctx.u.pulse,
                              DS2413_READ_BYTES);
        if (ctx.command == DS2413_CMD_READ) {
            if (ds2413_reply_valid(ctx.raw[0])) {
                /* The read byte carries PIOA in bit 1 and PIOB in bit 3 of
                 * its low nibble; normalize to the write layout (bit 0 =
                 * PIOA, bit 1 = PIOB) so callers can compare read and write
                 * states directly. */
                ctx.last_pio = (uint8_t)(((ctx.raw[0] & 0x02u) ? DS2413_PIOA : 0u) |
                                         ((ctx.raw[0] & 0x08u) ? DS2413_PIOB : 0u));
                if (ctx.out != 0) {
                    *ctx.out = ctx.last_pio;
                }
                ctx.flags |= DS2413_FLAG_OK;
            }
        } else if (ctx.raw[0] == DS2413_ACK) {
            ctx.flags |= DS2413_FLAG_OK;
        }
        if ((ctx.flags & DS2413_FLAG_OK) == 0u &&
            ++ctx.attempts < DS2413_MAX_ATTEMPTS) {
            onewire_strong_pullup(1);
            ow_start_wait(DS2413_PRIME_US);
            ctx.phase = DS2413_PH_PRIME;
            break;
        }
        ctx.phase = DS2413_PH_DONE;
        break;

    case DS2413_PH_DONE:
    case DS2413_PH_IDLE:
    default:
        break;
    }
    return 0;
}

uint8_t ds2413_read_pio_poll(void) {
    return ds2413_poll();
}

uint8_t ds2413_write_pio_poll(void) {
    return ds2413_poll();
}
