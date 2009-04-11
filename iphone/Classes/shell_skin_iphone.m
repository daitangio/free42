/*****************************************************************************
 * Free42 -- an HP-42S calculator simulator
 * Copyright (C) 2004-2009  Thomas Okken
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see http://www.gnu.org/licenses/.
 *****************************************************************************/

#import <UIKit/UIKit.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell_skin_iphone.h"
#include "shell_loadimage.h"
#include "core_main.h"


/**************************/
/* Skin description stuff */
/**************************/

typedef struct {
	int x, y;
} SkinPoint;

typedef struct {
	int x, y, width, height;
} SkinRect;

typedef struct {
	int code, shifted_code;
	SkinRect sens_rect;
	SkinRect disp_rect;
	SkinPoint src;
} SkinKey;

#define SKIN_MAX_MACRO_LENGTH 31

typedef struct _SkinMacro {
	int code;
	unsigned char macro[SKIN_MAX_MACRO_LENGTH + 1];
	struct _SkinMacro *next;
} SkinMacro;

typedef struct {
	SkinRect disp_rect;
	SkinPoint src;
} SkinAnnunciator;

static SkinRect skin;
static SkinPoint display_loc;
static SkinPoint display_scale;
static CGColorRef display_bg;
static CGColorRef display_fg;
static SkinKey *keylist = NULL;
static int nkeys = 0;
static int keys_cap = 0;
static int currently_pressed_key = -1;
static SkinMacro *macrolist = NULL;
static SkinAnnunciator annunciators[7];
static int annunciator_state[7];

static FILE *external_file;
static long builtin_length;
static long builtin_pos;
static const unsigned char *builtin_file;

static int skin_type;
static int skin_width, skin_height;
static int skin_ncolors;
static const SkinColor *skin_colors = NULL;
static int skin_y;
static CGImageRef skin_image = NULL;
static unsigned char *skin_bitmap = NULL;
static int skin_bytesperline;
static unsigned char *disp_bitmap = NULL;
static int disp_bytesperline;

static keymap_entry *keymap = NULL;
static int keymap_length = 0;

static bool display_enabled = true;


/**********************************************************/
/* Linked-in skins; defined in the skins.c, which in turn */
/* is generated by skin2c.c under control of skin2c.conf  */
/**********************************************************/

extern int skin_count;
extern const char *skin_name[];
extern long skin_layout_size[];
extern unsigned char *skin_layout_data[];
extern long skin_bitmap_size[];
extern unsigned char *skin_bitmap_data[];


/*****************/
/* Keymap parser */
/*****************/

keymap_entry *parse_keymap_entry(char *line, int lineno) {
	char *p;
	static keymap_entry entry;

	p =  strchr(line, '#');
	if (p != NULL)
		*p = 0;
	p = strchr(line, '\n');
	if (p != NULL)
		*p = 0;
	p = strchr(line, '\r');
	if (p != NULL)
		*p = 0;

	p = strchr(line, ':');
	if (p != NULL) {
		char *val = p + 1;
		char *tok;
		bool ctrl = false;
		bool alt = false;
		bool shift = false;
		bool cshift = false;
		int keycode = 0;
		int done = 0;
		unsigned char macro[KEYMAP_MAX_MACRO_LENGTH + 1];
		int macrolen = 0;

		/* Parse keycode */
		*p = 0;
		tok = strtok(line, " \t");
		while (tok != NULL) {
			if (done) {
				NSLog(@"Keymap, line %d: Excess tokens in key spec.", lineno);
				return NULL;
			}
			if (strcasecmp(tok, "ctrl") == 0)
				ctrl = true;
			else if (strcasecmp(tok, "alt") == 0)
				alt = true;
			else if (strcasecmp(tok, "shift") == 0)
				shift = true;
			else if (strcasecmp(tok, "cshift") == 0)
				cshift = true;
			else {
				char *endptr;
				long k = strtol(tok, &endptr, 10);
				if (k < 1 || *endptr != 0) {
					NSLog(@"Keymap, line %d: Bad keycode.", lineno);
					return NULL;
				}
				keycode = k;
				done = 1;
			}
			tok = strtok(NULL, " \t");
		}
		if (!done) {
			NSLog(@"Keymap, line %d: Unrecognized keycode.", lineno);
			return NULL;
		}

		/* Parse macro */
		tok = strtok(val, " \t");
		while (tok != NULL) {
			char *endptr;
			long k = strtol(tok, &endptr, 10);
			if (*endptr != 0 || k < 1 || k > 255) {
				NSLog(@"Keymap, line %d: Bad value (%s) in macro.", lineno, tok);
				return NULL;
			} else if (macrolen == KEYMAP_MAX_MACRO_LENGTH) {
				NSLog(@"Keymap, line %d: Macro too long (max=%d).\n", lineno, KEYMAP_MAX_MACRO_LENGTH);
				return NULL;
			} else
				macro[macrolen++] = (unsigned char) k;
			tok = strtok(NULL, " \t");
		}
		macro[macrolen] = 0;

		entry.ctrl = ctrl;
		entry.alt = alt;
		entry.shift = shift;
		entry.cshift = cshift;
		entry.keycode = keycode;
		strcpy((char *) entry.macro, (const char *) macro);
		return &entry;
	} else
		return NULL;
}


