#include <errno.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include "shell.h"

#include "shell_main.h"
#include <iostream>

static bool decimal_point;
state_type state;
char free42dirname[FILENAMELEN];

static FILE *statefile = NULL;
static char statefilename[FILENAMELEN];
static char printfilename[FILENAMELEN];
#include "core_main.h"


// Private functions
static void init_shell_state(int4 version);
static int read_shell_state(int4 *version);
static int write_shell_state();
static bool file_exists(const char *name);


static void activate() {



    // Capture state of decimal_point, which may have been changed by
    // gtk_init(), and then set it to the C locale, because the binary/decimal
    // conversions expect a decimal point, not a comma.
    struct lconv *loc = localeconv();
    decimal_point = strcmp(loc->decimal_point, ",") != 0;
    setlocale(LC_NUMERIC, "C");


    /*************************************************************/
    /***** Try to create the $XDG_DATA_HOME/free42 directory *****/
    /*************************************************************/

    char keymapfilename[FILENAMELEN];

    char *xdg_data_home = getenv("XDG_DATA_HOME");
    char *home = getenv("HOME");

    if (xdg_data_home == NULL || xdg_data_home[0] == 0)
        snprintf(free42dirname, FILENAMELEN, "%s/.local/share/free42", home);
    else
        snprintf(free42dirname, FILENAMELEN, "%s/free42", xdg_data_home);

    if (!file_exists(free42dirname)) {
        // The Free42 directory does not exist yet. Before trying to do
        // anything else, make sure the Free42 directory path starts with a slash.
        if (free42dirname[0] != '/') {
            fprintf(stderr, "Fatal: XDG_DATA_HOME or HOME are invalid; must start with '/'\n");
            exit(1);
        }
        // If $HOME/.free42 does exist, move it to the new location.
        char old_free42dirname[FILENAMELEN];
        snprintf(old_free42dirname, FILENAMELEN, "%s/.free42", home);
        bool have_old = false;
        struct stat st;
        if (lstat(old_free42dirname, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                const char *dest;
                if (xdg_data_home == NULL || xdg_data_home[0] == 0)
                    dest = "$HOME/.local/share/free42";
                else
                    dest = "$XDG_DATA_HOME/free42";
                fprintf(stderr, "$HOME/.free42 is a symlink; not moving it to %s\n", dest);
                strcpy(free42dirname, old_free42dirname);
                goto dir_done;
            }
            have_old = S_ISDIR(st.st_mode);
        }
        if (have_old) {
            // Temporarily remove the "/free42" part from the end of the path,
            // leaving the path of the parent, which we will create
            free42dirname[strlen(free42dirname) - 7] = 0;
        }
        // The Free42 directory does not exist yet. Trying to create it,
        // and all its ancestors. We're not checking for errors here, since
        // either XDG_DATA_HOME or else HOME really should be set to
        // something sane, and besides, trying to create the first few
        // components of this path is *expected* to return errors, because
        // they will already exist.
        char *slash = free42dirname;
        do {
            *slash = '/';
            char *nextSlash = strchr(slash + 1, '/');
            if (nextSlash != NULL)
                *nextSlash = 0;
            mkdir(free42dirname, 0755);
            slash = nextSlash;
        } while (slash != NULL);
        // Now, move the $HOME/.free42 directory, if it exists
        if (have_old) {
            strcat(free42dirname, "/free42");
            if (rename(old_free42dirname, free42dirname) != 0) {
                int err = errno;
                const char *dest;
                if (xdg_data_home == NULL || xdg_data_home[0] == 0)
                    dest = "$HOME/.local/share/free42";
                else
                    dest = "$XDG_DATA_HOME/free42";
                fprintf(stderr, "Unable to move $HOME/.free42 to %s: %s (%d)\n", dest, strerror(err), err);
                strcpy(free42dirname, old_free42dirname);
                goto dir_done;
            }
            // Create a symlink so the old directory will not appear
            // to have just vanished without a trace.
            // If XDG_DATA_HOME is a subdirectory of HOME, make
            // the symlink relative.
            int len = strlen(home);
            if (strncmp(free42dirname, home, len) == 0 && free42dirname[len] == '/')
                symlink(free42dirname + len + 1, old_free42dirname);
            else
                symlink(free42dirname, old_free42dirname);
        }
    }

    dir_done:
    snprintf(statefilename, FILENAMELEN, "%s/state", free42dirname);
    snprintf(printfilename, FILENAMELEN, "%s/print", free42dirname);
    snprintf(keymapfilename, FILENAMELEN, "%s/keymap", free42dirname);

    

    /****************************/
    /***** Read the key map *****/
    /****************************/

    // @FIXME read_key_map(keymapfilename);


    /***********************************************************/
    /***** Open the state file and read the shell settings *****/
    /***********************************************************/

    int4 version;
    int init_mode;
    char core_state_file_name[FILENAMELEN];
    int core_state_file_offset;

    statefile = fopen(statefilename, "r");
    if (statefile != NULL) {
        if (read_shell_state(&version)) {           
            init_mode = 1;
        } else {
            init_shell_state(-1);
            init_mode = 2;
        }
    } else {
        init_shell_state(-1);
        init_mode = 0;
    }
    if (init_mode == 1) {
        if (version > 25) {
            snprintf(core_state_file_name, FILENAMELEN, "%s/%s.f42", free42dirname, state.coreName);
            core_state_file_offset = 0;
        } else {
            strcpy(core_state_file_name, statefilename);
            core_state_file_offset = ftell(statefile);
        }
        fclose(statefile);
    }  else {
        // The shell state was missing or corrupt, but there
        // may still be a valid core state...
        snprintf(core_state_file_name, FILENAMELEN, "%s/%s.f42", free42dirname, state.coreName);
        if (file_exists(core_state_file_name)) {
            // Core state "Untitled.f42" exists; let's try to read it
            core_state_file_offset = 0;
            init_mode = 1;
            version = 26;
        }
    }

    /*********************************/
    /***** Build the main window *****/
    /*********************************/

/*
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "states_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(statesCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "show_printout_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(showPrintOutCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "paper_advance_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(paperAdvanceCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "import_programs_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(importProgramCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "export_programs_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(exportProgramCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "preferences_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(preferencesCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "quit_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(quitCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "copy_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(copyCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "paste_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(pasteCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "copy_printout_as_text_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(copyPrintAsTextCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "copy_printout_as_image_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(copyPrintAsImageCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "clear_printout_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(clearPrintOutCB), NULL);
    item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "about_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(aboutCB), NULL);
    */

    // Master cyvle
    // read input
    // when finished call core_paste(text);
    // Try to print out via char *buf = core_copy();

    std::string line;
    while (std::getline(std::cin, line))
    {
        std::cout << line << std::endl;
    }
    core_paste(line.c_str());
    printf("Program...");
    printf("%s",core_copy());

    /**************************************/
    /***** Build the print-out window *****/
    /**************************************/

    
    /*************************************************/
    /***** Show main window & start the emulator *****/
    /*************************************************/

}


