#include "net.h"

#include <assert.h>
#include <err.h>
#include <errno.h>

#include "canvas.h"
// #include <event2/buffer.h>
// #include <event2/bufferevent.h>
// #include <event2/event.h>
// #include <event2/thread.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

// Lines longer than this are considered an error.
#define NET_MAX_LINE 1024

// The server buffers up to NET_READ_BUFFER bytes per client connection.
// Lower values allow lots of clients to draw at the same time, each with a fair share.
// Higher values increase throughput but fast clients might be able to draw large batches at once.
#define NET_MAX_BUFFER 10240

#define NET_CSTATE_OPEN 0
#define NET_CSTATE_CLOSING 1

static inline int min(int a, int b) { return a < b ? a : b; }

// global state
static struct event_base *base;
static char *line_buffer;

// User defined callbacks
static net_on_connect netcb_on_connect = NULL;
static net_on_read netcb_on_read = NULL;
static net_on_close netcb_on_close = NULL;

pthread_t net_thread;
/** UV rewrite */
uv_tcp_t server;
uv_loop_t *loop;

typedef struct NetThreadArguments {
	int port;
	int id;
} NetThreadArguments;

// Helper functions

static inline int fast_str_startswith(const char *prefix, const char *str) {
	char cp, cs;
	while ((cp = *prefix++) == (cs = *str++)) {
		if (cp == 0) return 1;
	}
	return !cp;
}

// Decimal string to unsigned int. This variant does NOT consume +, - or whitespace.
// If **endptr is not NULL, it will point to the first non-decimal character, which
// may be \0 at the end of the string.
static inline uint32_t fast_strtoul10(const char *str, const char **endptr) {
	uint32_t result = 0;
	unsigned char c;
	for (; (c = *str - '0') <= 9; str++) result = result * 10 + c;
	if (endptr) *endptr = str;
	return result;
}

// Same as fast_strtoul10, but for hex strings.
static inline uint32_t fast_strtoul16(const char *str, const char **endptr) {
	uint32_t result = 0;
	unsigned char c;
	while ((c = *str - '0') <= 9			   // 0-9
		   || ((c -= 7) >= 10 && c <= 15)	   // A-F
		   || ((c -= 32) >= 10 && c <= 15)) {  // a-f
		result = result * 16 + c;
		str++;
	}
	if (endptr) *endptr = str;
	return result;
}

// libevent callbacks

uv_buf_t uv_buf_from_str(const char *str) {
	uv_buf_t buf;
	buf.len = strlen(str);
	buf.base = str;
	return buf;
}

uv_buf_t uv_buf_from_str_with_length(const char *str, int length) {
	uv_buf_t buf;
	buf.len = length;
	buf.base = str;
	return buf;
}

// Public functions

void net_stop() { uv_loop_close(loop); }

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
	buf->base = malloc(suggested_size);
	buf->len = suggested_size;
}

