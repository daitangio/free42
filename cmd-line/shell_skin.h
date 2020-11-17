#ifndef SHELL_SKIN_H
#define SHELL_SKIN_H 1
typedef struct {
    unsigned char r, g, b, pad;
} SkinColor;

#define IMGTYPE_MONO 1
#define IMGTYPE_GRAY 2
#define IMGTYPE_COLORMAPPED 3
#define IMGTYPE_TRUECOLOR 4

// GG Minimal API needed to compile shell_skin.

// keymap_entry *parse_keymap_entry(char *line, int lineno);

int skin_getchar();
void skin_rewind();

// No implementation will be provided below this line (fake methos)
int skin_init_image(int type, int ncolors, const SkinColor *colors,
                    int width, int height);
void skin_put_pixels(unsigned const char *data);
void skin_finish_image();

#endif