/* shell_annunciators()
 * Callback invoked by the emulator core to change the state of the display
 * annunciators (up/down, shift, print, run, battery, (g)rad).
 * Every parameter can have values 0 (turn off), 1 (turn on), or -1 (leave
 * unchanged).
 * The battery annunciator is missing from the list; this is the only one of
 * the lot that the emulator core does not actually have any control over, and
 * so the shell is expected to handle that one by itself.
 */
void shell_annunciators(int updn, int shf, int prt, int run, int g, int rad){
    // Hum.... for the meantime it will be omitted
}

void shell_get_time_date(uint4 *time, uint4 *date, int *weekday) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tms;
    localtime_r(&tv.tv_sec, &tms);
    if (time != NULL)
        *time = ((tms.tm_hour * 100 + tms.tm_min) * 100 + tms.tm_sec) * 100 + tv.tv_usec / 10000;
    if (date != NULL)
        *date = ((tms.tm_year + 1900) * 100 + tms.tm_mon + 1) * 100 + tms.tm_mday;
    if (weekday != NULL)
        *weekday = tms.tm_wday;
}

void shell_powerdown() {
    exit(0);
}

void shell_message(const char* message){
    fprintf(stdout, message);
}

int8 shell_random_seed() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

uint4 shell_milliseconds() {
    struct timeval tv;               
    gettimeofday(&tv, NULL);
    return (uint4) (tv.tv_sec * 1000L + tv.tv_usec / 1000);
}