void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
	uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));

	if (nread == -1) {
		/* if (uv_last_error(loop).code != UV_EOF) { */
		/* } */

		uv_close((uv_handle_t *)stream, NULL);
		return;
	}

	// printf("received %x bytes\n", nread);

	char *start = buf->base;
	char *last_potential_byte = buf->base + nread - 1;

	while (start < last_potential_byte) {
		if (*start == 10) {
			start++;
		}

		// printf("buffer: %s", start);
		if (fast_str_startswith("PX ", start)) {
			// puts("PX COMMAND");
			const char *ptr = start + 3;
			const char *endptr = ptr;
			errno = 0;

			uint32_t x = fast_strtoul10(ptr, &endptr);
			if (endptr == ptr) {
				// net_err(client, "Invalid command (expected decimal as first parameter)");
				return;
			}
			if (*endptr == '\0') {
				// net_err(client, "Invalid command (second parameter required)");
				return;
			}

			endptr++;  // eat space (or whatever non-decimal is found here)

			uint32_t y = fast_strtoul10((ptr = endptr), &endptr);
			if (endptr == ptr) {
				// net_err(client, "Invalid command (expected decimal as second parameter)");
				return;
			}

			// PX <x> <y> -> Get RGB color at position (x,y) or '0x000000' for out-of-range queries
			// printf("endptr: %d, ptr: %d\n", *endptr, *ptr);
			if (*endptr == '\0' || *endptr == '\n' || *endptr == 13) {
				uint32_t c = 0x00000000;
				canvas_get_px(x, y, &c);
				char str[64] = {0};
				snprintf(str, 64, "PX %u %u %06X\n", x, y, (c >> 8));
				uv_buf_t res = uv_buf_from_str_with_length(&str, 64);
				int r = uv_write(req, stream, &res, 1, NULL);
				if (r != 0) {
					printf("Error during PX command %d\n", r);
				}
				return;
			}

			endptr++;  // eat space (or whatever non-decimal is found here)

			// PX <x> <y> BB|RRGGBB|RRGGBBAA
			uint32_t c = fast_strtoul16((ptr = endptr), &endptr);
			if (endptr == ptr) {
				puts("Third parameter missing or invalid (should be hex color)");
				return;
			}

			if (endptr - ptr == 6) {
				// RGB -> RGBA (most common)
				c = (c << 8) + 0xff;
			} else if (endptr - ptr == 8) {
				// done
			} else if (endptr - ptr == 2) {
				// WW -> RGBA
				c = (c << 24) + (c << 16) + (c << 8) + 0xff;
			} else {
				puts("Color hex code must be 2, 6 or 8 characters long (WW, RGB or RGBA)");
				return;
			}

			// printf("Set pixel %d %d to 0x%08X \n", x, y, c);

			// px_pixelcount++;
			canvas_set_px(x, y, c);

			start = endptr;
		} else if (fast_str_startswith("SIZE", start)) {
			puts("SIZE COMMAND");
			unsigned int width, height;
			canvas_get_size(&width, &height);
			char str[64] = {0};
			snprintf(str, 64, "SIZE %d %d\n", width, height);
			uv_buf_t res = uv_buf_from_str_with_length(&str, 64);
			int r = uv_write(req, stream, &res, 1, NULL);
			if (r != 0) {
				printf("Error during SIZE command %d\n", r);
			}
			start += 3;
		} else if (fast_str_startswith("STATS", start)) {
			puts("STATS COMMAND");
			char str[64] = {0};
			snprintf(str, 64, "STATS px:%u conn:%u\n", 0, 0);
			uv_buf_t res = uv_buf_from_str_with_length(&str, 64);
			int r = uv_write(req, stream, &res, 1, NULL);
			if (r != 0) {
				printf("Error during STATS command %d\n", r);
			}
			start += 4;

		} else if (fast_str_startswith("HELP", start)) {
			char txt[] =
				"PX x y: Get color at position (x,y)\nPX x y rrggbb(aa): Draw a pixel (with "
				"optional alpha channel)\nSIZE: Get canvas size\nSTATS: Return statistics\n";
			uv_buf_t res = uv_buf_from_str(&txt);
			int r = uv_write(req, stream, &res, 1, NULL);
			if (r != 0) {
				printf("Error during HELP command %d\n", r);
			}
			start += 3;

		} else {
			// error
		}

		start++;
		int valid_bytes_left = nread - (start - buf->base);
	}
	free(buf->base);
}

void on_connection(uv_stream_t *server, int status) {
	uv_tcp_t *client = malloc(sizeof(uv_tcp_t));

	if (status == -1) {
		/* error */
	}

	// uv_tcp_init(loop, client);
	uv_tcp_init(server->loop, client);

	if (uv_accept(server, (uv_stream_t *)client) == 0) {
		int r = uv_read_start((uv_stream_t *)client, alloc_buffer, on_read);

		if (r) {
			/* error */
		}
	} else {
		uv_close((uv_handle_t *)client, NULL);
	}
}

int start_uv_server(void *arg) {
	NetThreadArguments *args = (NetThreadArguments *)arg;
	loop = uv_default_loop();

	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", args->port, &addr);

	uv_tcp_init(loop, &server);
	// uv_tcp_bind(&server, (const struct sockaddr *)&addr, UV_TCP_REUSEPORT);
	// libuv does not support UV_TCP_REUSEPORT under macOS
	uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);

	int r = uv_listen((uv_stream_t *)&server, 128, on_connection);

	if (r) {
		/* error */
	}

	return uv_run(loop, UV_RUN_DEFAULT);
}

void net_start_secondary_thread(int port, int id) {
	NetThreadArguments *args = (NetThreadArguments *)malloc(sizeof(NetThreadArguments));
	args->port = port;
	args->id = id;

	if (pthread_create(&net_thread, NULL, (void *)start_uv_server, args)) {
		puts("Failed to start render thread");
		exit(1);
	}
}