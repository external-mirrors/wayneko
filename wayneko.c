#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pixman.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#ifdef __linux__
#include <features.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#endif

#define MIN(A, B) (A < B ? A : B)

#include "wlr-layer-shell-unstable-v1.h"

const char usage[] =
	"Usage: wayneko [options...]\n"
	"  -h, --help\n"
	"  --background-colour 0xRRGGBB[AA]\n"
	"  --outline-colour    0xRRGGBB[AA]\n"
	"  --type              neko|inu|random\n"
	"\n";

pixman_color_t bg_colour;
pixman_color_t border_colour;

/* Note: Atlas width must be divisable by 4. */
#include "neko-bitmap.xbm"
const int neko_bitmap_stride = neko_bitmap_width / 8;
const uint8_t neko_size = 32;
pixman_image_t *neko_atlas = NULL;
pixman_image_t *neko_atlas_bg_fill = NULL;
pixman_image_t *neko_atlas_border_fill = NULL;

enum Type
{
	NEKO = 0,
	INU = 2,
};

enum Neko
{
	NEKO_SLEEP_1 = 0,
	NEKO_SLEEP_2,
	NEKO_YAWN,
	NEKO_SHOCK,
	NEKO_THINK,
	NEKO_STARE,
	NEKO_SCRATCH_1,
	NEKO_SCRATCH_2,
	NEKO_RUN_RIGHT_1,
	NEKO_RUN_RIGHT_2,
	NEKO_RUN_LEFT_1,
	NEKO_RUN_LEFT_2,
};

const uint16_t animation_timeout = 200;
size_t animation_ticks_until_next_frame = 10;
enum Neko current_neko = NEKO_STARE;
enum Type type = NEKO;
bool follow_pointer = true;

struct Seat
{
	struct wl_list link;
	struct wl_seat *wl_seat;
	struct wl_pointer *wl_pointer;
	uint32_t global_name;
	bool on_surface;
	uint32_t surface_x;
};

struct Buffer
{
	struct wl_list link;

	uint32_t width;
	uint32_t height;
	uint32_t stride;
	size_t size;
	void *mmap;
	struct wl_buffer *wl_buffer;
	pixman_image_t *pixman_image;
	bool busy;
};

struct Surface
{
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	uint16_t neko_x, prev_neko_x;
	bool configured;
};

struct Surface surface = { 0 };

const uint32_t desired_surface_height = neko_size;
const uint8_t neko_x_advance = 15;

int ret = EXIT_SUCCESS;
bool loop = true;
struct wl_display *wl_display = NULL;
struct wl_registry *wl_registry = NULL;
struct wl_callback *sync_callback = NULL;
struct wl_compositor *wl_compositor = NULL;
struct wl_shm *wl_shm = NULL;
struct zwlr_layer_shell_v1 *layer_shell = NULL;
struct timespec last_tick;

struct wl_list seats;

/* The amount of buffers per surface we consider the reasonable upper limit.
 * Some compositors sometimes tripple-buffer, so three seems to be ok.
 * Note that we can absolutely work with higher buffer numbers if needed,
 * however we consider that to be an anomaly and therefore do not want to
 * keep all those extra buffers around if we can avoid it, as to not have
 * unecessary memory overhead.
 */
const int max_buffer_multiplicity = 3;
const int surface_amount = 1;

struct wl_list buffer_pool;

/* No-Op function plugged into Wayland listeners we don't care about. */
static void noop () {}

/*************
 *           *
 *  Signals  *
 *           *
 *************/
/**
 * Intercept error signals (like SIGSEGV and SIGFPE) so that we can try to
 * print a fancy error message and a backtracke before letting the system kill us.
 */
static void handle_error (int signum)
{
	const char *msg =
		"\n"
		"🐈 🐈 🐈\n"
		"┌──────────────────────────────────────────┐\n"
		"│                                          │\n"
		"│         wayneko has crashed.             │\n"
		"│                                          │\n"
		"│    This is likely a bug, so please       │\n"
		"│    report this to the mailing list.      │\n"
		"│                                          │\n"
		"│  ~leon_plickat/public-inbox@lists.sr.ht  │\n"
		"│                                          │\n"
		"└──────────────────────────────────────────┘\n"
		"\n";
	fputs(msg, stderr);

	/* Set up the default handlers to deal with the rest. We do this before
	 * attempting to get a backtrace, because sometimes that could also
	 * cause a SEGFAULT and we don't want a funny signal loop to happen.
	 */
	signal(signum, SIG_DFL);

#ifdef __linux__
#ifdef __GLIBC__
	fputs("Attempting to get backtrace:\n", stderr);

	void *buffer[255];
	const int calls = backtrace(buffer, sizeof(buffer) / sizeof(void *));
	backtrace_symbols_fd(buffer, calls, fileno(stderr));
	fputs("\n", stderr);
#endif
#endif

	/* Easiest way of calling the default signal handler. */
	kill(getpid(), signum);
}