int shell_decimal_point() {
    return decimal_point ? 1 : 0;
}

/* Unsupported
*/
void shell_print(const char *text, int length,
                 const char *bits, int bytesperline,
                 int x, int y, int width, int height) {
}

static FILE *logfile = NULL;

void shell_log(const char *message) {
    if (logfile == NULL)
        logfile = fopen("free42.log", "w");
    fprintf(logfile, "%s\n", message);
    fflush(logfile);
}

/** Nver pending events for cmd line */
int shell_wants_cpu() {
    return 0;
}

void shell_delay(int duration) {
}

void shell_request_timeout3(int delay) {
}

void shell_beeper(int frequency, int duration) {
    fprintf(stderr,"BEEP");
}

uint4 shell_get_mem() { 
    FILE *meminfo = fopen("/proc/meminfo", "r");
    char line[1024];
    uint4 bytes = 0;
    if (meminfo == NULL)
        return 0;
    while (fgets(line, 1024, meminfo) != NULL) {
        if (strncmp(line, "MemFree:", 8) == 0) {
            unsigned int kbytes;
            if (sscanf(line + 8, "%u", &kbytes) == 1)
                bytes = 1024 * kbytes;
            break;
        }
    }
    fclose(meminfo);
    return bytes;
}


const char *shell_platform() {
    // return VERSION " " VERSION_PLATFORM;
    return "0 ALPHACMD";
}


int shell_low_battery() {
         
    int lowbat = 0;
    FILE *apm = fopen("/proc/apm", "r");
    if (apm != NULL) {
        /* /proc/apm partial legend:
         * 
         * 1.16 1.2 0x03 0x01 0x03 0x09 9% -1 ?
         *               ^^^^ ^^^^
         *                 |    +-- Battery status (0 = full, 1 = low,
         *                 |                        2 = critical, 3 = charging)
         *                 +------- AC status (0 = offline, 1 = online)
         */
        char line[1024];
        int ac_stat, bat_stat;
        if (fgets(line, 1024, apm) == NULL)
            goto done1;
        if (sscanf(line, "%*s %*s %*s %x %x", &ac_stat, &bat_stat) == 2)
            lowbat = ac_stat != 1 && (bat_stat == 1 || bat_stat == 2);
        done1:
        fclose(apm);
    } else {
        /* Battery considered low if
         *
         *   /sys/class/power_supply/BATn/status == "Discharging"
         *   and
         *   /sys/class/power_supply/BATn/capacity <= 10
         *
         * Assuming status will always be "Discharging" when the system is
         * actually running on battery (it could also be "Full", but then it is
         * definitely not low!), and that capacity is a number between 0 and
         * 100. The choice of 10% or less as being "low" is completely
         * arbitrary.
         * Checking BATn where n = 0, 1, or 2. Some docs suggest BAT0 should
         * exist, others suggest 1 should exist; I'm playing safe and trying
         * both, and throwing in BAT2 just for the fun of it.
         */
        char status_filename[50];
        char capacity_filename[50];
        char line[50];
        for (int n = 0; n <= 2; n++) {
            sprintf(status_filename, "/sys/class/power_supply/BAT%d/status", n);
            FILE *status_file = fopen(status_filename, "r");
            if (status_file == NULL)
                continue;
            sprintf(capacity_filename, "/sys/class/power_supply/BAT%d/capacity", n);
            FILE *capacity_file = fopen(capacity_filename, "r");
            if (capacity_file == NULL) {
                fclose(status_file);
                continue;
            }
            bool discharging = fgets(line, 50, status_file) != NULL && strncasecmp(line, "discharging", 11) == 0;
            int capacity;
            if (fscanf(capacity_file, "%d", &capacity) != 1)
                capacity = 100;
            fclose(status_file);
            fclose(capacity_file);
            lowbat = discharging && capacity <= 10;
            break;
        }
    }

    return lowbat;
}