/*******************/
/* Local functions */
/*******************/

static int skin_open(const char *skinname, int open_layout);
static int skin_gets(char *buf, int buflen);
static void skin_close();


static int skin_open(const char *skinname, int open_layout) {
	int i;
	char namebuf[1024];

	/* Look for built-in skin first */
	for (i = 0; i < skin_count; i++) {
		if (strcmp(skinname, skin_name[i]) == 0) {
			external_file = NULL;
			builtin_pos = 0;
			if (open_layout) {
				builtin_length = skin_layout_size[i];
				builtin_file = skin_layout_data[i];
			} else {
				builtin_length = skin_bitmap_size[i];
				builtin_file = skin_bitmap_data[i];
			}
			return 1;
		}
	}

	/* name did not match a built-in skin; look for file */
	sprintf(namebuf, "%s.%s", skinname, open_layout ? "layout" : "gif");
	external_file = fopen(namebuf, "rb");
	return external_file != NULL;
}

int skin_getchar() {
	if (external_file != NULL)
		return fgetc(external_file);
	else if (builtin_pos < builtin_length)
		return builtin_file[builtin_pos++];
	else
		return EOF;
}

static int skin_gets(char *buf, int buflen) {
	int p = 0;
	int eof = -1;
	int comment = 0;
	while (p < buflen - 1) {
		int c = skin_getchar();
		if (eof == -1)
			eof = c == EOF;
		if (c == EOF || c == '\n' || c == '\r')
			break;
		/* Remove comments */
		if (c == '#')
			comment = 1;
		if (comment)
			continue;
		/* Suppress leading spaces */
		if (p == 0 && isspace(c))
			continue;
		buf[p++] = c;
	}
	buf[p++] = 0;
	return p > 1 || !eof;
}

void skin_rewind() {
	if (external_file != NULL)
		rewind(external_file);
	else
		builtin_pos = 0;
}

static void skin_close() {
	if (external_file != NULL)
		fclose(external_file);
}

static void MyProviderReleaseData(void *info,  const void *data, size_t size) {
	free((void *) data);
}

static void MyProviderReleaseData2(void *info,  const void *data, size_t size) {
	//free((void *) data);
}