/**
 * Intercept soft kills (like SIGINT and SIGTERM) so we can attempt to clean up
 * and exit gracefully.
 */
static void handle_term (int signum)
{
	fputs("[wayneko] Terminated by signal.\n", stderr);

	/* If cleanup fails or hangs and causes this signal to be recieved again,
	 * let the default signal handler kill us.
	 */
	signal(signum, SIG_DFL);

	loop = false;
}

/**
 * Set up signal handlers.
 */
static void init_signals (void)
{
	signal(SIGSEGV, handle_error);
	signal(SIGFPE, handle_error);

	signal(SIGINT, handle_term);
	signal(SIGTERM, handle_term);
}

/************
 *          *
 *  Buffer  *
 *          *
 ************/
static void buffer_randomize_string (char *str, size_t len)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;

	for (size_t i = 0; i < len; i++, str++)
	{
		/* Use two byte from the current nano-second to pseudo-randomly
		 * increase the ASCII character 'A' into another character,
		 * which will then subsitute the character at *str.
		 */
		*str = (char)('A' + (r&15) + (r&16));
		r >>= 5;
	}
}

/* Tries to create a shared memory object and returns its file descriptor if
 * successful.
 */
static bool buffer_get_shm_fd (int *fd, size_t size)
{
	char name[] = "/wayneko-RANDOM";
	char *rp    = name + strlen("/wayneko-"); /* Pointer to random part. */
	size_t rl   = strlen("RANDOM"); /* Length of random part. */

	/* Try a few times to get a unique name. */
	for (int tries = 100; tries > 0; tries--)
	{
		/* Make the name pseudo-random to not conflict with other
		 * running instances.
		 */
		buffer_randomize_string(rp, rl);

		/* Try to create a shared memory object. Returns -1 if the
		 * memory object already exists.
		 */
		*fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

		/* If a shared memory object was created, set its size and
		 * return its file descriptor.
		 */
		if ( *fd >= 0 )
		{
			shm_unlink(name);
			if ( ftruncate(*fd, (off_t)size) < 0 )
			{
				fprintf(stderr, "ERROR: ftruncate: %s.\n", strerror(errno));
				close(*fd);
				return false;
			}
			return true;
		}

		/* The EEXIST error means that the name is not unique and we
		 * must try again.
		 */
		if ( errno != EEXIST )
		{
			fprintf(stderr, "ERROR: shm_open: %s.\n", strerror(errno));
			return false;
		}
	}

	return false;
}

static void buffer_handle_release (void *data, struct wl_buffer *wl_buffer)
{
	struct Buffer *buffer = (struct Buffer *)data;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

static void buffer_finish (struct Buffer *buffer)
{
	if ( buffer->wl_buffer != NULL )
		wl_buffer_destroy(buffer->wl_buffer);
	if ( buffer->pixman_image != NULL )
		pixman_image_unref(buffer->pixman_image);
	if ( buffer->mmap != NULL )
		munmap(buffer->mmap, buffer->size);
}

static void buffer_destroy (struct Buffer *buffer)
{
	wl_list_remove(&buffer->link);
	free(buffer);
}

#define PIXMAN_STRIDE(A, B) (((PIXMAN_FORMAT_BPP(A) * B + 7) / 8 + 4 - 1) & -4)
static bool buffer_init (struct Buffer *buffer, uint32_t width, uint32_t height)
{
	assert(!buffer->busy);

	bool ret = true;
	int fd = -1;
	struct wl_shm_pool *shm_pool = NULL;

	buffer->width  = width;
	buffer->height = height;
	buffer->stride = (uint32_t)PIXMAN_STRIDE(PIXMAN_a8r8g8b8, (int32_t)width);
	buffer->size   = (size_t)(buffer->stride * height);

	if ( buffer->size == 0 )
	{
		ret = false;
		goto cleanup;
	}

	if (! buffer_get_shm_fd(&fd, buffer->size))
	{
		ret = false;
		goto cleanup;
	}

	buffer->mmap = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if ( buffer->mmap == MAP_FAILED )
	{
		fprintf(stderr, "ERROR: mmap: %s.\n", strerror(errno));
		ret = false;
		goto cleanup;
	}

	shm_pool = wl_shm_create_pool(wl_shm, fd, (int32_t)buffer->size);
	if ( shm_pool == NULL )
	{
		ret = false;
		goto cleanup;
	}

	buffer->wl_buffer = wl_shm_pool_create_buffer(shm_pool, 0, (int32_t)width,
			(int32_t)height, (int32_t)buffer->stride, WL_SHM_FORMAT_ARGB8888);
	if ( buffer->wl_buffer == NULL )
	{
		ret = false;
		goto cleanup;
	}
	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);

	buffer->pixman_image = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8,
			(int32_t)width, (int32_t)height, buffer->mmap, (int32_t)buffer->stride);
	if ( buffer->pixman_image == NULL )
	{
		ret = false;
		goto cleanup;
	}

