#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    int x = atoi(argv[1]), y = atoi(argv[2]);
    XTestFakeMotionEvent(d, -1, x, y, 0);
    XTestFakeButtonEvent(d, 1, True, 0);
    XTestFakeButtonEvent(d, 1, False, 0);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}