void skin_load(NSString *nsskinname, long *width, long *height) {
	char line[1024];
	int success;
	int size;
	int kmcap = 0;
	int lineno = 0;

	const char *skinname = [nsskinname cStringUsingEncoding:NSUTF8StringEncoding];
	if (skinname[0] == 0) {
		fallback_on_1st_builtin_skin:
		skinname = skin_name[0];
	}

	/*************************/
	/* Load skin description */
	/*************************/

	if (!skin_open(skinname, 1))
		goto fallback_on_1st_builtin_skin;

	if (keylist != NULL)
		free(keylist);
	keylist = NULL;
	nkeys = 0;
	keys_cap = 0;

	while (macrolist != NULL) {
		SkinMacro *m = macrolist->next;
		free(macrolist);
		macrolist = m;
	}

	if (keymap != NULL)
	    free(keymap);
	keymap = NULL;
	keymap_length = 0;

	while (skin_gets(line, 1024)) {
		lineno++;
		if (*line == 0)
			continue;
		if (strncasecmp(line, "skin:", 5) == 0) {
			int x, y, width, height;
			if (sscanf(line + 5, " %d,%d,%d,%d", &x, &y, &width, &height) == 4){
				skin.x = x;
				skin.y = y;
				skin.width = width;
				skin.height = height;
			}
		} else if (strncasecmp(line, "display:", 8) == 0) {
			int x, y, xscale, yscale;
			unsigned long bg, fg;
			if (sscanf(line + 8, " %d,%d %d %d %lx %lx", &x, &y,
											&xscale, &yscale, &bg, &fg) == 6) {
				display_loc.x = x;
				display_loc.y = y;
				display_scale.x = xscale;
				display_scale.y = yscale;
				CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
				CGFloat comps[4];
				comps[0] = ((bg >> 16) & 255) / 255.0;
				comps[1] = ((bg >> 8) & 255) / 255.0;
				comps[2] = (bg & 255) / 255.0;
				comps[3] = 1.0;
				display_bg = CGColorCreate(color_space, comps);
				comps[0] = ((fg >> 16) & 255) / 255.0;
				comps[1] = ((fg >> 8) & 255) / 255.0;
				comps[2] = (fg & 255) / 255.0;
				display_fg = CGColorCreate(color_space, comps);
				CGColorSpaceRelease(color_space);
			}
		} else if (strncasecmp(line, "key:", 4) == 0) {
			char keynumbuf[20];
			int keynum, shifted_keynum;
			int sens_x, sens_y, sens_width, sens_height;
			int disp_x, disp_y, disp_width, disp_height;
			int act_x, act_y;
			if (sscanf(line + 4, " %s %d,%d,%d,%d %d,%d,%d,%d %d,%d",
								 keynumbuf,
								 &sens_x, &sens_y, &sens_width, &sens_height,
								 &disp_x, &disp_y, &disp_width, &disp_height,
								 &act_x, &act_y) == 11) {
				int n = sscanf(keynumbuf, "%d,%d", &keynum, &shifted_keynum);
				if (n > 0) {
					if (n == 1)
						shifted_keynum = keynum;
					SkinKey *key;
					if (nkeys == keys_cap) {
						keys_cap += 50;
						keylist = (SkinKey *) realloc(keylist, keys_cap * sizeof(SkinKey));
						// TODO - handle memory allocation failure
					}
					key = keylist + nkeys;
					key->code = keynum;
					key->shifted_code = shifted_keynum;
					key->sens_rect.x = sens_x;
					key->sens_rect.y = sens_y;
					key->sens_rect.width = sens_width;
					key->sens_rect.height = sens_height;
					key->disp_rect.x = disp_x;
					key->disp_rect.y = disp_y;
					key->disp_rect.width = disp_width;
					key->disp_rect.height = disp_height;
					key->src.x = act_x;
					key->src.y = act_y;
					nkeys++;
				}
			}
		} else if (strncasecmp(line, "macro:", 6) == 0) {
			char *tok = strtok(line + 6, " ");
			int len = 0;
			SkinMacro *macro = NULL;
			while (tok != NULL) {
				char *endptr;
				long n = strtol(tok, &endptr, 10);
				if (*endptr != 0) {
					/* Not a proper number; ignore this macro */
					if (macro != NULL) {
						free(macro);
						macro = NULL;
						break;
					}
				}
				if (macro == NULL) {
					if (n < 38 || n > 255)
						/* Macro code out of range; ignore this macro */
						break;
					macro = (SkinMacro *) malloc(sizeof(SkinMacro));
					// TODO - handle memory allocation failure
					macro->code = n;
				} else if (len < SKIN_MAX_MACRO_LENGTH) {
					if (n < 1 || n > 37) {
						/* Key code out of range; ignore this macro */
						free(macro);
						macro = NULL;
						break;
					}
					macro->macro[len++] = (unsigned char) n;
				}
				tok = strtok(NULL, " ");
			}
			if (macro != NULL) {
				macro->macro[len++] = 0;
				macro->next = macrolist;
				macrolist = macro;
			}
		} else if (strncasecmp(line, "annunciator:", 12) == 0) {
			int annnum;
			int disp_x, disp_y, disp_width, disp_height;
			int act_x, act_y;
			if (sscanf(line + 12, " %d %d,%d,%d,%d %d,%d",
								  &annnum,
								  &disp_x, &disp_y, &disp_width, &disp_height,
								  &act_x, &act_y) == 7) {
				if (annnum >= 1 && annnum <= 7) {
					SkinAnnunciator *ann = annunciators + (annnum - 1);
					ann->disp_rect.x = disp_x;
					ann->disp_rect.y = disp_y;
					ann->disp_rect.width = disp_width;
					ann->disp_rect.height = disp_height;
					ann->src.x = act_x;
					ann->src.y = act_y;
				}
			}
		} else if (strchr(line, ':') != 0) {
			keymap_entry *entry = parse_keymap_entry(line, lineno);
			if (entry != NULL) {
				if (keymap_length == kmcap) {
					kmcap += 50;
					keymap = (keymap_entry *) realloc(keymap, kmcap * sizeof(keymap_entry));
					// TODO - handle memory allocation failure
				}
				memcpy(keymap + (keymap_length++), entry, sizeof(keymap_entry));
			}
		}
	}

	skin_close();

	/********************/
	/* Load skin bitmap */
	/********************/

	if (!skin_open(skinname, 0))
		goto fallback_on_1st_builtin_skin;

	/* shell_loadimage() calls skin_getchar() and skin_rewind() to load the
	 * image from the compiled-in or on-disk file; it calls skin_init_image(),
	 * skin_put_pixels(), and skin_finish_image() to create the in-memory
	 * representation.
	 */
	success = shell_loadimage();
	skin_close();

	if (!success)
		goto fallback_on_1st_builtin_skin;

	*width = skin.width;
	*height = skin.height;

	/********************************/
	/* (Re)build the display bitmap */
	/********************************/

	if (disp_bitmap != NULL)
		free(disp_bitmap);
	disp_bytesperline = 17; // bytes needed for 131 pixels
	size = disp_bytesperline * 16;
	disp_bitmap = (unsigned char *) malloc(size);
	// TODO - handle memory allocation failure
	memset(disp_bitmap, 255, size);
}

