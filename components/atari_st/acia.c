/*
 * MC6850 ACIAs ($FFFC00 keyboard, $FFFC04 MIDI) and the HD6301 IKBD
 * behind the keyboard ACIA: keyboard, mouse, joysticks and its clock.
 *
 * Host input arrives from another task through a lock-free queue and is
 * turned into IKBD packets on the emulation side. IKBD bytes are delivered
 * at the real 7812.5 baud pace.
 */
#include <string.h>
#include <time.h>
#include "st_internal.h"

#define ACIA_RDRF           0x01
#define ACIA_TDRE           0x02
#define ACIA_OVRN           0x20
#define ACIA_IRQ            0x80
#define ACIA_CR_RX_IRQ      0x80
#define ACIA_CR_TX_MASK     0x60
#define ACIA_CR_TX_IRQ      0x20

/* 10 bits per byte at 7812.5 baud */
#define IKBD_BYTE_CYCLES    (ST_CPU_HZ / 781)

#define OUT_QUEUE_SIZE      256
#define EVENT_QUEUE_SIZE    64

typedef struct {
    uint8_t control;
    uint8_t status;
    uint8_t rx_data;
} acia_t;

enum { EV_KEY, EV_MOUSE, EV_JOYSTICK };

typedef struct {
    uint8_t type;
    uint8_t code;           /* key scancode, joystick port */
    uint8_t value;          /* key pressed, mouse buttons, joystick state */
    int16_t dx, dy;
} input_event_t;

enum { MOUSE_RELATIVE, MOUSE_ABSOLUTE, MOUSE_KEYCODE, MOUSE_OFF };
enum { JOY_EVENT, JOY_INTERROGATE, JOY_OFF };

static struct {
    acia_t kbd, midi;

    /* IKBD -> ACIA byte stream */
    uint8_t out[OUT_QUEUE_SIZE];
    uint16_t out_head, out_tail;
    int out_delay;

    /* ACIA -> IKBD command parser */
    uint8_t cmd[8];
    int cmd_len, cmd_need;

    int mouse_mode;
    int joy_mode;
    bool y_at_bottom;
    bool paused;
    uint8_t buttons;            /* bit 1 left, bit 0 right (IKBD order) */
    int abs_x, abs_y, abs_max_x, abs_max_y;
    uint8_t joystick[2];

    /* Time-of-day clock: host time + offset set by the ST */
    time_t clock_offset;
} ikbd;

/* Single producer (host input task), single consumer (emulation) */
static input_event_t events[EVENT_QUEUE_SIZE];
static volatile uint32_t ev_head, ev_tail;

static void push_event(const input_event_t *ev)
{
    uint32_t head = __atomic_load_n(&ev_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&ev_tail, __ATOMIC_ACQUIRE);
    if (head - tail >= EVENT_QUEUE_SIZE) {
        return;     /* full: drop */
    }
    events[head % EVENT_QUEUE_SIZE] = *ev;
    __atomic_store_n(&ev_head, head + 1, __ATOMIC_RELEASE);
}

void st_ikbd_key(uint8_t scancode, bool pressed)
{
    input_event_t ev = { .type = EV_KEY, .code = scancode & 0x7F, .value = pressed };
    push_event(&ev);
}

void st_ikbd_mouse(int dx, int dy, bool left, bool right)
{
    input_event_t ev = {
        .type = EV_MOUSE,
        .value = (left ? 2 : 0) | (right ? 1 : 0),
        .dx = dx, .dy = dy,
    };
    push_event(&ev);
}

void st_ikbd_joystick(int port, uint8_t state)
{
    input_event_t ev = { .type = EV_JOYSTICK, .code = port & 1, .value = state };
    push_event(&ev);
}

/* ---- ACIA ---- */

static void update_irq_line(void)
{
    acia_t *acias[2] = { &ikbd.kbd, &ikbd.midi };
    bool irq = false;
    for (int i = 0; i < 2; i++) {
        acia_t *a = acias[i];
        bool rx = (a->control & ACIA_CR_RX_IRQ) && (a->status & ACIA_RDRF);
        bool tx = (a->control & ACIA_CR_TX_MASK) == ACIA_CR_TX_IRQ && (a->status & ACIA_TDRE);
        if (rx || tx) {
            a->status |= ACIA_IRQ;
            irq = true;
        } else {
            a->status &= ~ACIA_IRQ;
        }
    }
    mfp_set_gpip(MFP_GPIP_ACIA, !irq);
}

