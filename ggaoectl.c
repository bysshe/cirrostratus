#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "ctl.h"
#include "util.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/ether.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include <glib.h>

#define DEFAULT_INTERVAL 1.0

/**********************************************************************
 * Global variables
 */

/* Socket descriptor */
static int ctl_fd;

/* Hash tables holding the previous and new statistics for devices/interfaces */
static GTree *old_dev, *old_net, *new_dev, *new_net;

/* Time elapsed since the previous reading */
static double elapsed;

/* Our local socket address */
static struct sockaddr_un local_addr;

/* Set to 1 when a signal is received */
static volatile int do_quit;


/**********************************************************************
 * Functions
 */

static char* print_eth(const struct ether_addr *addr)
{
	static char buf[18];

	snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
		addr->ether_addr_octet[0], addr->ether_addr_octet[1],
		addr->ether_addr_octet[2], addr->ether_addr_octet[3],
		addr->ether_addr_octet[4], addr->ether_addr_octet[5]);
	return buf;
}

static void send_command(uint32_t cmd, char **argv)
{
	struct iovec iov[2];
	struct msghdr msg;
	char *buf;
	int i, len;

	for (i = len = 0; argv && argv[i]; i++)
		len += strlen(argv[i]) + 1;

	buf = g_malloc(len);
	for (i = len = 0; argv && argv[i]; i++)
	{
		memcpy(buf + len, argv[i], strlen(argv[i]) + 1);
		len += strlen(argv[i]) + 1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	iov[0].iov_base = &cmd;
	iov[0].iov_len = sizeof(cmd);
	if (len)
	{
		iov[1].iov_base = buf;
		iov[1].iov_len = len;
		msg.msg_iovlen = 2;
	}
	else
		msg.msg_iovlen = 1;

	if (sendmsg(ctl_fd, &msg, 0) < 0)
	{
		perror("sendmsg()");
		exit(1);
	}
}

static int receive_msg(void **pkt)
{
	int ret;

	*pkt = g_malloc(CTL_MAX_PACKET);
	ret = recv(ctl_fd, *pkt, CTL_MAX_PACKET, 0);
	if (ret < 0)
	{
		if (do_quit)
			exit(0);
		perror("recv()");
		exit(1);
	}
	*pkt = g_realloc(*pkt, ret);
	return ret;
}

static void add_devstat(GTree *dst, struct msg_devstat *stat, size_t buflen)
{
	struct device_stats *data;
	char *name;

	if (buflen < sizeof(*stat) + 1)
		return;
	name = g_strndup(stat->name, buflen - sizeof(*stat));
	data = g_new(struct device_stats, 1);
	*data = stat->stats;

	g_tree_insert(dst, name, data);
}

static void add_netstat(GTree *dst, struct msg_netstat *stat, size_t buflen)
{
	struct netif_stats *data;
	char *name;

	if (buflen < sizeof(*stat) + 1)
		return;
	name = g_strndup(stat->name, buflen - sizeof(*stat));
	data = g_new(struct netif_stats, 1);
	*data = stat->stats;

	g_tree_insert(dst, name, data);
}

/* Calculate the max. name length of devices/interfaces */
static int max_name_length(void *key, void *value G_GNUC_UNUSED, void *ptr)
{
	unsigned *len = ptr;
	char *name = key;

	if (strlen(name) > *len)
		*len = strlen(name);
	return FALSE;
}

#define DIFF(field)	diff.field = new->field - old->field
static int print_dev_record(void *key, void *value, void *ptr)
{
	struct device_stats *new = value, *old, diff;
	unsigned *len = ptr, allreq;
	double reqtime, qlen;
	struct timespec sum;
	char *name = key;

	old = g_tree_lookup(old_dev, name);
	if (!old)
	{
		old = g_new0(struct device_stats, 1);
		g_tree_insert(old_dev, g_strdup(name), old);
	}

	DIFF(read_cnt);
	DIFF(read_bytes);
	DIFF(write_cnt);
	DIFF(write_bytes);
	DIFF(other_cnt);
	DIFF(queue_length);
	DIFF(queue_stall);
	DIFF(queue_over);
	DIFF(ata_err);
	DIFF(proto_err);

	timespec_sub(&new->read_time, &old->read_time, &diff.read_time);
	timespec_sub(&new->write_time, &old->write_time, &diff.write_time);
	timespec_sub(&new->other_time, &old->other_time, &diff.other_time);
	memset(&sum, 0, sizeof(sum));
	timespec_add(&sum, &diff.read_time, &sum);
	timespec_add(&sum, &diff.write_time, &sum);
	timespec_add(&sum, &diff.other_time, &sum);

	allreq = diff.read_cnt + diff.write_cnt + diff.other_cnt;
	if (!allreq)
	{
		reqtime = 0;
		qlen = 0;
	}
	else
	{
		/* reqtime is in milliseconds */
		reqtime = (sum.tv_sec * 1000 + (double)sum.tv_nsec / 1000000) / allreq;
		qlen = (double)diff.queue_length / allreq;
	}

	printf("%-*s %8.1f %10.2f %8.1f %10.2f %3u %6.2f %2u %2u %2u %2u %8.2f\n", *len, name,
		(double)diff.read_cnt / elapsed,
		(double)diff.read_bytes / 1024 / elapsed,
		(double)diff.write_cnt / elapsed,
		(double)diff.write_bytes / 1024 / elapsed,
		(unsigned)diff.other_cnt,
		qlen,
		(unsigned)diff.queue_stall,
		(unsigned)diff.queue_over,
		(unsigned)diff.ata_err,
		(unsigned)diff.proto_err,
		reqtime);

	return FALSE;
}

static void print_dev_stats(unsigned len)
{
	if (g_tree_nnodes(new_dev))
		printf("%-*s   rrqm/s      rkB/s   wrqm/s      wkB/s oth avgqsz qs qf ae pe    svctm\n",
			len, "dev");

	g_tree_foreach(new_dev, print_dev_record, &len);
}

static int print_net_record(void *key, void *value, void *ptr)
{
	struct netif_stats *new = value, *old, diff;
	unsigned *len = ptr;
	char *name = key;
	double avgr;

	old = g_tree_lookup(old_net, name);
	if (!old)
	{
		old = g_new0(struct netif_stats, 1);
		g_tree_insert(old_net, g_strdup(name), old);
	}

	DIFF(rx_cnt);
	DIFF(rx_bytes);
	DIFF(tx_cnt);
	DIFF(tx_bytes);
	DIFF(dropped);
	DIFF(rx_runs);
	DIFF(tx_runs);

	if (diff.rx_runs + diff.tx_runs)
		avgr = (double)(diff.rx_cnt + diff.tx_cnt) / (diff.rx_runs + diff.tx_runs);
	else
		avgr = 0;

	printf("%-*s %8.1f %10.2f %8.1f %10.2f %3u %6.2f\n", *len, name,
		(double)diff.rx_cnt / elapsed,
		(double)diff.rx_bytes / 1024 / elapsed,
		(double)diff.tx_cnt / elapsed,
		(double)diff.tx_bytes / 1024 / elapsed,
		(unsigned)diff.dropped,
		avgr);

	return FALSE;
}

static void print_net_stats(unsigned len)
{
	if (g_tree_nnodes(new_net))
		printf("%-*s   rrqm/s      rkB/s   wrqm/s      wkB/s drp  avrun\n", len, "net");

	g_tree_foreach(new_net, print_net_record, &len);
}

static int str_compare(const void *a, const void *b, void *data G_GNUC_UNUSED)
{
	return strcmp(a, b);
}

static void do_monitor(int argc, char **argv)
{
	struct msg_uptime *uptime, prev_uptime;
	struct timespec diff, stime;
	double interval;
	unsigned len;
	uint32_t i;
	void *msg;

	/* If the first argument is a number, then treat it as the update interval */
	if (argv && argv[0])
	{
		char *p;

		interval = strtod(argv[0], &p);
		if (p && *p)
			interval = DEFAULT_INTERVAL;
		else
		{
			++argv;
			--argc;
		}
	}
	else
		interval = DEFAULT_INTERVAL;

	old_dev = g_tree_new_full(str_compare, NULL, g_free, g_free);
	old_net = g_tree_new_full(str_compare, NULL, g_free, g_free);
	new_dev = g_tree_new_full(str_compare, NULL, g_free, g_free);
	new_net = g_tree_new_full(str_compare, NULL, g_free, g_free);

	uptime = NULL;
	memset(&prev_uptime, 0, sizeof(prev_uptime));

	while (!do_quit)
	{
		g_tree_destroy(old_dev);
		g_tree_destroy(old_net);
		old_dev = new_dev;
		old_net = new_net;
		new_dev = g_tree_new_full(str_compare, NULL, g_free, g_free);
		new_net = g_tree_new_full(str_compare, NULL, g_free, g_free);

		send_command(CTL_CMD_GET_STATS, argv);
		len = receive_msg((void **)&uptime);
		if (len != sizeof(*uptime) || uptime->type != CTL_MSG_UPTIME)
		{
			g_free(uptime);
			fprintf(stderr, "Unexpected message\n");
			return;
		}

		timespec_sub(&uptime->uptime, &prev_uptime.uptime, &diff);
		prev_uptime = *uptime;
		g_free(uptime);

		elapsed = (double)diff.tv_sec + (double)diff.tv_nsec / NSEC_PER_SEC;

		do {
			len = receive_msg(&msg);
			if (len < 4)
				return;

			uint32_t *type = msg;
			switch (*type)
			{
				case CTL_MSG_OK:
					g_free(msg);
					goto print;
				case CTL_MSG_DEVSTAT:
					add_devstat(new_dev, msg, len);
					break;
				case CTL_MSG_NETSTAT:
					add_netstat(new_net, msg, len);
					break;
				default:
					fprintf(stderr, "Unexpected message\n");
					return;
			}
			g_free(msg);
		} while (1);

print:
		/* Min. size of the name field */
		len = 4;

		if (!argc)
		{
			g_tree_foreach(new_dev, max_name_length, &len);
			g_tree_foreach(new_net, max_name_length, &len);
		}
		else
		{
			for (i = 0; i < (unsigned)argc; i++)
				max_name_length(argv[i], NULL, &len);
		}

		print_dev_stats(len);
		if (g_tree_nnodes(new_dev) && g_tree_nnodes(new_net))
			printf("\n");
		print_net_stats(len);
		printf("\n");
		fflush(stdout);

		stime.tv_sec = interval;
		stime.tv_nsec = (interval - (long)interval) * 1000000000l;
		nanosleep(&stime, NULL);
	}
}

#define PRINT64(field) printf(#field ": %" PRIu64 "\n", stats->stats.field)
#define PRINT32(field) printf(#field ": %" PRIu32 "\n", stats->stats.field)
#define PRINTtime(field) printf(#field ": %g\n", \
	(double)stats->stats.field.tv_sec + \
	(double)stats->stats.field.tv_nsec / NSEC_PER_SEC)
