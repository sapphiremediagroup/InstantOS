#include <graphics/framebuffer.hpp>
#include <graphics/bsod.hpp>

void _bsod(){
    //fb->clear(0x0079d8);
    int x = 0, y = 0;
    for(size_t i = 0; i < sizeof(bsod) / sizeof(bsod[0]); i++){
        DrawPixel pixel = bsod[i];
        for(uint32_t j = 0; j < pixel.amountOf; j++){
            if(pixel.pixel != 0x0)
                //fb->putPixel(x, y, pixel.pixel);
            x++;
            if(x >= 1280){
                x = 0;
                y++;
            }
            if(y >= 720) return;
        }
    }
}