int skin_init_image(int type, int ncolors, const SkinColor *colors,
					int width, int height) {
	if (skin_image != NULL) {
		CGImageRelease(skin_image);
		skin_image = NULL;
		skin_bitmap = NULL;
	}

	skin_type = type;
	skin_ncolors = ncolors;
	skin_colors = colors;
	
	switch (type) {
		case IMGTYPE_MONO:
			skin_bytesperline = (width + 7) >> 3;
			break;
		case IMGTYPE_GRAY:
			skin_bytesperline = width;
			break;
		case IMGTYPE_TRUECOLOR:
		case IMGTYPE_COLORMAPPED:
			skin_bytesperline = width * 3;
			break;
		default:
			return 0;
	}

	skin_bitmap = (unsigned char *) malloc(skin_bytesperline * height);
	// TODO - handle memory allocation failure
	skin_width = width;
	skin_height = height;
	skin_y = skin_height;
	return skin_bitmap != NULL;
}

void skin_put_pixels(unsigned const char *data) {
	skin_y--;
	unsigned char *dst = skin_bitmap + skin_y * skin_bytesperline;
	if (skin_type == IMGTYPE_COLORMAPPED) {
		int src_bytesperline = skin_bytesperline / 3;
		for (int i = 0; i < src_bytesperline; i++) {
			int index = data[i] & 255;
			const SkinColor *c = skin_colors + index;
			*dst++ = c->r;
			*dst++ = c->g;
			*dst++ = c->b;
		}
	} else
		memcpy(dst, data, skin_bytesperline);
}

void skin_finish_image() {
	int bits_per_component;
	int bits_per_pixel;
	CGColorSpaceRef color_space;
	
	switch (skin_type) {
		case IMGTYPE_MONO:
			bits_per_component = 1;
			bits_per_pixel = 1;
			color_space = CGColorSpaceCreateDeviceGray();
			break;
		case IMGTYPE_GRAY:
			bits_per_component = 8;
			bits_per_pixel = 8;
			color_space = CGColorSpaceCreateDeviceGray();
			break;
		case IMGTYPE_COLORMAPPED:
		case IMGTYPE_TRUECOLOR:
			bits_per_component = 8;
			bits_per_pixel = 24;
			color_space = CGColorSpaceCreateDeviceRGB();
			break;
	}
	
	int bytes_per_line = (skin_width * bits_per_pixel + 7) >> 3;

	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, skin_bitmap, bytes_per_line * skin_height, MyProviderReleaseData);
	skin_image = CGImageCreate(skin_width, skin_height, bits_per_component, bits_per_pixel, bytes_per_line,
							   color_space, kCGBitmapByteOrder32Big, provider, NULL, false, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(color_space);
	skin_bitmap = NULL;
}