cleanup:
	if ( shm_pool != NULL )
		wl_shm_pool_destroy(shm_pool);
	if ( fd != -1 )
		close(fd);
	if (! ret)
		buffer_finish(buffer);

	return ret;
}
#undef PIXMAN_STRIDE

static struct Buffer *buffer_pool_find_suitable_buffer (uint32_t width, uint32_t height)
{
	struct Buffer *first_unbusy_buffer = NULL;
	struct Buffer *current = NULL;
	wl_list_for_each(current, &buffer_pool, link)
	{
		if (current->busy)
			continue;
		first_unbusy_buffer = current;
		if ( current->width != width )
			continue;
		if ( current->height != height )
			continue;
		return current;
	}

	/* No buffer has matching dimensions, however we do have an unbusy
	 * buffer which we can just re-init.
	 */
	if ( first_unbusy_buffer != NULL )
	{
		buffer_finish(first_unbusy_buffer);
		if (!buffer_init(first_unbusy_buffer, width, height))
		{
			buffer_destroy(first_unbusy_buffer);
			return NULL;
		}
		return first_unbusy_buffer;
	}

	return NULL;
}

static struct Buffer *buffer_pool_new_buffer (uint32_t width, uint32_t height)
{
	struct Buffer *buffer = calloc(1, sizeof(struct Buffer));
	if ( buffer == NULL )
	{
		fprintf(stderr, "ERROR: calloc(): %s\n", strerror(errno));
		return NULL;
	}

	memset(buffer, 0, sizeof(struct Buffer));
	wl_list_insert(&buffer_pool, &buffer->link);
	if (!buffer_init(buffer, width, height))
	{
		buffer_destroy(buffer);
		return NULL;
	}

	return buffer;
}

static void buffer_pool_cull_buffers (struct Buffer *skip)
{
	int to_remove = wl_list_length(&buffer_pool) - (max_buffer_multiplicity * surface_amount);
	struct Buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &buffer_pool, link)
	{
		if ( buffer == skip )
			continue;
		if ( to_remove == 0 )
			break;
		if (buffer->busy)
			continue;
		buffer_finish(buffer);
		buffer_destroy(buffer);
		to_remove--;
	}
}

/**
 * Get a buffer of the specified dimenisons. If possible an idle buffer is
 * reused, otherweise a new one is created.
 */
static struct Buffer *buffer_pool_next_buffer (uint32_t width, uint32_t height)
{
	struct Buffer *ret = buffer_pool_find_suitable_buffer(width, height);
	if ( ret == NULL )
		ret = buffer_pool_new_buffer(width, height);

	if ( wl_list_length(&buffer_pool) > max_buffer_multiplicity * surface_amount )
		buffer_pool_cull_buffers(ret);

	return ret;
}

static void buffer_pool_destroy_all_buffers (void)
{
	struct Buffer *buffer, *tmp;
	wl_list_for_each_safe(buffer, tmp, &buffer_pool, link)
	{
		buffer_finish(buffer);
		buffer_destroy(buffer);
	}
}

/**********
 *        *
 *  Seat  *
 *        *
 **********/
static struct Seat *seat_from_global_name (uint32_t name)
{
	struct Seat *seat;
	wl_list_for_each(seat, &seats, link)
		if ( seat->global_name == name )
			return seat;
	return NULL;
}

static void seat_release_pointer (struct Seat *seat)
{
	seat->on_surface = false;
	if (seat->wl_pointer)
	{
		wl_pointer_release(seat->wl_pointer);
		seat->wl_pointer = NULL;
	}
}

static void pointer_handle_enter (void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *wl_surface,
		wl_fixed_t x, wl_fixed_t y)
{
	struct Seat *seat = (struct Seat *)data;
	assert(wl_surface == surface.wl_surface);
	seat->on_surface = true;
	seat->surface_x = (uint32_t)wl_fixed_to_int(x);

	/* Abort current animation frame. */
	animation_ticks_until_next_frame = 0;
}

static void pointer_handle_leave (void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *wl_surface)
{
	struct Seat *seat = (struct Seat *)data;
	assert(wl_surface == surface.wl_surface);
	assert(seat->on_surface == true);
	seat->on_surface = false;
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct Seat *seat = (struct Seat *)data;
	assert(seat->on_surface == true);
	seat->surface_x = (uint32_t)wl_fixed_to_int(x);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter         = pointer_handle_enter,
	.leave         = pointer_handle_leave,
	.motion        = pointer_handle_motion,
	.axis_discrete = noop,
	.axis          = noop,
	.axis_source   = noop,
	.axis_stop     = noop,
	.button        = noop,
	.frame         = noop,
};

