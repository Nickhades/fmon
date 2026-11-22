#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    if (argc > 1) {
        Window w = (Window)strtoul(argv[1], 0, 0);
        XSetInputFocus(d, w, RevertToParent, CurrentTime);
        XFlush(d);
    }
    KeyCode k = XKeysymToKeycode(d, XK_Escape);
    XTestFakeKeyEvent(d, k, True, 0);
    XTestFakeKeyEvent(d, k, False, 0);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}