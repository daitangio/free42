#ifndef SHELL_MAIN_H
#define SHELL_MAIN_H 1

#define FILENAMELEN 256

#define SHELL_VERSION 7

struct state_type {
    int extras;
    int printerToTxtFile;
    int printerToGifFile;
    char printerTxtFileName[FILENAMELEN];
    char printerGifFileName[FILENAMELEN];
    int printerGifMaxLength;
    char mainWindowKnown, printWindowKnown, printWindowMapped;
    int mainWindowX, mainWindowY;
    int printWindowX, printWindowY, printWindowHeight;
    char skinName[FILENAMELEN];
    int singleInstance;
    char coreName[FILENAMELEN];
    bool matrix_singularmatrix;
    bool matrix_outofrange;
    bool auto_repeat;
    bool old_repaint;
};

extern state_type state;

extern char free42dirname[FILENAMELEN];

#endif