static void seat_bind_pointer (struct Seat *seat)
{
	seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
	wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
}

static void seat_handle_capabilities (void *data, struct wl_seat *wl_seat,
		uint32_t capabilities)
{
	struct Seat *seat = (struct Seat *)data;

	if ( capabilities & WL_SEAT_CAPABILITY_POINTER )
		seat_bind_pointer(seat);
	else
		seat_release_pointer(seat);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = noop,
};

static void seat_new (struct wl_seat *wl_seat, uint32_t name)
{
	struct Seat *seat = calloc(1, sizeof(struct Seat));
	if ( seat == NULL )
	{
		fprintf(stderr, "ERROR: calloc(): %s\n", strerror(errno));
		return;
	}

	seat->wl_seat = wl_seat;
	seat->global_name = name;
	seat->on_surface = false;

	wl_seat_set_user_data(seat->wl_seat, seat);
	wl_list_insert(&seats, &seat->link);

	wl_seat_add_listener(wl_seat, &seat_listener, seat);
}

static void seat_destroy (struct Seat *seat)
{
	seat_release_pointer(seat);
	wl_seat_destroy(seat->wl_seat);
	wl_list_remove(&seat->link);
	free(seat);
}

/***********
 *         *
 *  Atlas  *
 *         *
 ***********/
static void atlas_deinit (void)
{
	if ( neko_atlas != NULL )
	{
		pixman_image_unref(neko_atlas);
		neko_atlas = NULL;
	}
	if ( neko_atlas_bg_fill != NULL )
	{
		pixman_image_unref(neko_atlas_bg_fill);
		neko_atlas_bg_fill = NULL;
	}
	if ( neko_atlas_border_fill != NULL )
	{
		pixman_image_unref(neko_atlas_border_fill);
		neko_atlas_border_fill = NULL;
	}
}

static bool atlas_init (void)
{
	assert(neko_atlas == NULL);

	neko_atlas = pixman_image_create_bits_no_clear(
		PIXMAN_a1, neko_bitmap_width, neko_bitmap_height,
		(uint32_t *)(&neko_bitmap_bits), neko_bitmap_stride
	);
	if ( neko_atlas == NULL )
	{
		fprintf(stderr, "ERROR: Failed to create texture atlas.\n");
		atlas_deinit();
		return false;
	}

	neko_atlas_bg_fill = pixman_image_create_solid_fill(&bg_colour);
	if ( neko_atlas_bg_fill == NULL )
	{
		fprintf(stderr, "ERROR: Failed to create solid fill.\n");
		atlas_deinit();
		return false;
	}

	neko_atlas_border_fill = pixman_image_create_solid_fill(&border_colour);
	if ( neko_atlas_border_fill == NULL )
	{
		fprintf(stderr, "ERROR: Failed to create solid fill.\n");
		atlas_deinit();
		return false;
	}

	return true;
}

static void atlas_composite_neko (struct Buffer *buffer, enum Neko neko_type, uint16_t x, uint16_t y)
{
	pixman_image_composite32(
		PIXMAN_OP_SRC,
		neko_atlas_bg_fill,    /* Source. */
		neko_atlas,            /* Mask. */
		buffer->pixman_image,  /* Destination. */
		0,                     /* Source x. */
		0,                     /* Source y. */
		(uint16_t)neko_type * neko_size, /* Mask x. */
		neko_size + (uint16_t)type * neko_size, /* Mask y. */
		x,                     /* Destination x. */
		y,                     /* Destination y. */
		neko_size,             /* Source width. */
		neko_size              /* Source height. */
	);
	pixman_image_composite32(
		PIXMAN_OP_OVER,
		neko_atlas_border_fill,
		neko_atlas,
		buffer->pixman_image,
		0,
		0,
		(uint16_t)neko_type * neko_size,
		(uint16_t)type * neko_size,
		x,
		y,
		neko_size,
		neko_size
	);
}

static bool animation_can_run_left (void)
{
	return surface.neko_x > neko_x_advance;
}

static void animation_neko_do_run_left (void)
{
	current_neko = current_neko == NEKO_RUN_LEFT_1 ? NEKO_RUN_LEFT_2 : NEKO_RUN_LEFT_1;
	animation_ticks_until_next_frame = 0;
}

static bool animation_can_run_right (void)
{
	return surface.neko_x < surface.width - neko_size - neko_x_advance;
}

static void animation_neko_do_run_right (void)
{
	current_neko = current_neko == NEKO_RUN_RIGHT_1 ? NEKO_RUN_RIGHT_2 : NEKO_RUN_RIGHT_1;
	animation_ticks_until_next_frame = 0;
}

static void animation_neko_advance_left (void)
{
	surface.prev_neko_x = surface.neko_x;
	surface.neko_x -= neko_x_advance;
}

