#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

enum cmd_id {
	NOP,
	SECTION,
	END,
	BUS,
	DELAY,
	WRITE,
	READ,
};

struct cmd
{
	enum cmd_id id;
	union {
		long delay_us;
		uint16_t addr;
	};
	void *data;
	size_t size, allocated;
};

static void cmd_realloc(struct cmd *cmd, size_t need)
{
	if (cmd->allocated < need)
		cmd->data = realloc(cmd->data, cmd->allocated = need);
}

static void cmd_set_str(struct cmd *cmd, const char *s)
{
	cmd->size = strlen(s);
	cmd_realloc(cmd, cmd->size + 1);
	strcpy(cmd->data, s);
}

static int i2c_enable = 1;
static int i2c_timeout = 0;
static char *i2c_bus;
static int i2c_force;
static int i2c_fd = -1;

static int i2c_open(struct cmd *cmd)
{
	if (i2c_fd != -1)
		return 0;
	if (!i2c_bus) {
		static int once;
		if (once == 0) {
			fprintf(stderr, "No I2C bus specified\n");
			once = 1;
		}
		return -1;
	}
	i2c_fd = open(i2c_bus, O_RDWR);
	if (i2c_fd == -1) {
		fprintf(stderr, "%s: %s\n", i2c_bus, strerror(errno));
		exit(2);
	}
	if (i2c_timeout && ioctl(i2c_fd, I2C_TIMEOUT, i2c_timeout) < 0) {
		fprintf(stderr, "%s: timeout %d: %s\n", i2c_bus,
			i2c_timeout, strerror(errno));
		exit(2);
	}
	return 0;
}

static void i2c_write(struct cmd *cmd)
{
	if (i2c_fd == -1 && i2c_open(cmd) == -1)
		return;

	if (ioctl(i2c_fd, i2c_force ? I2C_SLAVE_FORCE : I2C_SLAVE, cmd->addr) < 0) {
		fprintf(stderr, "%s: setting slave address 0x%x: %s\n", i2c_bus,
			cmd->addr, strerror(errno));
		return;
	}
	ssize_t wr = write(i2c_fd, cmd->data, cmd->size);
	if (wr == -1)
		fprintf(stderr, "i2c: write slave 0x%x: %s\n", cmd->addr, strerror(errno));
	else if (wr != cmd->size)
		fprintf(stderr, "i2c: write slave 0x%x: short write %ld of %ld\n", cmd->addr, (long)wr, (long)cmd->size);
}

static void i2c_close(void)
{
	if (i2c_fd != -1) {
		close(i2c_fd);
		i2c_fd = -1;
	}
}

static void execute(struct cmd *cmd)
{
	switch (cmd->id) {
	case BUS:
		i2c_bus = strdup(cmd->data);
		i2c_close();
		break;
	case DELAY:
		printf("delay %ldus\n", cmd->delay_us);
		usleep(cmd->delay_us);
		break;
	case WRITE:
		printf("%c 0x%x", i2c_enable ? 'W' : 'w', cmd->addr);
		{
			unsigned int i;
			for (i = 0; i < cmd->size; ++i)
				printf(" %02X", *((uint8_t *)cmd->data + i));
			printf(" # %02X\n", cmd->addr << 1);
		}
		if (i2c_enable)
			i2c_write(cmd);
		break;
	case READ:
		printf("%c 0x%x: ", i2c_enable ? 'R' : 'r', cmd->addr);
		{
			unsigned int i;
			printf("@%02X", (cmd->addr << 1) | 1);
			for (i = 0; i < cmd->size; ++i)
				printf(" %02X", *((uint8_t *)cmd->data + i));
			printf("\n");
		}
		if (i2c_enable)
			; // i2c_read(cmd);
		break;
	default:
		fprintf(stderr, "exec %d [%u]\n", cmd->id, (unsigned)cmd->size);
	}
}

static const char SPACES[] = " \t\r\n";

static inline int at_comment(const char *s)
{
	return *s == ';' || *s == '#';
}

static inline int at_space(const char *s)
{
	return *s && strchr(SPACES, *s);
}

