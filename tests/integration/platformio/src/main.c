#include "ds18b20.h"

int main(void)
{
    ds18b20_init();
    ds18b20_poll();
    for (;;) {
    }
}