static void dump_devstats(const struct msg_devstat *stats, unsigned length)
{
	if (length < sizeof(*stats))
		return;

	printf("# Statistics for device %.*s\n",
		(int)(length - sizeof(*stats)), stats->name);
	PRINT64(read_cnt);
	PRINT64(read_bytes);
	PRINTtime(read_time);
	PRINT64(write_cnt);
	PRINT64(write_bytes);
	PRINTtime(write_time);
	PRINT32(other_cnt);
	PRINTtime(other_time);
	PRINT64(io_slots);
	PRINT64(io_runs);
	PRINT64(queue_length);
	PRINT32(queue_stall);
	PRINT32(queue_over);
	PRINT32(ata_err);
	PRINT32(proto_err);
}

static void dump_netstats(const struct msg_netstat *stats, unsigned length)
{
	if (length < sizeof(*stats))
		return;

	printf("# Statistics for interface %.*s\n",
		(int)(length - sizeof(*stats)), stats->name);
	PRINT64(rx_cnt);
	PRINT64(rx_bytes);
	PRINT64(rx_runs);
	PRINT32(rx_buffers_full);
	PRINT64(tx_cnt);
	PRINT64(tx_bytes);
	PRINT64(tx_runs);
	PRINT32(tx_buffers_full);
	PRINT32(dropped);
	PRINT32(ignored);
	PRINT32(broadcast);
}