static inline int at_eol(const char *s)
{
	return !*s || *s == '\r' || *s == '\n';
}

static char *trim(char *s)
{
	char *e;

	s += strspn(s, SPACES);
	e = s + strlen(s);
	while (e > s) {
		if (at_space(--e))
			continue;
		e[1] = '\0';
		break;
	}
	return s;
}

static int strnumbase(char **s)
{
	if (strncmp(*s, "0x", 2) == 0) {
		*s = *s + 2;
		return 16;
	}
	if (strncmp(*s, "0d", 2) == 0) {
		*s = *s + 2;
		return 10;
	}
	if (strncmp(*s, "0b", 2) == 0) {
		*s = *s + 2;
		return 2;
	}
	if (strncmp(*s, "0o", 2) == 0) {
		*s = *s + 1;
		return 8;
	}
	return 0;
}

static int parse_addr(char **sp, int nr, struct cmd *cmd)
{
	char *p, *orig = *sp, *s = *sp;
	int base = strnumbase(&s);
	unsigned long v = strtoul(s, &p, base ? base : 16);

	if (!v || (unsigned long)v > 0x3fful) {
		fprintf(stderr, "%d: invalid address: %s\n", nr, orig);
		return -1;
	}
	cmd->addr = (uint16_t)v;
	*sp = p;
	return 0;
}

static int parse_bytes(char **sp, int nr, struct cmd *cmd)
{
	char *p, *s = *sp;
	uint8_t *bytes = cmd->data;

	while (!at_eol(s)) {
		int base;
		unsigned long v;

		s += strspn(s, SPACES);
		if (at_comment(s)) /* trailing comment */
			break;
		base = strnumbase(&s);
		v = strtoul(s, &p, base ? base : 16);
		if (p == s || (v & 0xff) != v) {
			fprintf(stderr, "%d: invalid data at %d: %s\n", nr, (int)(s - *sp), *sp);
			return -1;
		}
		*bytes++ = (uint8_t)v;
		s = p;
	}
	if (bytes == cmd->data) {
		fprintf(stderr, "%d: no data: %s\n", nr, *sp);
		return -1;
	}
	cmd->size = bytes - (uint8_t *)cmd->data;
	*sp = s;
	return 0;
}

static int parse(char *line, int nr, struct cmd *cmd)
{
	char *s = line + strspn(line, SPACES);

	cmd->id = NOP;
	cmd->size = 0;
	cmd->delay_us = 0;

	if (at_comment(s) || at_eol(s))
		goto end;

	if (':' == *s) {
		s = trim(s);
		cmd_set_str(cmd, s);
		cmd->id = SECTION;
		goto end;
	}
	if ((strncmp(s, "End", 3) == 0 || strncmp(s, "end", 3) == 0) &&
	    (at_eol(s + 3) || at_comment(s + 3) || at_space(s + 3))) {
		cmd->id = END;
		goto end;
	}
	if (strncmp(s, "bus", 3) == 0 && at_space(s + 3)) {
		s = trim(s + 3);
		cmd_set_str(cmd, s);
		cmd->id = BUS;
		goto end;
	}
	if (strncmp(s, "delay", 5) == 0 && at_space(s + 5)) {
		int base;
		char *p;
		s = trim(s + 5);
		base = strnumbase(&s);
		cmd->id = DELAY;
		cmd->delay_us = strtol(s, &p, base);
		switch (*p) {
		case 'u':
			break;
		case '\0':
		case '\t':
		case ' ':
		case '\r':
		case '\n':
		case ';':
		case '#':
		case 'm':
			cmd->delay_us *= 1000;
			break;
		case 's':
			cmd->delay_us *= 1000000;
			break;
		}
		goto end;
	}
	if ('W' == *s) {
		s = trim(s + 1);
		cmd->id = WRITE;
		cmd_realloc(cmd, strlen(s) / 2); /* more than enough */
		if (parse_addr(&s, nr, cmd) == -1)
			goto fail;
		if (parse_bytes(&s, nr, cmd) == -1)
			goto fail;
		goto end;
	}
	if ('R' == *s) {
		s = trim(s + 1);
		cmd->id = READ;
		cmd_realloc(cmd, strlen(s) / 2); /* more than enough */
		if (parse_addr(&s, nr, cmd) == -1)
			goto fail;
		if (parse_bytes(&s, nr, cmd) == -1)
			goto fail;
		goto end;
	}
	if (strchr("0123456789abcdefABCDEF", *s)) {
		char *p, *orig = s;
		int base = strnumbase(&s);
		unsigned long v = strtoul(s, &p, base ? base : 16);

		if (s == p || (unsigned long)v > 0xfful) {
			fprintf(stderr, "%d: invalid address: %s\n", nr, orig);
			goto fail;
		}
		cmd->id = v & 1 ? READ : WRITE;
		cmd->addr = (uint16_t)v >> 1;
		s = p;
		cmd_realloc(cmd, strlen(s) / 2); /* more than enough */
		if (parse_bytes(&s, nr, cmd) == -1)
			goto fail;
		goto end;
	}
fail:
	return -1;
end:
	return 0;
}

