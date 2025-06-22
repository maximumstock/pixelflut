#include <errno.h>
#include <stdio.h>	//sprintf
#include <stdlib.h>

#include "canvas.h"
#include "net.h"

unsigned int px_width = 1024;
unsigned int px_height = 1024;
unsigned int px_pixelcount = 0;
unsigned int px_clientcount = 0;

void px_on_key(int key, int scancode, int mods) {
	printf("Key pressed: key:%d scancode:%d mods:%d\n", key, scancode, mods);

	if (key == 300) {  // F11
		int display = canvas_get_display();
		if (display < 0)
			canvas_fullscreen(0);
		else
			canvas_fullscreen(-1);
	} else if (key == 301) {  // F12
		canvas_fullscreen(canvas_get_display() + 1);
	} else if (key == 67) {	 // c
		canvas_fill(0x00000088);
	} else if (key == 81 || key == 256) {  // q or ESC
		canvas_close();
	}
}

void px_on_resize() { canvas_get_size(&px_width, &px_height); }

void px_on_window_close() {
	printf("Window closed\n");
	// net_stop();
}

int main(int argc, char **argv) {
	// canvas_setcb_key(&px_on_key);
	canvas_setcb_resize(&px_on_resize);

	// net_start_secondary_thread(1337, &px_on_connect, &px_on_read, &px_on_close);
	// net_start_secondary_thread(1337, 0);
	int loop_count = 4;	 // TODO SO_REUSEPORT

#ifdef __APPLE__
	loop_count = 1;
#endif

	start_event_loops(loop_count, 1337);

	// The OpenGL implementation in macOS' Cocoa only receives window and input events
	// and only allows most window and input actions to be executed on the main thread instead
	// of any thread. Therefore, to create a window and setup the input event handlers,
	// we move the canvas rendering logic onto the main thread and the network code
	// runs in a separately spawned stack.
	// See https://discourse.glfw.org/t/multithreading-glfw/573/4
	canvas_start(1024, &px_on_window_close);

	return 0;
}
