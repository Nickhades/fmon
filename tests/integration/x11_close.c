#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window w = (Window)strtoul(argv[1], 0, 0);
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)del;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(d, w, False, NoEventMask, &ev);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}