static void usage(char *argv[])
{
	printf("%s [-h] [-f] [-t <i2c-timeout>] [-d <i2c-bus-name-or-full-path>] [-s <section-search>] <cmd-file>\n"
	       "-h\t\tShow usage information\n"
	       "-f\t\tForce use of the slave addresses (dangerous!)\n"
	       "-d <path>\tUse this I2C bus\n"
	       "<cmd-file>\tFile to process for I2C commands\n"
	       "\n"
	       "<cmd-file> has following format:\n"
	       "\n"
	       ": section-name\n"
	       "# commentary\n"
	       "; commentary\n"
	       "[ bus <i2c-bus-name-or-full-path> ]\n"
	       "delay <delay>[(ms|us|s)] [ (#|;) pause, ms by default ]\n"
	       "W <addr> <byte> [<byte>...]   [ (#|;) write command commentary ]\n"
	       /* "W+ <addr> <byte> [<byte>...]   [ (#|;) repeated start + write command commentary ]\n" */
	       "R <addr> <byte> [<byte>...]   [ (#|;) read command commentary ]\n"
	       "<addr << 1 | rdwr> byte0 [bytes...]   [ (#|;) read/write command commentary ]\n"
	       "(End | end) [ (#|;) end of section commentary ]\n"
	       "<byte> is expected in hexadecimal, 0b (binary), 0 (octal), 0d (decimal),\n"
	       "and 0x (hex) prefixes supported\n"
	       "\n",
	       argv[0]);
}

int main(int argc, char *argv[])
{
	FILE *fp;
	const char *search = NULL;
	char line[BUFSIZ];
	int nr;
	struct cmd cmd = { 0 };
	int opt;

	argv[0] = basename(argv[0]);
	while ((opt = getopt(argc, argv, "d:s:t:fh")) != -1)
		switch (opt) {
		case 'd':
			i2c_bus = optarg;
			break;
		case 'f':
			i2c_force = 1;
			break;
		case 's':
			i2c_enable = 0;
			search = optarg;
			break;
		case 't':
			i2c_timeout = strtol(optarg, NULL, 0);
			break;
		default:
		case 'h':
			usage(argv);
			exit(1);
		}
	if (optind == argc) {
		usage(argv);
		exit(1);
	}
	if (strcmp(argv[optind], "-") == 0)
		fp = stdin;
	else {
		fp = fopen(argv[optind], "r");
		if (!fp) {
			fprintf(stderr, "%s: %s: %s\n", argv[0], argv[optind], strerror(errno));
			exit(2);
		}
	}
	nr = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (parse(line, ++nr, &cmd) == 0)
			switch (cmd.id) {
			case NOP:
				break;
			case SECTION:
				i2c_enable = search && strstr(cmd.data, search);
				printf("# Section%s %s\n", i2c_enable ? "" : " ignored", (char *)cmd.data);
				break;
			case END:
				i2c_enable = 0;
				i2c_close();
				fprintf(stderr, "end\n");
				break;
			default:
				execute(&cmd);
				break;
			}
	}
	if (fp != stdin)
		fclose(fp);
	free(cmd.data);
	i2c_close();

	return 0;
}