static void animation_neko_advance_right (void)
{
	surface.prev_neko_x = surface.neko_x;
	surface.neko_x += neko_x_advance;
}

static void animation_neko_do_stare (bool quick)
{
	current_neko = NEKO_STARE;
	animation_ticks_until_next_frame = quick ? 5 : 10;
}

static void animation_neko_do_yawn (void)
{
	current_neko = NEKO_YAWN;
	animation_ticks_until_next_frame = 5;
}

static void animation_neko_do_think (void)
{
	current_neko = NEKO_THINK;
	animation_ticks_until_next_frame = 15;
}

static void animation_neko_do_shock (void)
{
	current_neko = NEKO_SHOCK;
	animation_ticks_until_next_frame = 3;
}

static void animation_neko_do_sleep (void)
{
	current_neko = current_neko == NEKO_SLEEP_1 ? NEKO_SLEEP_2 : NEKO_SLEEP_1;
	animation_ticks_until_next_frame = 10;
}

static void animation_neko_do_scratch (void)
{
	current_neko = current_neko == NEKO_SCRATCH_1 ? NEKO_SCRATCH_2 : NEKO_SCRATCH_1;
	animation_ticks_until_next_frame = 0;
}

static bool animtation_neko_wants_sleep (void)
{
	/* Neko likes to sleep at night. */
	const long now = time(NULL);
	struct tm tm = *localtime(&now);
	return tm.tm_hour >= 23 || tm.tm_hour <= 6;
}

/** Returns true if new frame is needed. */
static bool animation_next_state_with_hotspot (uint32_t x)
{
	if ( x < surface.neko_x ) /* Cursor left of neko. */
	{
		switch (current_neko)
		{
			case NEKO_SHOCK:
			case NEKO_RUN_LEFT_1:
			case NEKO_RUN_LEFT_2:
				if (!animation_can_run_left())
				{
					animation_neko_do_stare(true);
					return true;
				}
				animation_neko_advance_left();
				animation_neko_do_run_left();
				return true;

			default:
				animation_neko_do_shock();
				return true;
		}
	}
	else if ( x > surface.neko_x + neko_size ) /* Cursor right of neko. */
	{
		switch (current_neko)
		{
			case NEKO_SHOCK:
			case NEKO_RUN_RIGHT_1:
			case NEKO_RUN_RIGHT_2:
				if (!animation_can_run_right())
				{
					animation_neko_do_stare(true);
					return true;
				}
				animation_neko_advance_right();
				animation_neko_do_run_right();
				return true;

			default:
				animation_neko_do_shock();
				return true;
		}
	}
	else /* Cursor on neko. */
	{
		switch (current_neko)
		{
			case NEKO_SLEEP_1:
			case NEKO_SLEEP_2:
			case NEKO_YAWN:
				if (animtation_neko_wants_sleep())
					animation_neko_do_sleep();
				else
					animation_neko_do_stare(false);
				return true;

			case NEKO_STARE:
				if (animtation_neko_wants_sleep())
				{
					animation_neko_do_yawn();
					return true;
				}
				else
				{
					animation_neko_do_stare(false);
					return false;
				}

			default:
				animation_neko_do_stare(false);
				return true;
		}

	}
}

