/*
 * The pads on PS3: LV2's libpad (io/pad) → the game's PS2 18-byte pad frame.
 *
 * The frame (the game's own pads: scePad2Read via plat_stub.c, which gets here through Port_PadRead):
 *     data[0] = the left/right/down/up/start/R3/L3/select keys' bits, pressed = 0 (active low)
 *     data[1] = square/cross/circle/triangle/R1/L1/R2/L2, pressed = 0
 *     data[2..5] = right stick x, y, left stick x, y (0x80 at rest)
 *     data[6..17] = the pressure of right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
 *
 * libpad's padData is the same table in big-endian u16 fields with the value in the low byte
 * (verified by disassembling PSL1GHT's generated code for the struct, port/ps3/padprobe.sh):
 *     byte 9  = the first group's bits, pressed = 1 (active high): left 0x80 down 0x40 right 0x20
 *               up 0x10 start 0x08 R3 0x04 L3 0x02 select 0x01
 *     byte 11 = the second group: square 0x80 cross 0x40 circle 0x20 triangle 0x10 R1 0x08
 *               L1 0x04 R2 0x02 L2 0x01
 *     bytes 13, 15, 17, 19 = right x, right y, left x, left y
 *     bytes 21..43 = pressures, in the same order as the frame
 * So the conversion is: the two digital bytes inverted (the game's more is the PS2's less), the
 * stick bytes taken as they are, the pressure bytes taken as they are.
 */
#include <stdint.h>
#include <string.h>
#include <io/pad.h>

#define PD_PORTS 2

static int sPadOn;      /* 0 not yet started, -1 libpad refused, 1 running */
static unsigned sKnown; /* which ports were connected last read (bit per port) */
static int sSmallMotor[MAX_PADS], sLargeMotor[MAX_PADS];

static int pads_on(void) {
    if (sPadOn != 0) {
        return sPadOn > 0;
    }
    if (ioPadInit(PD_PORTS) != 0) {
        sPadOn = -1;
        return 0;
    }
    sPadOn = 1;
    return 1;
}

/* One pad's reading, as the 18-byte frame. Returns 0 when the pad is not attached. */
int Port_PadRead(int socket, unsigned char *data) {
    padInfo2 info;
    padData d;

    if (!pads_on() || socket < 0 || socket >= PD_PORTS) {
        return 0;
    }
    if (ioPadGetInfo2(&info) != 0 || (info.port_status[socket] & 1) == 0) {
        sKnown &= ~(1u << socket);
        return 0; /* plat_stub.c marks the pad idle */
    }
    if (!(sKnown & (1u << socket))) {
        sKnown |= 1u << socket;
        ioPadClearBuf(socket);                 /* drop what was in the buffer before the pad connected */
        ioPadSetPortSetting(socket, 3);        /* 1=pressure on, 2=sensors on: the game reads the pressure */
        sSmallMotor[socket] = sLargeMotor[socket] = 0; /* a new motor starts still */
    }
    if (ioPadGetData(socket, &d) != 0) {
        sKnown &= ~(1u << socket); /* gone or failing: idle again */
        return 0;
    }
    {
        const unsigned char *in = (const unsigned char *)&d;
        int i;
        data[0] = (unsigned char)~in[9];
        data[1] = (unsigned char)~in[11];
        for (i = 0; i < 4; i++) {
            data[2 + i] = in[13 + i * 2];
        }
        for (i = 0; i < 12; i++) {
            data[6 + i] = in[21 + i * 2];
        }
    }
    (void)sSmallMotor;
    return 18;
}

/* The game's motor command (plat_stub.c's sceVibSetActParam): small = the small motor's on/off,
   large = the large motor's power (bits 1..7). The controls are sent as they change, not every frame. */
void Port_PadVibrate(int socket, int small, int large) {
    padActParam act;

    if (!pads_on() || socket < 0 || socket >= PD_PORTS || !(sKnown & (1u << socket))) {
        return;
    }
    if ((small != 0) == (sSmallMotor[socket] != 0) && large == sLargeMotor[socket]) {
        return;
    }
    sSmallMotor[socket] = small;
    sLargeMotor[socket] = large;
    memset(&act, 0, sizeof(act));
    act.small_motor = (u8)(small != 0);
    act.large_motor = (u8)(large & 0x7F);
    ioPadSetActDirect((u32)socket, &act);
}