void skin_repaint(CGRect *rect) {
	CGContextRef myContext = UIGraphicsGetCurrentContext();
	
	// Optimize for the common case that *only* the display needs painting
	bool paintOnlyDisplay = rect->origin.x >= display_loc.x && rect->origin.y >= display_loc.y
				&& rect->origin.x + rect->size.width <= display_loc.x + display_scale.x * 131
				&& rect->origin.y + rect->size.height <= display_loc.y + display_scale.y * 16;
	
	if (!paintOnlyDisplay) {
		// Paint black background (in case the skin is smaller than the available area)
		// TODO: What *is* the available area?
		CGContextSetRGBFillColor(myContext, 0.0, 0.0, 0.0, 1.0);
		CGContextFillRect(myContext, CGRectMake(0, 0, 320, 480));
		
		// Redisplay skin
		CGImageRef si = CGImageCreateWithImageInRect(skin_image, CGRectMake(skin.x, skin_height - (skin.y + skin.height), skin.width, skin.height));
		CGContextDrawImage(myContext, CGRectMake(0, 0, skin.width, skin.height), si);
		CGImageRelease(si);
	
		// Repaint pressed hard key, if any
		if (currently_pressed_key >= 0 && currently_pressed_key < nkeys) {
			SkinKey *k = keylist + currently_pressed_key;
			CGImageRef key_image = CGImageCreateWithImageInRect(skin_image, CGRectMake(k->src.x, skin_height - (k->src.y + k->disp_rect.height), k->disp_rect.width, k->disp_rect.height));
			CGContextDrawImage(myContext, CGRectMake(k->disp_rect.x, k->disp_rect.y, k->disp_rect.width, k->disp_rect.height), key_image);
			CGImageRelease(key_image);
		}
	}
	
	// Repaint display (and pressed softkey, if any)
	CGContextSaveGState(myContext);
	CGContextTranslateCTM(myContext, display_loc.x, display_loc.y);
	CGContextScaleCTM(myContext, display_scale.x, display_scale.y);

	int x1 = (int) ((rect->origin.x - display_loc.x) / display_scale.x);
	int y1 = (int) ((rect->origin.y - display_loc.y) / display_scale.y);
	int x2 = (int) ((rect->origin.x + rect->size.width - display_loc.x) / display_scale.x);
	int y2 = (int) ((rect->origin.y + rect->size.height - display_loc.y) / display_scale.y);
	if (x1 < 0)
		x1 = 0;
	else if (x1 > 131)
		x1 = 131;
	if (y1 < 0)
		y1 = 0;
	else if (y1 > 16)
		y1 = 16;
	if (x2 < x1)
		x2 = x1;
	else if (x2 > 131)
		x2 = 131;
	if (y2 < y1)
		y2 = y1;
	else if (y2 > 16)
		y2 = 16;
	
	if (x2 > x1 && y2 > y1) {
		CGContextSetFillColorWithColor(myContext, display_bg);
		CGContextFillRect(myContext, CGRectMake(x1, y1, x2 - x1, y2 - y1));
		CGContextSetFillColorWithColor(myContext, display_fg);
		bool softkey_pressed = currently_pressed_key >= -7 && currently_pressed_key <= -2;
		int skx1, skx2, sky1, sky2;
		if (softkey_pressed) {
			skx1 = (-2 - currently_pressed_key) * 22;
			skx2 = skx1 + 21;
			sky1 = 9;
			sky2 = 16;
		}
		for (int v = y1; v < y2; v++) {
			for (int h = x1; h < x2; h++) {
				int pixel = (disp_bitmap[v * disp_bytesperline + (h >> 3)] & (128 >> (h & 7))) != 0;
				if (softkey_pressed && h >= skx1 && h < skx2 && v >= sky1 && v < sky2)
					pixel = !pixel;
				if (pixel)
					CGContextFillRect(myContext, CGRectMake(h, v, 1, 1));
			}
		}
	}
	
	CGContextRestoreGState(myContext);	

	if (!paintOnlyDisplay) {
		// Repaint annunciators
		for (int i = 0; i < 7; i++) {
			if (annunciator_state[i]) {
				SkinAnnunciator *ann = annunciators + i;
				CGImageRef ann_image = CGImageCreateWithImageInRect(skin_image, CGRectMake(ann->src.x, skin_height - (ann->src.y + ann->disp_rect.height), ann->disp_rect.width, ann->disp_rect.height));
				CGContextDrawImage(myContext, CGRectMake(ann->disp_rect.x, ann->disp_rect.y, ann->disp_rect.width, ann->disp_rect.height), ann_image);
				CGImageRelease(ann_image);
			}
		}
	}
}