/** Returns true if new frame is needed. */
static bool animation_next_state_normal (void)
{
	/* Sleep at night, but with a small chance to wake up and do something.
         * If the neko is already awake, slightly higher chance to stay awake.
	 */
	const bool neko_is_sleeping = current_neko == NEKO_SLEEP_1 || current_neko == NEKO_SLEEP_2;
	if ( animtation_neko_wants_sleep() && (( neko_is_sleeping && rand() % 5 != 0 ) || ( !neko_is_sleeping && rand() % 2 != 0 )) )
	{
		switch (current_neko)
		{
			case NEKO_RUN_RIGHT_1:
			case NEKO_RUN_RIGHT_2:
			case NEKO_RUN_LEFT_1:
			case NEKO_RUN_LEFT_2:
				animation_neko_do_stare(true);
				return true;

			case NEKO_SLEEP_1:
			case NEKO_SLEEP_2:
				animation_neko_do_sleep();
				return true;

			default:
				animation_neko_do_yawn();
				return true;
		}
	}

	switch (current_neko)
	{
		case NEKO_STARE:
			switch (rand() % 24)
			{
				case 0:
					animation_neko_do_scratch();
					break;

				case 1:
					animation_neko_do_sleep();
					break;

				case 2:
					animation_neko_do_yawn();
					break;

				case 3:
					animation_neko_do_think();
					break;

				case 4:
					if (!animation_can_run_left())
						return false;
					animation_neko_advance_left();
					animation_neko_do_run_left();
					break;

				case 5:
					if (!animation_can_run_right())
						return false;
					animation_neko_advance_right();
					animation_neko_do_run_right();
					break;

				default:
					return false;
			}
			return true;

		case NEKO_RUN_RIGHT_1:
		case NEKO_RUN_RIGHT_2:
			if ( animation_can_run_right() && rand() % 4 != 0 )
			{
				animation_neko_do_run_right();
				animation_neko_advance_right();
			}
			else
				animation_neko_do_stare(false);
			return true;

		case NEKO_RUN_LEFT_1:
		case NEKO_RUN_LEFT_2:
			if ( animation_can_run_left() && rand() % 4 != 0 )
			{
				animation_neko_do_run_left();
				animation_neko_advance_left();
			}
			else
				animation_neko_do_stare(false);
			return true;

		case NEKO_SLEEP_1:
		case NEKO_SLEEP_2:
			if ( rand() % 4 == 0 )
			{
				if ( rand() % 2 == 0 )
					animation_neko_do_shock();
				else
					animation_neko_do_stare(false);
			}
			else
				animation_neko_do_sleep();
			return true;

		case NEKO_SCRATCH_1:
		case NEKO_SCRATCH_2:
			if ( rand() % 4 == 0 )
				animation_neko_do_stare(false);
			else
				animation_neko_do_scratch();
			return true;

		case NEKO_THINK:
			if ( rand() % 2 == 0 )
				animation_neko_do_stare(false);
			else
				animation_neko_do_shock();
			return true;

		case NEKO_YAWN:
			if ( rand() % 2 == 0 )
				animation_neko_do_stare(false);
			else
				animation_neko_do_sleep();
			return true;

		case NEKO_SHOCK:
			animation_neko_do_stare(true);
			return true;
	}

	assert(false); /* unreachable. */
	return false;
}

/** Returns true if new frame is needed. */
static bool animation_next_state (void)
{
	if ( animation_ticks_until_next_frame > 0 )
	{
		animation_ticks_until_next_frame--;
		return false;
	}

	struct Seat *seat;
	wl_list_for_each(seat, &seats, link)
	{
		if (seat->on_surface)
			return animation_next_state_with_hotspot(seat->surface_x);
	}
	return animation_next_state_normal();
}

/*************
 *           *
 *  Surface  *
 *           *
 *************/
static void surface_next_frame (void)
{
	if (!surface.configured)
		return;
	struct Buffer *buffer = buffer_pool_next_buffer(surface.width, surface.height);
	if ( buffer == NULL )
		return;

	pixman_image_fill_rectangles(
		PIXMAN_OP_CLEAR, buffer->pixman_image, &bg_colour,
		1, &(pixman_rectangle16_t){
			(int16_t)surface.prev_neko_x, (int16_t)0,
			(uint16_t)neko_size, (uint16_t)neko_size,
		}
	);
	atlas_composite_neko(buffer, current_neko, surface.neko_x, 0);

	wl_surface_set_buffer_scale(surface.wl_surface, 1);
	wl_surface_attach(surface.wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(
		surface.wl_surface,
		MIN(surface.neko_x, surface.prev_neko_x), 0,
		neko_size + neko_x_advance, neko_size
	);
	buffer->busy = true;
	wl_surface_commit(surface.wl_surface);
}

static void surface_destroy (void)
{
	if ( surface.layer_surface != NULL )
		zwlr_layer_surface_v1_destroy(surface.layer_surface);
	if ( surface.wl_surface != NULL )
		wl_surface_destroy(surface.wl_surface );
}

static void layer_surface_handle_configure (void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	(void)data;
	(void)layer_surface;

	zwlr_layer_surface_v1_ack_configure(surface.layer_surface, serial);
	surface.width = width;
	surface.height = height;

	/* Center neko on first configure. */
	if (!surface.configured)
	{
		surface.neko_x = (uint16_t)((width / 2) - neko_size);
		surface.prev_neko_x = surface.neko_x;
	}

	surface.configured = true;

	surface_next_frame();
}

static void layer_surface_handle_closed (void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	(void)data;
	(void)layer_surface;
	surface_destroy();
}

const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed    = layer_surface_handle_closed
};

static void surface_create (void)
{
	assert(!surface.configured);

	surface.wl_surface = wl_compositor_create_surface(wl_compositor);
	surface.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell,
		surface.wl_surface,
		NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
		"wayneko"
	);

	zwlr_layer_surface_v1_add_listener(
		surface.layer_surface,
		&layer_surface_listener,
		NULL
	);
	zwlr_layer_surface_v1_set_size(
		surface.layer_surface,
		0, desired_surface_height
	);
	zwlr_layer_surface_v1_set_anchor(
		surface.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
	);

	if (! follow_pointer)
	{
		struct wl_region *region = wl_compositor_create_region(wl_compositor);
		wl_surface_set_input_region(surface.wl_surface, region);
		wl_region_destroy(region);
	}

	wl_surface_commit(surface.wl_surface);
}

