// Keyboard suite: /bin/login of the probe image. Prints every read of the
// console (stdin with no terminal) in hex on the serial port, one line per
// read, for tools/kbd-suite/drive.py to compare with the keys it pressed.
#include "ostest.h"

__attribute__((force_align_arg_pointer))
void _start(void){
    sw("\n[kbdprobe] ready\n");
    static const char hex[] = "0123456789abcdef";
    for(;;){
        unsigned char buf[64];
        i64 n = t_read(0, buf, sizeof(buf));
        if(n <= 0){ msleep(10); continue; }
        char line[200]; int k = 0;
        for(i64 i = 0; i < n && k < 190; i++){ line[k++] = hex[buf[i] >> 4]; line[k++] = hex[buf[i] & 15]; }
        line[k] = 0;
        sw("[kbdprobe] "); sw(line); sw("\n");
    }
}