static void do_dump_stats(int argc, char **argv)
{
	struct msg_uptime *uptime;
	unsigned len;
	void *msg;

	send_command(CTL_CMD_GET_STATS, argv);
	len = receive_msg((void **)&uptime);
	if (len != sizeof(*uptime) || uptime->type != CTL_MSG_UPTIME)
	{
		g_free(uptime);
		fprintf(stderr, "Unexpected message\n");
		return;
	}
	g_free(uptime);

	do {
		len = receive_msg(&msg);
		if (len < 4)
			return;

		uint32_t *type = msg;
		switch (*type)
		{
			case CTL_MSG_OK:
				g_free(msg);
				return;
			case CTL_MSG_DEVSTAT:
				dump_devstats(msg, len);
				break;
			case CTL_MSG_NETSTAT:
				dump_netstats(msg, len);
				break;
			default:
				fprintf(stderr, "Unexpected message\n");
				return;
		}
		g_free(msg);
		printf("\n");
	} while (1);
}

static void do_reload(void)
{
	uint32_t cmd;
	int ret;

	cmd = CTL_CMD_RELOAD;
	ret = write(ctl_fd, &cmd, sizeof(cmd));
	if (ret != sizeof(cmd))
	{
		fprintf(stderr, "Failed to send the command\n");
		exit(1);
	}
	ret = read(ctl_fd, &cmd, sizeof(cmd));
	if (ret != sizeof(cmd))
	{
		fprintf(stderr, "Short read when receiving the status\n");
		exit(1);
	}
}