void skin_update_annunciator(int which, int state, MainView *view) {
	if (which < 1 || which > 7)
		return;
	which--;
	if (annunciator_state[which] == state)
		return;
	annunciator_state[which] = state;
	SkinRect *r = &annunciators[which].disp_rect;
	[view setNeedsDisplayInRectSafely:CGRectMake(r->x, r->y, r->width, r->height)];
}
	
void skin_find_key(int x, int y, bool cshift, int *skey, int *ckey) {
	int i;
	if (core_menu()
			&& x >= display_loc.x
			&& x < display_loc.x + 131 * display_scale.x
			&& y >= display_loc.y + 9 * display_scale.y
			&& y < display_loc.y + 16 * display_scale.y) {
		int softkey = (x - display_loc.x) / (22 * display_scale.x) + 1;
		*skey = -1 - softkey;
		*ckey = softkey;
		return;
	}
	for (i = 0; i < nkeys; i++) {
		SkinKey *k = keylist + i;
		int rx = x - k->sens_rect.x;
		int ry = y - k->sens_rect.y;
		if (rx >= 0 && rx < k->sens_rect.width
				&& ry >= 0 && ry < k->sens_rect.height) {
			*skey = i;
			*ckey = cshift ? k->shifted_code : k->code;
			return;
		}
	}
	*skey = -1;
	*ckey = 0;
}

int skin_find_skey(int ckey) {
	int i;
	for (i = 0; i < nkeys; i++)
		if (keylist[i].code == ckey || keylist[i].shifted_code == ckey)
			return i;
	return -1;
}

unsigned char *skin_find_macro(int ckey) {
	SkinMacro *m = macrolist;
	while (m != NULL) {
		if (m->code == ckey)
			return m->macro;
		m = m->next;
	}
	return NULL;
}

/*
unsigned char *skin_keymap_lookup(int keycode, bool ctrl, bool alt, bool shift, bool cshift, bool *exact) {
	int i;
	unsigned char *macro = NULL;
	for (i = 0; i < keymap_length; i++) {
		keymap_entry *entry = keymap + i;
		if (keycode == entry->keycode
				&& ctrl == entry->ctrl
				&& alt == entry->alt
				&& shift == entry->shift) {
			macro = entry->macro;
			if (cshift == entry->cshift) {
				*exact = true;
				return macro;
			}
		}
	}
	*exact = false;
	return macro;
}
 */

static void invalidate_key(int key, MainView *view) {
	if (key == -1)
		return;
	if (key >= -7 && key <= -2) {
		int k = -1 - key;
		int x = (k - 1) * 22 * display_scale.x + display_loc.x;
		int y = 9 * display_scale.y + display_loc.y;
		int w = 21 * display_scale.x;
		int h = 7 * display_scale.y;
		[view setNeedsDisplayInRectSafely:CGRectMake(x, y, w, h)];
	} else if (key >= 0 && key < nkeys) {
		SkinRect *r = &keylist[key].disp_rect;
		[view setNeedsDisplayInRectSafely:CGRectMake(r->x, r->y, r->width, r->height)];
	}
}

void skin_set_pressed_key(int key, MainView *view) {
	if (key == currently_pressed_key)
		return;
	invalidate_key(currently_pressed_key, view);
	currently_pressed_key = key;
	invalidate_key(currently_pressed_key, view);
}
	
void skin_display_blitter(const char *bits, int bytesperline, int x, int y, int width, int height, MainView *view) {
	int h, v;

	for (v = y; v < y + height; v++)
		for (h = x; h < x + width; h++) {
			int pixel = (bits[v * bytesperline + (h >> 3)] & (1 << (h & 7))) == 0;
			if (pixel)
				disp_bitmap[v * disp_bytesperline + (h >> 3)] &= ~(128 >> (h & 7));
			else
				disp_bitmap[v * disp_bytesperline + (h >> 3)] |= 128 >> (h & 7);
		}
	
	[view setNeedsDisplayInRectSafely:CGRectMake(display_loc.x + x * display_scale.x,
												 display_loc.y + y * display_scale.y,
												 width * display_scale.x,
												 height * display_scale.y)];
}

void skin_repaint_display(MainView *view) {
	if (!display_enabled)
		// Prevent screen flashing during macro execution
		return;
	[view setNeedsDisplayInRectSafely:CGRectMake(display_loc.x, display_loc.y, 131 * display_scale.x, 16 * display_scale.y)];
}

void skin_display_set_enabled(bool enable) {
	display_enabled = enable;
}
