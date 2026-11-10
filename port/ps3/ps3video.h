#ifndef BT3_PS3_VIDEO_H
#define BT3_PS3_VIDEO_H

#include <stdint.h>

int Port_Ps3VideoInit(void);
void Port_Ps3VideoPresent(const uint8_t *rgb, uint32_t width, uint32_t height);

#endif
