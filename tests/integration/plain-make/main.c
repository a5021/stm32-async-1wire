#include "ds18b20.h"

int main(void)
{
    ds18b20_init();
    return ds18b20_crc8((const uint8_t *)"bluepill", 8) == 0;
}
