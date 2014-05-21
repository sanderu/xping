/*-
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <mph@hoth.dk> wrote this file. As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return Martin Topholm
 * ----------------------------------------------------------------------------
 */

#include <sys/param.h>
#include <netinet/in.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/dns.h>

#include "xping.h"

#define SETRES(t,i,r) t->res[(t->npkts+i) % NUM] = r
#define GETRES(t,i) t->res[(t->npkts+i) % NUM]

/* Option flags */
int	i_interval = 1000;
int	a_flag = 0;
int	c_count = 0;
int	A_flag = 0;
int	C_flag = 0;
int	T_flag = 0;
int	v4_flag = 0;
int	v6_flag = 0;

/* Global structures */
int	fd4, fd4errno;
int	fd6, fd6errno;
struct	event_base *ev_base;
struct	evdns_base *dns;
struct	timeval tv_interval;
int	numtargets = 0;
int	numcomplete = 0;

struct target *list = NULL;

void (*ui_init)(void) = termio_init;
void (*ui_update)(struct target *) = termio_update;
void (*ui_cleanup)(void) = termio_cleanup;

/*
 * Signal to catch program termination
 */
void
sigint(int sig)
{
	event_base_loopexit(ev_base, NULL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
}

/*
 * Register status for send and "timed out" requests and send a probe.
 */
static void
target_probe(int fd, short what, void *thunk)
{
	struct target *t = thunk;

	/* Check packet count limit */
	if (c_count && t->npkts >= c_count) {
		numcomplete++;
		event_del(t->ev_write);
		if (numcomplete >= numtargets) {
			event_base_loopexit(ev_base, NULL);
		}
		return;
	}

	/* Missed request */
	if (t->npkts > 0 && GETRES(t, -1) != '.') {
		if (GETRES(t, -1) == ' ')
			target_mark(t, t->npkts - 1, '?');
		if (A_flag == 1)
			write(STDOUT_FILENO, "\a", 1);
		else if (A_flag >= 2 &&
		    GETRES(t, -4) == '.' &&
		    GETRES(t, -3) == '.' &&
		    GETRES(t, -2) != '.' &&
		    GETRES(t, -1) != '.')
			write(STDOUT_FILENO, "\a", 1);
	}

	/* Transmit request */
	probe_send(t->prb, t->npkts);
	t->npkts++;

	ui_update(t);
}

/*
 * Does the scheduling of periodic transmissions.
 */
static void
target_probe_sched(int fd, short what, void *thunk)
{
	struct target *t = thunk;

	t->ev_write = event_new(ev_base, -1, EV_PERSIST, target_probe, t);
	event_add(t->ev_write, &tv_interval);
	target_probe(fd, what, thunk);
}

/*
 * Mark a target and sequence with given symbol
 */
void
target_mark(struct target *t, int seq, int ch)
{
	t->res[seq % NUM] = ch;
	if (a_flag && ch == '.') {
		if (a_flag == 1)
			write(STDOUT_FILENO, "\a", 1);
		else if (a_flag >=2 &&
		    t->res[(seq-3) % NUM] != '.' &&
		    t->res[(seq-2) % NUM] != '.' &&
		    t->res[(seq-1) % NUM] == '.' &&
		    t->res[(seq-0) % NUM] == '.')
			write(STDOUT_FILENO, "\a", 1);
	}

	if (seq == t->npkts - 1)
		ui_update(t);
	else
		ui_update(NULL); /* this is a late reply, need full update to redraw this */
}

/*
 * Clear a mark, used before sending a new probe
 */
void
target_unmark(struct target *t, int seq)
{
	t->res[seq % NUM] = ' ';
}

/*
 * Target resolved update address family
 */
void
target_resolved(struct target *t, int af, void *address)
{
	t->af = af;
	ui_update(NULL);
}

/*
 * Create a new a probe target, apply resolver if needed.
 */
static int
target_add(const char *line)
{
	struct target *t;

	t = (struct target *)calloc(1, sizeof(*t));
	if (t == NULL)
		return -1;
	memset(t->res, ' ', sizeof(t->res));
	strncat(t->host, line, sizeof(t->host) - 1);
	DL_APPEND(list, t);
	t->prb = probe_new(line, t);
	if (t->prb == NULL)
		return -1;
	numtargets++;
	return 0;
}

void
usage(const char *whine)
{
	if (whine != NULL) {
		fprintf(stderr, "%s\n", whine);
	}
	fprintf(stderr,
	    "usage: xping [-46ACTVah] [-c count] [-i interval] host [host [...]]\n"
	    "\n");
	exit(EX_USAGE);
}

/*
 * Continiously probing multiple hosts using ICMP-ECHO. As packets are
 * received dots are printed on the screen. Hosts not responding before
 * next packet is due will get a questionmark in the display. The probing
 * stops when SIGINT is received.
 */
int
main(int argc, char *argv[])
{
	char buf[BUFSIZ];
	struct timeval tv;
	struct target *t;
	char *end;
	int i;
	int len;
	char ch;

	/* Open RAW-socket and drop root-privs */
	fd4 = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	fd4errno = errno;
	fd6 = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	fd6errno = errno;
	setuid(getuid());

	/* Parse command line options */
	while ((ch = getopt(argc, argv, "46ACTac:i:hV")) != -1) {
		switch(ch) {
		case '4':
			v4_flag = 1;
			v6_flag = 0;
			break;
		case '6':
			v4_flag = 0;
			v6_flag = 1;
			break;
		case 'a':
			a_flag++;
			break;
		case 'A':
			A_flag++;
			break;
		case 'C':
			C_flag = 1;
			break;
		case 'c':
			c_count = strtol(optarg, &end, 10);
			if (*optarg != '\0' && *end != '\0')
				usage("Invalid count");
			break;
		case 'T':
			T_flag = 1;
			break;
		case 'i':
			i_interval = strtod(optarg, &end) * 1000;
			if (*optarg != '\0' && *end != '\0')
				usage("Invalid interval");
			if (i_interval < 1000 && getuid() != 0)
				usage("Dangerous interval");
			break;
		case 'V':
			fprintf(stderr, "%s %s (built %s)\n", "xping",
			    version, built);
			return (0);
		case 'h':
			usage(NULL);
			/* NOTREACHED */
		default:
			usage(NULL);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	tv_interval.tv_sec = i_interval / 1000;
	tv_interval.tv_usec = i_interval % 1000 * 1000;

	/* Prepare event system and inbound socket */
	ev_base = event_base_new();
	dns = evdns_base_new(ev_base, 1);
	probe_setup();

	/* Read targets from program arguments and/or stdin. */
	list = NULL;
	for (i=0; i<argc; i++) {
		if (target_add(argv[i]) < 0) {
			return 1;
		}
	}
	if (!isatty(STDIN_FILENO) || argc < 1) {
		while(fgets(buf, sizeof(buf), stdin) != NULL) {
			if ((end = strchr(buf, '#')) != NULL)
				*end = '\0';
			for (len = strlen(buf) - 1; len > 0; len--) {
				if (strchr(" \t\n", buf[len]) == NULL)
					break;
				buf[len] = '\0';
			}
			if (buf[0] == '#' || len < 1)
				continue;
			if (target_add(buf) < 0) {
				perror("malloc");
				return 1;
			}
		}
	}
	if (!isatty(STDOUT_FILENO)) {
		ui_init = report_init;
		ui_update = report_update;
		ui_cleanup = report_cleanup;
	}
	if (list == NULL) {
		usage("no arguments");
	}

	/* Initial scheduling with increasing delay, distributes
	 * transmissions across the interval and gives a cascading effect. */
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	DL_FOREACH(list, t) {
		t->ev_write = event_new(ev_base, -1, 0, target_probe_sched, t);
		event_add(t->ev_write, &tv);
		tv.tv_usec += 100*1000; /* target spacing: 100ms */
		tv.tv_sec += (tv.tv_usec >= 1000000 ? 1 : 0);
		tv.tv_usec -= (tv.tv_usec >= 1000000 ? 1000000 : 0);
	}

	/* Startup UI and probing */
	signal(SIGINT, sigint);
	signal(SIGTERM, sigint);
	ui_init();
	event_base_dispatch(ev_base);
	ui_cleanup();

	close(fd4);
	close(fd6);
	return 0;
}
