#include <assert.h>
#include <errno.h>
#include <evemu.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <libudev.h>
#include <libinput.h>
#include <time.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

static int open_restricted(const char *path, int flags,
			   void *data)
{
	int fd;
	fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *data)
{
	close(fd);
}

static const struct libinput_interface simple_interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

static struct libevdev*
init_random_device(const char *name)
{
	struct libevdev *d = libevdev_new();
	long int nbits = (random() % 64) + 1; /* how many bits do we want? */
	struct input_absinfo abs =  {
		.minimum = 0,
		.maximum = 100,
	};

	libevdev_set_name(d, name);

	while (nbits) {
		int type, code, max;

		type = (random() % EV_MAX) + 1; /* ignore EV_SYN */
		if (type == EV_REP)
			continue;

		max = libevdev_event_type_get_max(type);
		if (max == -1)
			continue;

		code = random() % (max + 1);

		libevdev_enable_event_code(d, type, code,
					   (type == EV_ABS) ? &abs : NULL);

		nbits--;
	}

	return d;
}

static void
drain_events(struct libinput *li)
{
	struct libinput_event *event;

	libinput_dispatch(li);
	while ((event = libinput_get_event(li))) {
		libinput_event_destroy(event);
		libinput_dispatch(li);
	}
}

static void
send_events(struct libevdev *d,
	    struct libevdev_uinput *uinput,
	    struct libinput *li)
{
	size_t nframes = random() % 200; /* how many event frames to send */
	struct bits {
		unsigned int type;
		unsigned int code;
	} bits[EV_MAX * KEY_MAX];
	size_t nbits = 0;
	unsigned int type, code;
	struct timespec tp_last;

	for (type = 1; type <= EV_MAX; type++) {
		int max;

		if (!libevdev_has_event_type(d, type))
			continue;

		max = libevdev_event_type_get_max(type);
		if (max == -1)
			continue;

		for (code = 0; code <= (unsigned int)max; code++) {
			if (!libevdev_has_event_code(d, type, code))
				continue;

			bits[nbits].type = type;
			bits[nbits].code = code;
			nbits++;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &tp_last);

	while (nframes--) {
		size_t nevents = random() % 12; /* events per frame */
		struct timespec tp;
		unsigned long dt = 0;

		clock_gettime(CLOCK_MONOTONIC, &tp);

		dt = (tp.tv_sec * 1E6 + tp.tv_nsec/1000) - (tp_last.tv_sec * 1E6 +
							    tp_last.tv_nsec/1000);

		while (nevents--) {
			int value;
			int idx;

			idx = random() % nbits;
			type = bits[idx].type;
			code = bits[idx].code;

			switch(type) {
			case EV_KEY:
			case EV_SW:
				value = random() % 2;
				break;
			default:
				value = random() % 50;
				break;
			}


			printf("E: %lu.%06lu %04x %04x %04d    ",
			       tp.tv_sec,
			       tp.tv_nsec / 1000,
			       type, code, value);
			printf("# %s / %-20s %d\n",
			       libevdev_event_type_get_name(type),
			       libevdev_event_code_get_name(type, code),
			       value);
			libevdev_uinput_write_event(uinput, type, code, value);
		}

		printf("E: %lu.%06lu %04x %04x %04d    ",
		       tp.tv_sec, tp.tv_nsec / 1000, EV_SYN, SYN_REPORT, 0);
		printf("# ------------ %s (%d) ---------- %+ldms\n",
		       libevdev_event_code_get_name(EV_SYN, SYN_REPORT),
		       0,
		       dt);

		libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
		drain_events(li);
	}
}

__attribute__((format (printf, 3, 0)))
static void
log_handler(struct libinput *libinput,
	    enum libinput_log_priority pri,
	    const char *format,
	    va_list args)
{
	vfprintf(stderr, format, args);
}

static void
test_one_device(int iteration)
{
	struct libinput *li;
	struct udev *udev;
	struct libevdev_uinput *uinput = NULL;
	struct libevdev *d;
	char name[64];
	int rc;
	struct evemu_device *device;
	int fd;


	snprintf(name, sizeof(name), "fuzzy device %d", iteration);
	d = init_random_device(name);
	assert(d);
	rc = libevdev_uinput_create_from_device(d,
						LIBEVDEV_UINPUT_OPEN_MANAGED,
						&uinput);
	assert(rc == 0);

	printf("#############################################################\n"
	       "#################### fuzzy device %d ########################\n"
	       "#############################################################\n",
	       iteration);
	fprintf(stderr,
	       "#############################################################\n"
	       "#################### fuzzy device %d ########################\n"
	       "#############################################################\n",
	       iteration);
	device = evemu_new(NULL);
	setbuf(stdout, NULL);
	fd = open(libevdev_uinput_get_devnode(uinput), O_RDWR);
	evemu_extract(device, fd);
	evemu_write(device, stdout);
	close(fd);

	udev = udev_new();
	li = libinput_udev_create_context(&simple_interface, NULL, udev);
	assert(li);
	libinput_log_set_handler(li, log_handler);
	libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_DEBUG);
	libinput_udev_assign_seat(li, "seat0");
	drain_events(li);

	send_events(d, uinput, li);

	libevdev_uinput_destroy(uinput);
	libevdev_free(d);
	libinput_unref(li);
	udev_unref(udev);
}

static bool stop = false;

static void
sighandler(int sig)
{
	stop = true;
}

int
main (int argc, char **argv)
{
	int iteration = 0;

	signal(SIGINT, sighandler);

	while (!stop) {
		test_one_device(iteration++);
		usleep(500);
	}

	return 0;
}

