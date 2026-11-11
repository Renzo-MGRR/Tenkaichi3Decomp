#include <stdio.h>
#include <stddef.h>
#include "common.h"
#include "sys/common.h"

int main(void) {
    printf("sizeof(CommonRes)=0x%zX unk20=0x%zX size=0x%zX data=0x%zX\n",
           sizeof(CommonRes), offsetof(CommonRes, unk20),
           offsetof(CommonRes, size), offsetof(CommonRes, data));
    return 0;
}
