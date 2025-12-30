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
// static struct event_base *base;
// static char *line_buffer;

// User defined callbacks
static net_on_connect netcb_on_connect = NULL;
static net_on_read netcb_on_read = NULL;
static net_on_close netcb_on_close = NULL;

typedef struct NetThreadArguments {
	int port;
	int id;
	uv_loop_t* loop;
	uv_tcp_t* server;
} NetThreadArguments;

// Helper functions

static inline int fast_str_startswith(const char* prefix, const char* str) {
	char cp, cs;
	while ((cp = *prefix++) == (cs = *str++)) {
		if (cp == 0) return 1;
	}
	return !cp;
}

// Decimal string to unsigned int. This variant does NOT consume +, - or whitespace.
// If **endptr is not NULL, it will point to the first non-decimal character, which
// may be \0 at the end of the string.
static inline uint32_t fast_strtoul10(const char* str, const char** endptr) {
	uint32_t result = 0;
	unsigned char c;
	for (; (c = *str - '0') <= 9; str++) result = result * 10 + c;
	if (endptr) *endptr = str;
	return result;
}

// Same as fast_strtoul10, but for hex strings.
static inline uint32_t fast_strtoul16(const char* str, const char** endptr) {
	uint32_t result = 0;
	unsigned char c;
	while ((c = *str - '0') <= 9							 // 0-9
				 || ((c -= 7) >= 10 && c <= 15)			 // A-F
				 || ((c -= 32) >= 10 && c <= 15)) {	 // a-f
		result = result * 16 + c;
		str++;
	}
	if (endptr) *endptr = str;
	return result;
}

// libevent callbacks

uv_buf_t uv_buf_from_str(const char* str) {
	uv_buf_t buf;
	buf.len = strlen(str);
	buf.base = str;
	return buf;
}

uv_buf_t uv_buf_from_str_with_length(const char* str, int length) {
	uv_buf_t buf;
	buf.len = length;
	buf.base = str;
	return buf;
}

// Public functions

// void net_stop() { uv_loop_close(loop); }

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	buf->base = malloc(suggested_size);
	buf->len = suggested_size;
}

void handle_size_command(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	uv_write_t* req = (uv_write_t*)malloc(sizeof(uv_write_t));

	printf("Handling SIZE command\n");

	// TODO: actually read canvas size
	unsigned int width, height;
	canvas_get_size(&width, &height);
	char str[64] = {0};
	snprintf(str, 64, "SIZE %d %d\n", width, height);
	uv_buf_t res = uv_buf_from_str_with_length((const char*)&str, 64);

	// only need a single write
	int r = uv_write(req, stream, &res, 1, NULL);
	if (r != 0) {
		printf("Error during SIZE command %d\n", r);
	}
}

void handle_stats_command(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	uv_write_t* req = (uv_write_t*)malloc(sizeof(uv_write_t));

	printf("Handling SIZE command\n");

	// TODO: actually read stats
	char str[64] = {0};
	snprintf(str, 64, "STATS px:%u conn:%u\n", 0, 0);
	uv_buf_t res = uv_buf_from_str_with_length((const char*)&str, 64);

	// only need a single write
	int r = uv_write(req, stream, &res, 1, NULL);
	if (r != 0) {
		printf("Error during STATS command %d\n", r);
	}
}

void handle_help_command(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	uv_write_t* req = (uv_write_t*)malloc(sizeof(uv_write_t));

	printf("Handling HELP command\n");

	char txt[] =
			"PX x y: Get color at position (x,y)\nPX x y rrggbb(aa): Draw a pixel (with "
			"optional alpha channel)\nSIZE: Get canvas size\nSTATS: Return statistics\n";
	uv_buf_t res = uv_buf_from_str((const char*)&txt);
	int r = uv_write(req, stream, &res, 1, NULL);
	if (r != 0) {
		printf("Error during HELP command %d\n", r);
	}
}

void handle_reset_command(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	printf("Handling RESET command\n");
	canvas_fill(0x000000ff);
}

/**
 * What should the flow of handing commands be?
 *
 * 1. If the first command is SIZE, HELP or STATS, we just reply directly and ignore the remaining
 * command buffer
 *
 * For PX getting and setting, we want to be able to answer multiple commands together, so:
 *
 * 2. If the first command is get PX, we parse the command bufferuntil we encounter something else,
 * stop parsing, prepare the output buffer(s) and send those out. We can then return control flow
 * and let the command parsing restart.
 *
 * 3. If the first command is set PX, we can do the same as in #2, but we don't need to write
 * anything back.
 *
 * 4. Error Handling: If nothing matches we just ignore the remaining buffer for now. In the future
 * we can respond with an actual parser error message.
 */
