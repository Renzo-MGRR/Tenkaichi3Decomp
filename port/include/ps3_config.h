#ifndef BT3_PS3_CONFIG_H
#define BT3_PS3_CONFIG_H

/* Temporary XMB/save-data identity selected for the first PS3 package. */
#ifndef PS3_TITLE_ID
#define PS3_TITLE_ID "BT3P00001"
#endif

#define PS3_APP_ROOT "/dev_hdd0/game/" PS3_TITLE_ID "/USRDIR"
#define PS3_DATA_ROOT PS3_APP_ROOT "/gamedata"
#define PS3_EBOOT_PATH PS3_APP_ROOT "/EBOOT.BIN"

#endif
