#include "shell_skin.h"
#include "shell_main.h"

#include <stdio.h>

/** Easy as reading from stdinput */
int skin_getchar() {
    return fgetc(stdin);
}

// FAKE SKIN API
int skin_init_image(int type, int ncolors, const SkinColor *colors,
                    int width, int height){

}
void skin_put_pixels(unsigned const char *data){

}
void skin_finish_image(){
    
}