void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	if (nread < 0) {
		if (nread != UV_EOF) {
			// Some error that I don't care about atm
		}

		// Always close the stream
		uv_close((uv_handle_t*)stream, NULL);
		return;
	}

	printf("received %d bytes\n", nread);

	char* start = buf->base;
	char* end = &buf->base[nread - 1];

	// TODO: only keep parsing when the initial bytes looks like PX commands
	// We can actually send multiple buffers
	// https://ant.readthedocs.io/en/latest/stream.html#c.uv_write
	while (start != end) {
		while (*start == 10 || *start == 13 || *start == 0) {
			start++;
		}

		// printf("First char is %c/%d\n", *start, *start);

		// printf("buffer: %s", start);
		if (fast_str_startswith("PX ", start)) {
			printf("Handling PX command\n");

			uv_write_t* req = (uv_write_t*)malloc(sizeof(uv_write_t));

			const char* ptr = start + 3;
			const char* endptr = ptr;
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

			endptr++;	 // eat space (or whatever non-decimal is found here)

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
				// TODO: build buffer instead of writing all the time
				uv_buf_t res = uv_buf_from_str_with_length((const char*)&str, 64);
				int r = uv_write(req, stream, &res, 1, NULL);
				if (r != 0) {
					printf("Error during PX command %d\n", r);
				}
				return;
			}

			endptr++;	 // eat space (or whatever non-decimal is found here)

			// PX <x> <y> BB|RRGGBB|RRGGBBAA
			uint32_t c = fast_strtoul16((ptr = endptr),
																	&endptr);	 // advances endptr until the last non-hex character
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

			printf("Set pixel %d %d to 0x%08X \n", x, y, c);

			// px_pixelcount++;
			canvas_set_px(x, y, c);

			start = endptr;
		} else if (fast_str_startswith("SIZE", start)) {
			handle_size_command(stream, nread, buf);
			break;
		} else if (fast_str_startswith("STATS", start)) {
			handle_stats_command(stream, nread, buf);
			break;
		} else if (fast_str_startswith("HELP", start)) {
			handle_help_command(stream, nread, buf);
			break;
		} else if (fast_str_startswith("RESET", start)) {
			handle_reset_command(stream, nread, buf);
			break;
		} else {
			// error
			puts("Cant parse whatever it is");
			// TODO: return an error message
			break;
		}
	}

	free(buf->base);
	puts("Finished reading socket");
}

void on_connection(uv_stream_t* server, int status) {
	uv_tcp_t* client = malloc(sizeof(uv_tcp_t));
	// NetThreadArguments *ctx = (NetThreadArguments *)server->data;

	// printf("new connection on thread %d\n", ctx->id);

	if (status == -1) {
		/* error */
	}

	// uv_tcp_init(loop, client);
	uv_tcp_init(server->loop, client);

	if (uv_accept(server, (uv_stream_t*)client) == 0) {
		int r = uv_read_start((uv_stream_t*)client, alloc_buffer, on_read);

		if (r) {
			/* error */
		}
	} else {
		uv_close((uv_handle_t*)client, NULL);
	}
}

int start_uv_server(void* arg) {
	// We assume we are running on our own thread at this opoint.
	NetThreadArguments* ctx = (NetThreadArguments*)arg;
	uv_loop_t* loop = uv_default_loop();
	// ctx->loop = loop;

	uv_tcp_t server;

	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", ctx->port, &addr);

	uv_tcp_init(loop, &server);
	printf("Initiated loop on thread %d on port %d\n", ctx->id, ctx->port);

	ctx->server = &server;
	ctx->server->data = ctx;

// uv_tcp_bind(&server, (const struct sockaddr *)&addr, UV_TCP_REUSEPORT);
// libuv does not support UV_TCP_REUSEPORT under macOS
#ifdef __APPLE__
	uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
#else
	uv_tcp_bind(&server, (const struct sockaddr*)&addr, UV_TCP_REUSEPORT);
#endif

	int r = uv_listen((uv_stream_t*)&server, 128, on_connection);

	/* error */
	if (r != 0) {
		return r;
	}

	return uv_run(loop, UV_RUN_DEFAULT);
}

void start_event_loops(int loop_count, int port) {
	pthread_t net_thread[loop_count];
	uv_tcp_t server[loop_count];
	uv_loop_t* loop[loop_count];

	for (int i = 0; i < loop_count; i++) {
		NetThreadArguments* args = (NetThreadArguments*)malloc(sizeof(NetThreadArguments));
		args->port = port;
		args->id = i;
		args->loop = loop[i];
		args->server = &server[i];

		printf("Creating thread with id %d\n", i);
		if (pthread_create(&net_thread[i], NULL, (void*)start_uv_server, args)) {
			printf("Failed to start net thread with id %d\n", i);
			exit(1);
		}
	}
}
