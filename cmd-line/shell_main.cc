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

static bool decimal_point;
char free42dirname[FILENAMELEN];

static FILE *statefile = NULL;
static char statefilename[FILENAMELEN];
static char printfilename[FILENAMELEN];

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

    read_key_map(keymapfilename);


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
            if (skin_arg != NULL) {
                strncpy(state.skinName, skin_arg, FILENAMELEN - 1);
                state.skinName[FILENAMELEN - 1] = 0;
            }
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

    char *xml = (char *) malloc(10240);
    if (use_compactmenu)
        sprintf(xml, mainWindowXml, compactMenuIntroXml, compactMenuOutroXml);
    else
        sprintf(xml, mainWindowXml, "", "");
    GtkBuilder *builder = gtk_builder_new();
    gtk_builder_add_from_string(builder, xml, -1, NULL);
    free(xml);
    GObject *obj = gtk_builder_get_object(builder, "window");
    mainwindow = GTK_WIDGET(obj);
    gtk_window_set_application(GTK_WINDOW(mainwindow), app);

    icon_128 = gdk_pixbuf_new_from_xpm_data((const char **) icon_128_xpm);
    icon_48 = gdk_pixbuf_new_from_xpm_data((const char **) icon_48_xpm);

    gtk_window_set_icon(GTK_WINDOW(mainwindow), icon_128);
    gtk_window_set_title(GTK_WINDOW(mainwindow), TITLE);
    gtk_window_set_role(GTK_WINDOW(mainwindow), "Free42 Calculator");
    gtk_window_set_resizable(GTK_WINDOW(mainwindow), FALSE);
    no_mwm_resize_borders(mainwindow);
    g_signal_connect(G_OBJECT(mainwindow), "delete_event",
                     G_CALLBACK(delete_cb), NULL);
    if (state.mainWindowKnown)
        gtk_window_move(GTK_WINDOW(mainwindow), state.mainWindowX,
                                            state.mainWindowY);

    // The "Skin" menu is dynamic; we don't populate any items in it here.
    // Instead, we attach a callback which scans the .free42 directory for
    // available skins; this callback is invoked when the menu is about to
    // be mapped.
    GtkMenuItem *item = GTK_MENU_ITEM(gtk_builder_get_object(builder, "skin_item"));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(skin_menu_update), NULL);

    // With GTK 2 and GTK 3.4, the above logic worked fine, but with 3.24,
    // it appears that the pop-up shell is laid out *before* the 'activate'
    // callback is invoked. The result is that you do end up with the correct
    // menu items, but they don't fit in the pop-up and so are cut off.
    // Can't think of a proper way around this, but this at least will fix the
    // Skin menu appearance in the most common use case, i.e. when the set of
    // skins does not change while Free42 is running.
    skin_menu_update(GTK_WIDGET(item));

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


    /****************************************/
    /* Drawing area for the calculator skin */
    /****************************************/

    GtkWidget *box = GTK_WIDGET(gtk_builder_get_object(builder, "box"));

    int win_width, win_height;
    skin_load(&win_width, &win_height);
    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_size_request(w, win_width, win_height);
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(draw_cb), NULL);
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(button_cb), NULL);
    g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(button_cb), NULL);
    gtk_widget_set_can_focus(w, TRUE);
    g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(key_cb), NULL);
    g_signal_connect(G_OBJECT(w), "key-release-event", G_CALLBACK(key_cb), NULL);
    calc_widget = w;


    /**************************************/
    /***** Build the print-out window *****/
    /**************************************/

    // In the Motif version, I create an XImage and read the bitmap data into
    // it; in the GTK version, that approach is not practical, since pixbuf
    // only comes in 24-bit and 32-bit flavors -- which would mean wasting
    // 25 megabytes for a 286x32768 pixbuf. So, instead, I use a 1 bpp buffer,
    // and simply create pixbufs on the fly whenever I have to repaint.
    print_bitmap = (unsigned char *) malloc(PRINT_SIZE);
    print_text = (unsigned char *) malloc(PRINT_TEXT_SIZE);
    // TODO - handle memory allocation failure

    FILE *printfile = fopen(printfilename, "r");
    if (printfile != NULL) {
        int n = fread(&printout_bottom, 1, sizeof(int), printfile);
        if (n == sizeof(int)) {
            if (printout_bottom > PRINT_LINES) {
                int excess = (printout_bottom - PRINT_LINES) * PRINT_BYTESPERLINE;
                fseek(printfile, excess, SEEK_CUR);
                printout_bottom = PRINT_LINES;
            }
            int bytes = printout_bottom * PRINT_BYTESPERLINE;
            n = fread(print_bitmap, 1, bytes, printfile);
            if (n == bytes) {
                n = fread(&print_text_bottom, 1, sizeof(int), printfile);
                int n2 = fread(&print_text_pixel_height, 1, sizeof(int), printfile);
                if (n == sizeof(int) && n2 == sizeof(int)) {
                    n = fread(print_text, 1, print_text_bottom, printfile);
                    if (n != print_text_bottom) {
                        print_text_bottom = 0;
                        print_text_pixel_height = 0;
                    }
                } else {
                    print_text_bottom = 0;
                    print_text_pixel_height = 0;
                }
            } else {
                printout_bottom = 0;
                print_text_bottom = 0;
                print_text_pixel_height = 0;
            }
        } else {
            printout_bottom = 0;
            print_text_bottom = 0;
            print_text_pixel_height = 0;
        }
        fclose(printfile);
    } else {
        printout_bottom = 0;
        print_text_bottom = 0;
        print_text_pixel_height = 0;
    }
    printout_top = 0;
    print_text_top = 0;

    printwindow = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_icon(GTK_WINDOW(printwindow), icon_128);
    gtk_window_set_title(GTK_WINDOW(printwindow), "Free42 Print-Out");
    gtk_window_set_role(GTK_WINDOW(printwindow), "Free42 Print-Out");
    g_signal_connect(G_OBJECT(printwindow), "delete_event",
                     G_CALLBACK(delete_print_cb), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_container_add(GTK_CONTAINER(printwindow), scroll);
    GtkWidget *view = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    print_widget = gtk_drawing_area_new();
    gtk_widget_set_size_request(print_widget, 358, printout_bottom);
    gtk_container_add(GTK_CONTAINER(view), print_widget);
    print_adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect(G_OBJECT(print_widget), "draw", G_CALLBACK(print_draw_cb), NULL);
    gtk_widget_set_can_focus(print_widget, TRUE);
    g_signal_connect(G_OBJECT(print_widget), "key-press-event", G_CALLBACK(print_key_cb), NULL);

    gtk_widget_show(print_widget);
    gtk_widget_show(view);
    gtk_widget_show(scroll);

    GdkGeometry geom;
    geom.min_width = 358;
    geom.max_width = 358;
    geom.min_height = 1;
    geom.max_height = 32767;
    gtk_window_set_geometry_hints(GTK_WINDOW(printwindow), NULL, &geom, GdkWindowHints(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));

    if (state.printWindowKnown)
        gtk_window_move(GTK_WINDOW(printwindow), state.printWindowX,
                                                 state.printWindowY);
    gint width, height;
    gtk_window_get_size(GTK_WINDOW(printwindow), &width, &height);
    gtk_window_resize(GTK_WINDOW(printwindow), width,
            state.printWindowKnown ? state.printWindowHeight : 600);

    gtk_widget_realize(printwindow);
    gtk_widget_realize(print_widget);
    scroll_printout_to_bottom();


    /*************************************************/
    /***** Show main window & start the emulator *****/
    /*************************************************/

    if (state.printWindowKnown && state.printWindowMapped)
        gtk_widget_show(printwindow);
    gtk_widget_show_all(mainwindow);
    gtk_widget_show(mainwindow);

    core_init(init_mode, version, core_state_file_name, core_state_file_offset);
    if (core_powercycle())
        enable_reminder();

    /* Check if /proc/apm exists and is readable, and if so,
     * start the battery checker "thread" that keeps the battery
     * annunciator on the calculator display up to date.
     */
    FILE *apm = fopen("/proc/apm", "r");
    if (apm != NULL) {
        fclose(apm);
        shell_low_battery();
        g_timeout_add(60000, battery_checker, NULL);
    } else {
        /* Check if /sys/class/power_supply exists */
        DIR *d = opendir("/sys/class/power_supply");
        if (d != NULL) {
            closedir(d);
            shell_low_battery();
            g_timeout_add(60000, battery_checker, NULL);
        }
    }

    if (pipe(pype) != 0)
        fprintf(stderr, "Could not create pipe for signal handler; not catching signals.\n");
    else {
        GIOChannel *channel = g_io_channel_unix_new(pype[0]);
        GError *err = NULL;
        g_io_channel_set_encoding(channel, NULL, &err);
        g_io_channel_set_flags(channel,     
            (GIOFlags) (g_io_channel_get_flags(channel) | G_IO_FLAG_NONBLOCK), &err);
        g_io_add_watch(channel, G_IO_IN, gt_signal_handler, NULL);

        struct sigaction act;
        act.sa_handler = int_term_handler;
        sigemptyset(&act.sa_mask);
        sigaddset(&act.sa_mask, SIGINT);
        sigaddset(&act.sa_mask, SIGTERM);
        act.sa_flags = 0;
        sigaction(SIGINT, &act, NULL);
        sigaction(SIGTERM, &act, NULL);
    }
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


int main(int argc, char *argv[]) {
    fprintf(stderr,"SETUP....");
    shell_log("Free42CMD_START");
    // Mmmm call a gtk-simil activate method to run the REPL loop somewhat
    activate();
}