/**********
 *        *
 *  Main  *
 *        *
 **********/
static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if ( strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0 )
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	else if ( strcmp(interface, wl_compositor_interface.name) == 0 )
		wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if ( strcmp(interface, wl_shm_interface.name) == 0 )
		wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if ( strcmp(interface, wl_seat_interface.name) == 0 )
	{
		struct wl_seat *wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
		seat_new(wl_seat, name);
	}
}

static void registry_handle_global_remove (void *data, struct wl_registry *registry,
		uint32_t name)
{
	struct Seat *seat = seat_from_global_name(name);
	if ( seat != NULL )
		seat_destroy(seat);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static char *check_for_interfaces (void)
{
	if ( wl_compositor == NULL )
		return "wl_compositor";
	if ( wl_shm == NULL )
		return "wl_shm";
	if ( layer_shell == NULL )
		return "wlr_layershell_v1";
	return NULL;
}

static void sync_handle_done (void *data, struct wl_callback *wl_callback, uint32_t other)
{
	wl_callback_destroy(wl_callback);
	sync_callback = NULL;

	const char *missing = check_for_interfaces();
	if ( missing != NULL )
	{
		fprintf(stderr, "ERROR, Wayland compositor does not support %s.\n", missing);
		loop = false;
		ret = EXIT_FAILURE;
		return;
	}

	surface_create();
}

static const struct wl_callback_listener sync_callback_listener = {
	.done = sync_handle_done,
};

static void timespec_diff (struct timespec *a, struct timespec *b, struct timespec *result)
{
	result->tv_sec  = a->tv_sec  - b->tv_sec;
	result->tv_nsec = a->tv_nsec - b->tv_nsec;
	if ( result->tv_nsec < 0 )
	{
		result->tv_sec--;
		result->tv_nsec += 1000000000L;
	}
}

static bool colour_from_hex (pixman_color_t *colour, const char *hex)
{
	if ( strlen(hex) != strlen("0xRRGGBBAA") && strlen(hex) != strlen("0xRRGGBB") )
	{
		fprintf(stderr, "ERROR: Invalid colour: %s\n", hex);
		return false;
	}

	uint16_t r = 0, g = 0, b = 0, a = 255;

	if ( 4 != sscanf(hex, "0x%02hx%02hx%02hx%02hx", &r, &g, &b, &a)
			&& 3 != sscanf(hex, "0x%02hx%02hx%02hx", &r, &g, &b) )
	{
		fprintf(stderr, "ERROR: Invalid colour: %s\n", hex);
		return false;
	}

	colour->alpha = (uint16_t)(((double)a / 255.0) * 65535.0);
	colour->red   = (uint16_t)((((double)r / 255.0) * 65535.0) * colour->alpha / 0xffff);
	colour->green = (uint16_t)((((double)g / 255.0) * 65535.0) * colour->alpha / 0xffff);
	colour->blue  = (uint16_t)((((double)b / 255.0) * 65535.0) * colour->alpha / 0xffff);

	return true;
}

static char *get_argument (int argc, char *argv[], int *i)
{
	if ( argc == (*i) + 1 )
	{
		fprintf(stderr, "ERROR: Flag '%s' requires a parameter.\n", argv[(*i)]);
		return NULL;
	}
	(*i)++;
	return argv[(*i)];
}

static bool colour_from_flag (pixman_color_t *colour, int argc, char *argv[], int *i)
{
	const char *hex = get_argument(argc, argv, i);
	if ( hex == NULL )
		return false;
	if (!colour_from_hex(colour, hex))
		return false;
	return true;
}

int main (int argc, char *argv[])
{
	init_signals();

	colour_from_hex(&bg_colour, "0xFFFFFF");
	colour_from_hex(&border_colour, "0x000000");

	srand((unsigned int)time(0));

	for (int i = 1; i < argc; i++)
	{
		if ( strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0 )
		{
			fputs(usage, stderr);
			return EXIT_SUCCESS;
		}
		else if ( strcmp(argv[i], "--background-colour") == 0 )
		{
			if (!colour_from_flag(&bg_colour, argc, argv, &i))
				return EXIT_FAILURE;
		}
		else if ( strcmp(argv[i], "--outline-colour") == 0 )
		{
			if (!colour_from_flag(&border_colour, argc, argv, &i))
				return EXIT_FAILURE;
		}
		else if ( strcmp(argv[i], "--type") == 0 )
		{
			const char *t = get_argument(argc, argv, &i);
			if ( t == NULL )
				return EXIT_FAILURE;

			if ( strcmp(t, "neko") == 0 )
				type = NEKO;
			else if ( strcmp(t, "inu") == 0 )
				type = INU;
			else if ( strcmp(t, "random") == 0 )
				type = rand() % 2 == 0 ? NEKO : INU;
			else
			{
				fprintf(stderr, "ERROR: Unknown argument '%s' for flag '--type'.\n", t);
				return EXIT_FAILURE;
			}
		}
		else if ( strcmp(argv[i], "--follow-pointer") == 0 )
		{
			const char *t = get_argument(argc, argv, &i);
			if ( t == NULL )
				return EXIT_FAILURE;

			if ( strcmp(t, "yes") == 0 || strcmp(t, "on") == 0 || strcmp(t, "true") == 0 )
				follow_pointer = true;
			else if ( strcmp(t, "no") == 0 || strcmp(t, "off") == 0 || strcmp(t, "false") == 0 )
				follow_pointer = false;
			else
			{
				fprintf(stderr, "ERROR: Unknown argument '%s' for flag '--follow-pointer'.\n", t);
				return EXIT_FAILURE;
			}
		}
		else
		{
			fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	wl_list_init(&seats);
	wl_list_init(&buffer_pool);

	if (!atlas_init())
		return EXIT_FAILURE;

	/* We query the display name here instead of letting wl_display_connect()
	 * figure it out itself, because libwayland (for legacy reasons) falls
	 * back to using "wayland-0" when $WAYLAND_DISPLAY is not set, which is
	 * generally not desirable.
	 */
	const char *display_name = getenv("WAYLAND_DISPLAY");
	if ( display_name == NULL )
	{
		fputs("ERROR: WAYLAND_DISPLAY is not set.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_display = wl_display_connect(display_name);
	if ( wl_display == NULL )
	{
		fputs("ERROR: Can not connect to wayland display.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	struct pollfd pollfds[] = {
		{
			.fd = wl_display_get_fd(wl_display),
			.events = POLLIN,
		},
	};

	while (loop)
	{
		int current_timeout = animation_timeout;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		if (surface.configured)
		{
			const uint32_t nsec_delay = animation_timeout * 1000000;
			const uint32_t epsilon = 1000000;
			struct timespec time_since_last_tick;
			timespec_diff(&now, &last_tick, &time_since_last_tick);
			if ( time_since_last_tick.tv_sec > 0 || time_since_last_tick.tv_nsec >= nsec_delay - epsilon )
			{
				if (animation_next_state())
				 	surface_next_frame();
				clock_gettime(CLOCK_MONOTONIC, &last_tick);
			}
			else
			{
				const int _current_timeout = (int)(nsec_delay - time_since_last_tick.tv_nsec) / 1000000;
				if ( current_timeout == -1 || current_timeout > _current_timeout )
					current_timeout = _current_timeout;
			}
		}

		/* Flush pending Wayland events/requests. */
		while ( wl_display_prepare_read(wl_display) != 0 )
		{
			if ( wl_display_dispatch_pending(wl_display) != 0 )
			{
				fprintf(stderr, "ERROR: wl_display_dispatch_pending(): %s\n", strerror(errno));
				ret = EXIT_FAILURE;
				goto exit_main_loop;
			}
		}
		while (true)
		{
			/* Returns the amount of bytes flushed. */
			const int flush_ret = wl_display_flush(wl_display);
			if (flush_ret == -1) /* Error. */
			{
				if ( errno == EAGAIN )
					continue;
				fprintf(stderr, "ERROR: wl_display_flush(): %s\n", strerror(errno));
				ret = EXIT_FAILURE;
				goto exit_main_loop;
			}
			else if (flush_ret == 0) /* Done flushing. */
				break;
		}

		if ( poll(pollfds, 1, current_timeout) < 0 )
		{
			if ( errno == EINTR ) /* Interrupt: Signal received. */
				continue;
			fprintf(stderr, "ERROR: poll(): %s.\n", strerror(errno));
			ret = EXIT_FAILURE;
			break;
		}

		if ( wl_display_read_events(wl_display) == -1 )
		{
			fprintf(stderr, "ERROR: wl_display_read_events(): %s.\n", strerror(errno));
			break;
		}
		if ( wl_display_dispatch_pending(wl_display) == -1 )
		{
			fprintf(stderr, "ERROR: wl_display_dispatch_pending(): %s.\n", strerror(errno));
			break;
		}
	}

	/* Since C doesn't have first-class support for exiting the outer-loop
	 * from inside a nested loop, we unfortunately need to use a jump label.
	 */
exit_main_loop:

	close(pollfds[0].fd);

	surface_destroy();
	buffer_pool_destroy_all_buffers();

	if ( wl_compositor != NULL )
		wl_compositor_destroy(wl_compositor);
	if ( wl_shm != NULL )
		wl_shm_destroy(wl_shm);
	if ( layer_shell != NULL )
		zwlr_layer_shell_v1_destroy(layer_shell);
	if ( sync_callback != NULL )
		wl_callback_destroy(sync_callback);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	atlas_deinit();

	return ret;
}