static void acia_master_reset(acia_t *a)
{
    a->status = ACIA_TDRE;
    a->rx_data = 0;
}

/* ---- IKBD output ---- */

static void out_byte(uint8_t b)
{
    if ((uint16_t)(ikbd.out_head - ikbd.out_tail) < OUT_QUEUE_SIZE) {
        ikbd.out[ikbd.out_head++ % OUT_QUEUE_SIZE] = b;
    }
}

static int clamp8(int v)
{
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

static uint8_t bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static int from_bcd(uint8_t b)
{
    return (b >> 4) * 10 + (b & 0x0F);
}

static void send_abs_position(void)
{
    int y = ikbd.y_at_bottom ? ikbd.abs_max_y - ikbd.abs_y : ikbd.abs_y;
    out_byte(0xF7);
    out_byte(ikbd.buttons);
    out_byte(ikbd.abs_x >> 8);
    out_byte(ikbd.abs_x);
    out_byte(y >> 8);
    out_byte(y);
}

static void handle_event(const input_event_t *ev)
{
    switch (ev->type) {
    case EV_KEY:
        out_byte(ev->value ? ev->code : (ev->code | 0x80));
        break;

    case EV_MOUSE: {
        ikbd.buttons = ev->value;
        if (ikbd.mouse_mode == MOUSE_RELATIVE) {
            int dx = ev->dx;
            int dy = ikbd.y_at_bottom ? -ev->dy : ev->dy;
            do {
                int px = clamp8(dx), py = clamp8(dy);
                out_byte(0xF8 | ikbd.buttons);
                out_byte(px);
                out_byte(py);
                dx -= px;
                dy -= py;
            } while (dx || dy);
        } else if (ikbd.mouse_mode == MOUSE_ABSOLUTE) {
            ikbd.abs_x += ev->dx;
            ikbd.abs_y += ev->dy;
            ikbd.abs_x = ikbd.abs_x < 0 ? 0 : ikbd.abs_x > ikbd.abs_max_x ? ikbd.abs_max_x : ikbd.abs_x;
            ikbd.abs_y = ikbd.abs_y < 0 ? 0 : ikbd.abs_y > ikbd.abs_max_y ? ikbd.abs_max_y : ikbd.abs_y;
        }
        break;
    }

    case EV_JOYSTICK:
        ikbd.joystick[ev->code] = ev->value;
        if (ikbd.joy_mode == JOY_EVENT) {
            out_byte(0xFE + ev->code);
            out_byte(ev->value);
        }
        break;
    }
}

/* Number of parameter bytes that follow each IKBD command */
static int command_params(uint8_t c)
{
    switch (c) {
    case 0x80: return 1;
    case 0x07: return 1;
    case 0x09: return 4;
    case 0x0A: return 2;
    case 0x0B: return 2;
    case 0x0C: return 2;
    case 0x0E: return 5;
    case 0x17: return 1;
    case 0x19: return 6;
    case 0x1B: return 6;
    case 0x20: return 3;
    case 0x21: return 2;
    case 0x22: return 2;
    default:   return 0;
    }
}

static void ikbd_reset(void)
{
    ikbd.mouse_mode = MOUSE_RELATIVE;
    ikbd.joy_mode = JOY_EVENT;
    ikbd.y_at_bottom = false;
    ikbd.paused = false;
    ikbd.buttons = 0;
    ikbd.abs_x = ikbd.abs_y = 0;
    ikbd.abs_max_x = 320;
    ikbd.abs_max_y = 200;
    ikbd.out_head = ikbd.out_tail = 0;
}

/* The IKBD clock follows the host's local time; setting it from the ST
 * keeps an offset.
 */
static time_t clock_now(void)
{
    return time(NULL) + ikbd.clock_offset;
}

static void clock_set(int year, int month, int day, int hour, int minute, int second)
{
    struct tm tm = {
        .tm_year = (year < 80 ? year + 100 : year), .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_sec = second, .tm_isdst = -1,
    };
    ikbd.clock_offset = mktime(&tm) - time(NULL);
}

static void run_command(const uint8_t *c)
{
    switch (c[0]) {
    case 0x80:
        if (c[1] == 0x01) {
            ikbd_reset();
            out_byte(0xF1);     /* reset complete, firmware version */
        }
        break;
    case 0x08: ikbd.mouse_mode = MOUSE_RELATIVE; break;
    case 0x09:
        ikbd.mouse_mode = MOUSE_ABSOLUTE;
        ikbd.abs_max_x = (c[1] << 8) | c[2];
        ikbd.abs_max_y = (c[3] << 8) | c[4];
        break;
    case 0x0A: ikbd.mouse_mode = MOUSE_KEYCODE; break;
    case 0x0D: send_abs_position(); break;
    case 0x0E:
        ikbd.abs_x = (c[2] << 8) | c[3];
        ikbd.abs_y = (c[4] << 8) | c[5];
        break;
    case 0x0F: ikbd.y_at_bottom = true; break;
    case 0x10: ikbd.y_at_bottom = false; break;
    case 0x11: ikbd.paused = false; break;
    case 0x12: ikbd.mouse_mode = MOUSE_OFF; break;
    case 0x13: ikbd.paused = true; break;
    case 0x14: ikbd.joy_mode = JOY_EVENT; break;
    case 0x15: ikbd.joy_mode = JOY_INTERROGATE; break;
    case 0x16:
        out_byte(0xFD);
        out_byte(ikbd.joystick[0]);
        out_byte(ikbd.joystick[1]);
        break;
    case 0x1A: ikbd.joy_mode = JOY_OFF; break;
    case 0x1B:
        clock_set(from_bcd(c[1]), from_bcd(c[2]), from_bcd(c[3]),
                  from_bcd(c[4]), from_bcd(c[5]), from_bcd(c[6]));
        break;
    case 0x1C: {
        time_t t = clock_now();
        struct tm tm;
        localtime_r(&t, &tm);
        out_byte(0xFC);
        out_byte(bcd(tm.tm_year % 100));
        out_byte(bcd(tm.tm_mon + 1));
        out_byte(bcd(tm.tm_mday));
        out_byte(bcd(tm.tm_hour));
        out_byte(bcd(tm.tm_min));
        out_byte(bcd(tm.tm_sec));
        break;
    }
    default:
        break;      /* threshold, scale, keycode joystick, memory access: ignored */
    }
}

static void ikbd_receive(uint8_t b)
{
    if (ikbd.cmd_len == 0) {
        ikbd.cmd_need = 1 + command_params(b);
    }
    ikbd.cmd[ikbd.cmd_len++] = b;
    if (ikbd.cmd_len >= ikbd.cmd_need) {
        run_command(ikbd.cmd);
        ikbd.cmd_len = 0;
    }
}

/* Called once per scanline */
void acia_advance(int cpu_cycles)
{
    uint32_t tail = ev_tail;
    uint32_t head = __atomic_load_n(&ev_head, __ATOMIC_ACQUIRE);
    while (tail != head) {
        handle_event(&events[tail % EVENT_QUEUE_SIZE]);
        tail++;
    }
    __atomic_store_n(&ev_tail, tail, __ATOMIC_RELEASE);

    if (ikbd.out_delay > 0) {
        ikbd.out_delay -= cpu_cycles;
        return;
    }
    if (ikbd.paused || ikbd.out_head == ikbd.out_tail) {
        return;
    }
    acia_t *a = &ikbd.kbd;
    if (a->status & ACIA_RDRF) {
        a->status |= ACIA_OVRN;     /* previous byte not read in time */
    }
    a->rx_data = ikbd.out[ikbd.out_tail++ % OUT_QUEUE_SIZE];
    a->status |= ACIA_RDRF;
    ikbd.out_delay = IKBD_BYTE_CYCLES;
    update_irq_line();
}

void acia_reset(void)
{
    acia_master_reset(&ikbd.kbd);
    acia_master_reset(&ikbd.midi);
    ikbd.kbd.control = ikbd.midi.control = 0;
    ikbd.cmd_len = 0;
    ikbd.out_delay = 0;
    ikbd_reset();
    update_irq_line();
}

uint8_t acia_read(uint32_t address)
{
    acia_t *a = (address & 4) ? &ikbd.midi : &ikbd.kbd;
    if (address & 1) {
        return 0xFF;
    }
    if ((address & 2) == 0) {
        return a->status;
    }
    uint8_t data = a->rx_data;
    a->status &= ~(ACIA_RDRF | ACIA_OVRN);
    update_irq_line();
    return data;
}

void acia_write(uint32_t address, uint8_t value)
{
    acia_t *a = (address & 4) ? &ikbd.midi : &ikbd.kbd;
    if (address & 1) {
        return;
    }
    if ((address & 2) == 0) {
        if ((value & 0x03) == 0x03) {
            acia_master_reset(a);
        }
        a->control = value;
    } else if (a == &ikbd.kbd) {
        ikbd_receive(value);
    }
    update_irq_line();
}
