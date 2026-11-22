#include <X11/Xlib.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window w = (Window)strtoul(argv[1], 0, 0);
    XKillClient(d, w);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}