static void do_clear(int cmd, int argc, char **argv)
{
	unsigned len;
	void *msg;

	if (cmd != CTL_CMD_CLEAR_STATS && !argc)
	{
		fprintf(stderr, "No names were given on the command line\n");
		exit(1);
	}
	send_command(cmd, argv);

	len = receive_msg(&msg);
	if (len < 4)
	{
		fprintf(stderr, "Short read when receiving the status\n");
		exit(1);
	}
	g_free(msg);
}

static void dump_config(struct msg_config *msg, unsigned len)
{
	unsigned i, j;

	if (len < sizeof(*msg) + 1)
	{
		fprintf(stderr, "Short read\n");
		exit(1);
	}

	printf("Device %.*s:\n", (int)(len - sizeof(*msg)), msg->name);
	for (i = 0; i < msg->cfg.length; i += 16)
	{
		for (j = 0; j < 16 && i + j < msg->cfg.length; j++)
			printf("%02x ", msg->cfg.data[i + j]);
		while (j++ < 16)
			printf("   ");
		putchar(' ');
		for (j = 0; j < 16 && i + j < msg->cfg.length; j++)
			putchar(msg->cfg.data[i + j] < 32 || msg->cfg.data[i + j] > 127 ?
				'.' : msg->cfg.data[i + j]);
		putchar('\n');
	}
}