void shell_blitter(const char *bits, int bytesperline, int x, int y,
                                     int width, int height) 
{
}

static bool file_exists(const char *name) {
    struct stat st;
    return stat(name, &st) == 0;
}

// FIXME SHELL STATE. Retain compatibility with GTK version if possible

static void init_shell_state(int4 version) {
    switch (version) {
        case -1:
            state.extras = 0;
            /* fall through */
        case 0:
            state.printerToTxtFile = 0;
            state.printerToGifFile = 0;
            state.printerTxtFileName[0] = 0;
            state.printerGifFileName[0] = 0;
            state.printerGifMaxLength = 256;
            /* fall through */
        case 1:
            state.mainWindowKnown = 0;
            state.printWindowKnown = 0;
            /* fall through */
        case 2:
            state.skinName[0] = 0;
            /* fall through */
        case 3:
            state.singleInstance = 1;
            /* fall through */
        case 4:
            strcpy(state.coreName, "Untitled");
            /* fall through */
        case 5:
            core_settings.matrix_singularmatrix = false;
            core_settings.matrix_outofrange = false;
            core_settings.auto_repeat = true;
            /* fall through */
        case 6:
            state.old_repaint = true;
            /* fall through */
        case 7:
            /* current version (SHELL_VERSION = 7),
             * so nothing to do here since everything
             * was initialized from the state file.
             */
            ;
    }
}

static int read_shell_state(int4 *ver) {
    int4 magic;
    int4 version;
    int4 state_size;
    int4 state_version;

    if (fread(&magic, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (magic != FREE42_MAGIC)
        return 0;

    if (fread(&version, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (version == 0) {
        /* State file version 0 does not contain shell state,
         * only core state, so we just hard-init the shell.
         */
        init_shell_state(-1);
        *ver = version;
        return 1;
    }
    
    if (fread(&state_size, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (fread(&state_version, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (state_version < 0 || state_version > SHELL_VERSION)
        /* Unknown shell state version */
        return 0;
    if (fread(&state, 1, state_size, statefile) != (size_t) state_size)
        return 0;
    if (state_version >= 6) {
        core_settings.matrix_singularmatrix = state.matrix_singularmatrix;
        core_settings.matrix_outofrange = state.matrix_outofrange;
        core_settings.auto_repeat = state.auto_repeat;
    }

    init_shell_state(state_version);
    *ver = version;
    return 1;
}

static int write_shell_state() {
    int4 magic = FREE42_MAGIC;
    int4 version = 27;
    int4 state_size = sizeof(state_type);
    int4 state_version = SHELL_VERSION;

    if (fwrite(&magic, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (fwrite(&version, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (fwrite(&state_size, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    if (fwrite(&state_version, 1, sizeof(int4), statefile) != sizeof(int4))
        return 0;
    state.matrix_singularmatrix = core_settings.matrix_singularmatrix;
    state.matrix_outofrange = core_settings.matrix_outofrange;
    state.auto_repeat = core_settings.auto_repeat;
    if (fwrite(&state, 1, sizeof(state_type), statefile) != sizeof(int4))
        return 0;

    return 1;
}


int main(int argc, char *argv[]) {
    fprintf(stderr,"SETUP....");
    shell_log("Free42CMD_START");
    // Mmmm call a gtk-simil activate method to run the REPL loop somewhat
    activate();
}

