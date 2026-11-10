/*
 * PS3 start-up (the crt starts stdio before it calls here; see hello/ for the custom link script):
 *  - choose the game's data root and put it in BT3_DATA (the port reads getenv("BT3_DATA") in many
 *    places: plat_file.c, plat_mem.c, menu_text.c, ...), then check that the gamedata is there;
 *  - hand control to the game: Port_GameEnter moves onto the game's own 16 MB stack at 0x02000000
 *    (lv2_bt3.x placed the whole memory map) and runs game_main → main().
 *
 * The pads and the clock start lazily (ps3pad.c, ps3time.c). Gamedata lives under
 * /dev_hdd0/game/BT3P00001/USRDIR/gamedata; saves will use the title's native SaveData area.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include "ps3_config.h"
#include "ps3video.h"

/* Standard PSL1GHT process metadata; BT3 switches to its own 16 MB game stack later. */
SYS_PROCESS_PARAM(1001, 0x100000);

extern int Port_GameEnter(int argc, char **argv);

static int file_readable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

int __wrap_main(int argc, char **argv) { /* the link runs crt's call to main through here (--wrap=main) */
    const char *root = getenv("BT3_DATA");
    char probe[512];

    if (root == NULL || root[0] == '\0') {
        root = PS3_DATA_ROOT;
    }
    fprintf(stderr, "bt3-ps3: data root is %s\n", root);
    snprintf(probe, sizeof(probe), "%s/disc/SLUS_216.78", root);
    if (!file_readable(probe)) {
        fprintf(stderr, "bt3-ps3: %s is missing: the game's data (the copy of the disc that the setup program unpacks) is not in place\n"
                        "bt3-ps3: copy the gamedata folder from the PC port (C:\\Users\\Renzo\\Downloads\\Tenkaichi3Decomp\\gamedata) to the PS3\n",
                probe);
        return 2;
    }
    if (setenv("BT3_DATA", root, 1) != 0) {
        fprintf(stderr, "bt3-ps3: cannot set BT3_DATA\n");
        return 2;
    }
    fprintf(stderr, "bt3-ps3: gamedata is in place, starting RSX video\n");
    if (Port_Ps3VideoInit() != 0) {
        return 2;
    }
    fprintf(stderr, "bt3-ps3: video is ready, starting the game\n");
    return Port_GameEnter(argc, argv);
}