static void do_get_config(int argc, char **argv)
{
	unsigned len;
	void *msg;

	send_command(CTL_CMD_GET_CONFIG, argv);

	do {
		len = receive_msg(&msg);
		if (len < 4)
			return;

		uint32_t *type = msg;
		switch (*type)
		{
			case CTL_MSG_OK:
				g_free(msg);
				return;
			case CTL_MSG_CONFIG:
				dump_config(msg, len);
				break;
			default:
				fprintf(stderr, "Unexpected message\n");
				return;
		}
		g_free(msg);
		printf("\n");
	} while (1);
}

static void dump_maclist(struct msg_maclist *msg, unsigned len)
{
	unsigned i, j;

	if (len < sizeof(*msg) + 1)
	{
		fprintf(stderr, "Short read\n");
		exit(1);
	}

	printf("Device %.*s:\n", (int)(len - sizeof(*msg)), msg->name);
	for (i = 0; i < msg->list.length; i += 4)
	{
		for (j = 0; j < 4 && i + j < msg->list.length; j++)
		{
			printf("%s", print_eth(&msg->list.entries[i + j].e));
			if (j < 3)
				putchar(' ');
		}
		putchar('\n');
	}
}

static void do_get_maclist(int cmd, int argc, char **argv)
{
	unsigned len;
	void *msg;

	send_command(cmd, argv);

	do {
		len = receive_msg(&msg);
		if (len < 4)
			return;

		uint32_t *type = msg;
		switch (*type)
		{
			case CTL_MSG_OK:
				g_free(msg);
				return;
			case CTL_MSG_MACLIST:
				dump_maclist(msg, len);
				break;
			default:
				fprintf(stderr, "Unexpected message\n");
				return;
		}
		g_free(msg);
		printf("\n");
	} while (1);
}

static struct option longopts[] =
{
	{ "config",	required_argument,	NULL, 'c' },
	{ "help",	no_argument,		NULL, 'h' },
	{ "version",	no_argument,		NULL, 'V' },
	{ NULL }
};

static void usage(const char *prog, int error) G_GNUC_NORETURN;
static void usage(const char *prog, int error)
{
	printf("Usage: %s [options] <command> [args]\n", prog);
	printf("Valid options:\n");
	printf("\t-c FILE, --config FILE\tUse the specified config. file\n");
	printf("\t-h, --help\t\tThis help text\n");
	printf("\t-V, --version\t\tPrint the version number and exit\n");
	printf("Valid commands:\n");
	printf("\treload\t\t\t\tReload the configuration file\n");
	printf("\tmonitor [interval] [name...]\tMonitor devices/interfaces\n");
	printf("\tstats [name...]\t\t\tDump device/interface statistics\n");
	printf("\tshow-config [name...]\t\tShow the AoE configuration info\n");
	printf("\tshow-macmask [name...]\t\tShow the AoE MAC Mask list\n");
	printf("\tshow-reserve [name...]\t\tShow the AoE Reserve list\n");
	printf("\tclear-stats name [name...]\tClear device/interface statistics\n");
	printf("\tclear-config name [name...]\tClear the AoE configuration info\n");
	printf("\tclear-macmask name [name...]\tClear the AoE MAC Mask list\n");
	printf("\tclear-reserve name [name...]\tClear the AoE Reserve list\n");
	exit(error);
}

static void remove_local_socket(void)
{
	unlink(local_addr.sun_path);
}

static void signal_handler(int sig G_GNUC_UNUSED)
{
	do_quit = 1;
}

int main(int argc, char **argv)
{
	char *config_file = CONFIG_LOCATION, *ctl_socket;
	struct msg_hello *hello;
	struct sockaddr_un sun;
	struct sigaction sig;
	GError *error = NULL;
	GKeyFile *config;
	int ret, c, len;

	while (1)
	{
		c = getopt_long(argc, argv, "c:hi:V", longopts, NULL);
		if (c == -1)
			break;

		switch (c)
		{
			case 'c':
				config_file = optarg;
				break;
			case 'h':
				usage(argv[0], 0);
			case 'V':
				printf("%s\n", PACKAGE_STRING);
				exit(0);
			default:
				usage(argv[0], 1);

		}
	}

	argv += optind;
	argc -= optind;

	if (!argc)
	{
		fprintf(stderr, "You must specify a command.\n");
		exit(1);
	}

	config = g_key_file_new();
	g_key_file_set_list_separator(config, ',');
	ret = g_key_file_load_from_file(config, config_file, G_KEY_FILE_NONE, &error);
	if (!ret)
	{
		fprintf(stderr, "Loading the config file has failed: %s\n",
			error->message);
		g_error_free(error);
		exit(1);
	}

	ctl_socket = g_key_file_get_string(config, "defaults", "control-socket", NULL);
	if (!ctl_socket)
		ctl_socket = g_strdup(SOCKET_LOCATION);

	ctl_fd = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (ctl_fd == -1)
	{
		perror("socket()");
		exit(1);
	}

	/* Bind to a local name so the server can answer us */
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sun_family = AF_UNIX;
	snprintf(local_addr.sun_path, sizeof(local_addr.sun_path), "%s.%d",
		ctl_socket, (int)getpid());
	ret = bind(ctl_fd, (struct sockaddr *)&local_addr, SUN_LEN(&local_addr));
	if (ret)
	{
		perror("bind()");
		exit(1);
	}

	memset(&sig, 0, sizeof(sig));
	sig.sa_handler = signal_handler;
	sigaction(SIGINT, &sig, NULL);
	sigaction(SIGQUIT, &sig, NULL);
	sigaction(SIGTERM, &sig, NULL);

	atexit(remove_local_socket);

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", ctl_socket);
	ret = connect(ctl_fd, (struct sockaddr *)&sun, SUN_LEN(&sun));
	if (ret)
	{
		perror("connect()");
		exit(1);
	}

	send_command(CTL_CMD_HELLO, NULL);
	len = receive_msg((void **)&hello);
	if (len != sizeof(*hello) || hello->type != CTL_MSG_HELLO || hello->version != CTL_PROTO_VERSION)
	{
		fprintf(stderr, "Unknown response for HELLO\n");
		exit(1);
	}
	g_free(hello);

	if (!strcmp(argv[0], "monitor"))
		do_monitor(argc - 1, argv + 1);
	else if (!strcmp(argv[0], "stats"))
		do_dump_stats(argc - 1, argv + 1);
	else if (!strcmp(argv[0], "reload"))
		do_reload();
	else if (!strcmp(argv[0], "show-config"))
		do_get_config(argc - 1, argv + 1);
	else if (!strcmp(argv[0], "show-macmask"))
		do_get_maclist(CTL_CMD_GET_MACMASK, argc - 1, argv + 1);
	else if (!strcmp(argv[0], "show-reserve"))
		do_get_maclist(CTL_CMD_GET_RESERVE, argc - 1, argv + 1);
	else if (!strcmp(argv[0], "clear-stats"))
		do_clear(CTL_CMD_CLEAR_STATS, argc - 1, argv + 1);
	else if (!strcmp(argv[0], "clear-config"))
		do_clear(CTL_CMD_CLEAR_CONFIG, argc - 1, argv + 1);
	else if (!strcmp(argv[0], "clear-macmask"))
		do_clear(CTL_CMD_CLEAR_MACMASK, argc - 1, argv + 1);
	else if (!strcmp(argv[0], "clear-reserve"))
		do_clear(CTL_CMD_CLEAR_RESERVE, argc - 1, argv + 1);
	else
	{
		fprintf(stderr, "Unknown command\n");
		close(ctl_fd);
		exit(1);
	}
	
	close(ctl_fd);

	g_key_file_free(config);
	g_free(ctl_socket);

	return 0;
}
