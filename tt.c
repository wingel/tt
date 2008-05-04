/* Copyright 2001-2002 Christer Weinigel <wingel@hack.org>.  All
   rights reserved.  See the file COPYING for more information.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <getopt.h>
#include <ctype.h>

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/************************************************************************/

struct speed
{
    long    speed;
    int     code;
};

static struct speed speedtab[] =
{
    { 50, B50 },
    { 75, B75 },
    { 110, B110 },
    { 134, B134 },
    { 150, B150 },
    { 200, B200 },
    { 300, B300 },
    { 600, B600 },
    { 1200, B1200 },
    { 1800, B1800 },
    { 2400, B2400 },
    { 4800, B4800 },
    { 9600, B9600 },
#ifdef	B19200
    { 19200, B19200 },
#endif
#ifdef	B38400
    { 38400, B38400 },
#endif
#ifdef	B57600
    { 57600, B57600 },
#endif
#ifdef	B115200
    { 115200, B115200 },
#endif
    { 0, B0 },
    { -1, -1 }
};

static int speed_to_code(long speed)
{
    struct speed *sp;

    for (sp = speedtab; sp->speed != -1; sp++)
	if (sp->speed == speed)
	    break;

    return sp->code;
}

static long code_to_speed(int code)
{
    struct speed *sp;

    for (sp = speedtab; sp->speed != -1; sp++)
	if (sp->code == code)
	    break;

    return sp->speed;
}

/************************************************************************/

static const char *term_name;
static int term_fd;
static int log_fd;

static struct termios stdin_termios;
static struct termios stdout_termios;

static int escape_char = 28;		/* Ctrl-\ */
static int break_duration = 5;		/* break is 0.5 seconds long */
static int flag_nlcr = 0;	/* Translate NL to CRNL */
static int flag_hex = 0; 	/* Show hex */

/************************************************************************/

static void setup_tty(void)
{
    struct termios termios;

    if (tcgetattr(0, &stdin_termios) == -1)
    {
	perror("tcgetattr stdin");
	exit(1);
    }

    if (tcgetattr(1, &stdout_termios) == -1)
    {
	perror("tcgetattr stdout");
	exit(1);
    }

    memcpy(&termios, &stdin_termios, sizeof(termios));
    cfmakeraw(&termios);

    termios.c_oflag = OPOST;
    if (flag_nlcr)
	termios.c_oflag |= ONLCR;

    if (tcsetattr(0, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr stdin");
	exit(1);
    }

    if (tcsetattr(1, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr stdin");
	exit(1);
    }
}

static void restore_tty(void)
{
    if (tcsetattr(0, TCSANOW, &stdin_termios) == -1)
    {
	perror("tcsetattr stdin");
	exit(1);
    }

    if (tcsetattr(1, TCSANOW, &stdout_termios) == -1)
    {
	perror("tcsetattr stdin");
	exit(1);
    }
}

static int setup_term(int fd)
{
    struct termios termios;

    if (tcgetattr(fd, &termios) == -1)
    {
	perror("tcgetattr");
	return -1;
    }

    /* Set up tty */
    termios.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
			    |INLCR|IGNCR|ICRNL|IXON);
    termios.c_oflag &= ~OPOST;
    termios.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    termios.c_cflag &= ~(CSIZE|PARENB|CSTOPB);
    termios.c_cflag |= CS8 | CREAD;

    if (tcsetattr(fd, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr");
	return -1;
    }

    return 0;
}

/************************************************************************/

static int fuzzy(const char *pattern, char *input, char **args)
{
    while (*pattern)
    {
	/* if there are more words in the pattern but the input ends
           here, this means we have matched the beginning */
	if (!*input)
	    break;

	/* try to match the input against the beginning of a pattern word */
	while (*input && !isspace(*input) && tolower(*input) == tolower(*pattern))
	{
	    ++input;
	    ++pattern;
	}

	/* check that the input word ends here, if it doesn't the
           words don't match */
	if (*input && !isspace(*input))
	    return 0;

	/* skip the whitespace of the input */
	while (*input && isspace(*input))
	    ++input;

	/* ship to the next pattern word */
	while (*pattern && !isspace(*pattern))
	    ++pattern;
	while (*pattern && isspace(*pattern))
	    ++pattern;
    }

    *args = input;
    return 1;
}

/************************************************************************/

static int do_help(char *args, int extra);
static int do_set_help(char *args, int extra);
static int do_quit(char *args, int extra);

/************************************************************************/

static int do_connect(char *args, int extra)
{
    fd_set readfds;
    int fd_limit;
    int r;
    int escape_seen = 0;
    char bell = '\a';
    int term_close = 1;
    struct timeval tv;

reconnect:
    if (!term_name)
    {
	fprintf(stderr, "No port selected\n");
	return 0;
    }

    if (term_fd == -1)
	fprintf(stderr, "\nTrying to reconnect to \"%s\"\n", term_name);
    else
	fprintf(stderr, "Connected, press \\%03o C to quit\n", escape_char);
    setup_tty();

    while (1)
    {
	if (term_fd == -1)
	    fd_limit = 0 + 1;
	else
	    fd_limit = term_fd + 1;

	FD_ZERO(&readfds);
	FD_SET(0, &readfds);
	FD_SET(term_fd, &readfds);
	tv.tv_sec = 1;
	tv.tv_usec = 0;

	if ((r = select(fd_limit, &readfds, NULL, NULL, &tv)) < 0)
	{
	    perror("select");
	    break;
	}

	if (FD_ISSET(0, &readfds))
	{
	    char c;
	    int n = read(0, &c, 1);
	    if (n < 0)
	    {
		perror("read stdin");
		break;
	    }
	    if (n == 0)
	    {
		fprintf(stderr, "read stdin: EOF\n");
		break;
	    }

	    /* handle commands */
	    if (escape_seen)
	    {
		escape_seen = 0;

		if (c == escape_char)
		{
		    if (term_fd != -1)
		    {
			n = write(term_fd, &c, 1);
			if (n < 0)
			{
			    perror("write term_fd");
			    break;
			}
			if (n == 0)
			{
			    fprintf(stderr, "write term_fd: buffer full?\n");
			    break;
			}
			break;
		    }
		}
		else /* if (c == escape_char) */
		{
		    switch (tolower(c))
		    {
		    case 'h':
		    case '?':
			restore_tty();
			printf("\n"
			 "\\%03o\tSend \\%03o\n"
			     "h or ?\tShow this help message\n"
			     "!\tStart a shell\n"
			     "c\tReturn to the command line\n"
			     "q\tQuit\n"
			     "Command> ", escape_char, escape_char);
			fflush(stdout);
			setup_tty();
			escape_seen = 1;
			break;

		    case '!':
			restore_tty();
			puts("\nStarting a shell");
			system(getenv("SHELL"));
			puts("\nBack at the terminal");
			setup_tty();
			break;

		    case 'c':
			term_close = 0;
			goto do_close;

		    case 'b':
		    {
			printf("break\n");
			if (tcsendbreak(term_fd, break_duration) == -1)
			    perror("break");
			printf("break done\n");
			break;
		    }

		    case 'q':
		    {
			restore_tty();

			do_quit("", 0);
			exit(0);
		    }

		    default:
			write(1, &bell, 1);
			break;
		    }
		}
	    }
	    else if (c == escape_char)
		escape_seen = 1;
	    else
	    {
	    retry_write:
		if (term_fd != -1)
		{
		    n = write(term_fd, &c, 1);
		    if (n < 0)
		    {
			if (errno == EAGAIN)
			{
			    usleep(1);
			    goto retry_write;
			}
			fprintf(stderr, "write term_fd: %s (%d)\n",
				strerror(errno), errno);
			break;
		    }
		    if (n == 0)
		    {
			fprintf(stderr, "write term_fd: buffer full?\n");
			break;
		    }
		}
	    }
	}

	if (term_fd == -1)
	{
	    if ((term_fd = open(term_name, O_RDWR | O_NONBLOCK)) != -1)
	    {
		setup_term(term_fd);
		restore_tty();
		fprintf(stderr, "Connected, press \\%03o C to quit\n", escape_char);
		setup_tty();
	    }
	} 
	else if (FD_ISSET(term_fd, &readfds))
	{
	    char buf[1024];
	    int n = read(term_fd, buf, sizeof(buf));
	    if (n < 0)
	    {
		perror("read term_fd");
		break;
	    }
	    if (n == 0)
	    {
		fprintf(stderr, "read term_fd: EOF\n");
		break;
	    }
	    if (write(1, buf, n) != n)
	    {
		perror("write stdout");
		break;
	    }
	    if (log_fd)
		write(log_fd, buf, n);
	    if (flag_hex)
	    {
		int i;
		char s[16];
		for (i = 0; i < n; i++)
		{
		    sprintf(s, "[%02x]", buf[i]);
		    write(1, s, 4);
		}
		write(1, "\r\n", 2);
	    }
	}
    }

do_close:
    restore_tty();

    if (term_close)
    {
	if (term_fd != -1)
	{
	    close(term_fd);
	    term_fd = -1;

	    goto reconnect;
	}
    }

    fprintf(stderr, "\nBack at command prompt\n");

    return 1;
}

/************************************************************************/

static int do_log(char *args, int extra)
{
    char *fn;
    int flags;

    if (!*args || *args == '?')
    {
	fprintf(stderr, "Usage: log overwrite|append|stop <filename>\n");
	return 0;
    }

    if (log_fd == -1 && fuzzy("stop", args, &fn))
    {
	printf("No log active\n");
	return 1;
    }

    if (log_fd != -1)
    {
	fprintf(stderr, "Logging stopped\n");
	close(log_fd);
	log_fd = -1;
    }

    if (fuzzy("stop", args, &fn))
	return 1;

    if (!fn)
	fn = "tt.log";

    if (fuzzy("overwrite", args, &fn))
	flags = O_CREAT | O_TRUNC | O_WRONLY;
    else if (fuzzy("append", args, &fn))
	flags = O_CREAT | O_APPEND | O_WRONLY;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"log ?\" for help\n");
	return 0;
    }

    log_fd = open(fn, flags, 0777);
    if (log_fd == -1)
    {
	fprintf(stderr,
		"failed to open \"%s\" for logging: %s\n",
		fn, strerror(errno));
	return 0;
    }

    fprintf(stderr, "Logging started to \"%s\"\n", fn);

    return 1;
}

/************************************************************************/

static int do_quit(char *args, int extra)
{
    if (term_fd != -1)
	close(term_fd);

    if (log_fd != -1)
    {
	printf("Logging stopped\n");
	close(log_fd);
    }

    printf("Bye!\n");
    exit(1);
}

/************************************************************************/

static int do_set_break(char *args, int extra)
{
    long t;
    char *p;

    if (!*args || *args == '?')
    {
	fprintf(stderr,
		"Usage: set break <duration>\n"
		"Where duration is in 1/10 seconds from 1 to 50\n");
	return 0;
    }

    t = strtol(args, &p, 10);
    while (*p && isspace(*p))
	++p;

    if (*p || t < 1)
    {
	fprintf(stderr,
		"Invalid parameter, try \"set break ?\" for help\n");
	return 0;
    }

    break_duration = t;

    return 1;
}

/************************************************************************/

static int do_set_escape(char *args, int extra)
{
    long c;
    char *p;

    if (!*args || *args == '?')
    {
	fprintf(stderr,
		"Usage: set escape <character>\n"
	       "Where character is an ASCII value from 0 to 255\n");
	return 0;
    }

    c = strtoul(args, &p, 10);
    while (*p && isspace(*p))
	++p;

    if (*p || c < 0 || c > 255)
    {
	fprintf(stderr, "Invalid parameter, try \"set escape ?\" for help\n");
	return 0;
    }

    escape_char = c;

    return 1;
}

/************************************************************************/

static int do_set_flow(char *args, int extra)
{
    struct termios termios;
    char *space;
    int flags;

    if (!*args || *args == '?')
    {
	fprintf(stderr, "Usage: set flow rtscts|none\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (strncasecmp(args, "none", space-args) == 0)
	flags = 0;
    else if (strncasecmp(args, "rtscts", space-args) == 0)
	flags = CRTSCTS;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set flow ?\" for help\n");
	return 0;
    }

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (tcgetattr(term_fd, &termios) == -1)
    {
	perror("tcgetattr");
	return 0;
    }

    termios.c_cflag &= ~CRTSCTS;
    termios.c_cflag |= flags;

    if (tcsetattr(term_fd, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_set_nlcr(char *args, int extra)
{
    char *space;

    if (!*args || *args == '?')
    {
	fprintf(stderr, "Usage: set nlcr on|off\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (space-args > 1 && strncasecmp(args, "on", space-args) == 0)
	flag_nlcr = 1;
    else if (space-args > 1 && strncasecmp(args, "off", space-args) == 0)
	flag_nlcr = 0;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set nlcr ?\" for help\n");
	return 0;
    }

    return 1;
}

/************************************************************************/


static int do_set_modem(char *args, int extra)
{
    struct termios termios;
    char *space;
    int flags;

    if (!*args || *args == '?')
    {
	fprintf(stderr, "Usage: set modem on|off\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (space-args > 1 && strncasecmp(args, "on", space-args) == 0)
	flags = HUPCL;
    else if (space-args > 1 && strncasecmp(args, "off", space-args) == 0)
	flags = CLOCAL;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set modem ?\" for help\n");
	return 0;
    }

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (tcgetattr(term_fd, &termios) == -1)
    {
	perror("tcgetattr");
	return 0;
    }

    termios.c_cflag &= ~(CLOCAL|HUPCL);
    termios.c_cflag |= flags;

    if (tcsetattr(term_fd, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_set_hex(char *args, int extra)
{
    struct termios termios;
    char *space;
    int flags;

    if (!*args || *args == '?')
    {
	fprintf(stderr, "Usage: set hex on|off\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (space-args > 1 && strncasecmp(args, "on", space-args) == 0)
	flags = HUPCL;
    else if (space-args > 1 && strncasecmp(args, "off", space-args) == 0)
	flags = CLOCAL;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set hex ?\" for help\n");
	return 0;
    }

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (tcgetattr(term_fd, &termios) == -1)
    {
	perror("tcgetattr");
	return 0;
    }

    termios.c_cflag &= ~(CLOCAL|HUPCL);
    termios.c_cflag |= flags;

    if (tcsetattr(term_fd, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_set_port(char *args, int extra)
{
    if (!*args || *args == '?')
    {
	printf("Usage: set port <device>\n");
	return 0;
    }

    free((void *)term_name);
    term_name = NULL;
    setenv("TT_PORT", "", 1);

    if (term_fd != -1)
	close(term_fd);

    if ((term_fd = open(args, O_RDWR | O_NONBLOCK)) == -1)
    {
	fprintf(stderr, "failed to open %s: %s\n", args, strerror(errno));
	return 0;
    }

    term_name = strdup(args);
    setenv("TT_PORT", args, 1);

    setup_term(term_fd);

    return 1;
}

/************************************************************************/

static int do_set_rts(char *args, int extra)
{
    int flags;
    char *space;

    if (!*args || *args == '?')
    {
	printf("Usage: set rts on/off\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (ioctl(term_fd, TIOCMGET, &flags) == -1) {
	perror("TIOCMGET");
	return 0;
    }

    if (space-args > 1 && strncasecmp(args, "on", space-args) == 0)
	flags |= TIOCM_RTS;
    else if (space-args > 1 && strncasecmp(args, "off", space-args) == 0)
	flags &= ~TIOCM_RTS;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set rts ?\" for help\n");
	return 0;
    }

    if (ioctl(term_fd, TIOCMSET, &flags) == -1) {
	perror("TIOCMSET");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_set_dtr(char *args, int extra)
{
    int flags;
    char *space;

    if (!*args || *args == '?')
    {
	printf("Usage: set dtr on/off\n");
	return 0;
    }

    space = args;
    while (*space && !isspace(*space))
	++space;

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (ioctl(term_fd, TIOCMGET, &flags) == -1) {
	perror("TIOCMGET");
	return 0;
    }

    if (space-args > 1 && strncasecmp(args, "on", space-args) == 0)
	flags |= TIOCM_DTR;
    else if (space-args > 1 && strncasecmp(args, "off", space-args) == 0)
	flags &= ~TIOCM_DTR;
    else
    {
	fprintf(stderr, "Invalid parameter, try \"set dtr ?\" for help\n");
	return 0;
    }

    if (ioctl(term_fd, TIOCMSET, &flags) == -1) {
	perror("TIOCMSET");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_set_speed(char *args, int extra)
{
    struct termios termios;
    long speed;
    int speedcode;
    char *p;

    if (!*args || *args == '?')
    {
	struct speed *sp;
	printf("Usage: set speed <speed>\n"
	       "Where speed is one of: ");
	for (sp = speedtab; sp->speed != 0; ++sp)
	{
	    if (sp != speedtab)
		putchar(',');
	    if ((sp - speedtab) % 10 == 0)
		printf("\n    ");
	    else
		putchar(' ');
	    printf("%ld", sp->speed);
	}
	putchar('\n');
	return 0;
    }

    speed = strtol(args, &p, 10);
    while (*p && isspace(*p))
	++p;

    if (*p || (speedcode = speed_to_code(speed)) == -1)
    {
	fprintf(stderr, "Invalid parameter, try \"set speed ?\" for help\n");
	return 0;
    }

    if (term_fd == -1)
    {
	printf("No port selected\n");
	return 0;
    }

    if (tcgetattr(term_fd, &termios) == -1)
    {
	perror("tcgetattr");
	return 0;
    }

    cfsetospeed(&termios, speedcode);
    cfsetispeed(&termios, speedcode);

    if (tcsetattr(term_fd, TCSANOW, &termios) == -1)
    {
	perror("tcsetattr");
	return 0;
    }

    return 1;
}

/************************************************************************/

static int do_shell(char *args, int extra)
{
    if (*args)
	return system(args) == 0;
    else
	return system(getenv("SHELL")) == 0;
}

/************************************************************************/

static int do_show(char *args, int extra)
{
    struct termios termios;
    long speed;

    printf("global settings:\n");
    printf("    break-duration: %d (1/10 seconds)\n", break_duration);
    printf("    escape-char: %d\n", escape_char);
    printf("\n");

    printf("port settings:\n");
    if (term_fd == -1)
	printf("    no port selected\n");
    else
    {
	if (tcgetattr(term_fd, &termios) == -1)
	{
	    perror("tcgetattr");
	    return 0;
	}

	speed = code_to_speed(cfgetispeed(&termios));
	if (speed == -1)
	    printf("    speed:  unknown\n");
	else
	    printf("    speed:  %ld\n", speed);

	if (termios.c_cflag & CRTSCTS)
	    printf("    flow:   rtscts\n");
	else
	    printf("    flow:   none\n");

	if (termios.c_cflag & CLOCAL)
	    printf("    modem:  off\n");
	else
	    printf("    modem:  on\n");
    }
    printf("\n");

    return 1;
}

/************************************************************************/

struct command
{
    const char *name;
    int (*func)(char *args, int extra);
    const char *help;
};

static struct command commands[] =
{
    { "connect",	do_connect,	"connect" },
    { "help",		do_help,	"help or ?" },
    { "log",		do_log,		"log overwrite|append|stop [filename]" },
    { "quit",		do_quit,	"quit" },
    { "set ?",		do_set_help,	NULL },
    { "set break",	do_set_break,	"set break <duration>" },
    { "set escape",	do_set_escape,	"set escape <character>" },
    { "set flow",	do_set_flow,	"set flow rtscts|none" },
    { "set hex",	do_set_hex,	"set hex on|off" },
    { "set modem",	do_set_modem,	"set modem on|off" },
    { "set nlcr",	do_set_nlcr,	"set speed on|off" },
    { "set port",	do_set_port,	"set port <device>" },
    { "set rts",	do_set_rts,	"set rts on|off" },
    { "set dtr",	do_set_dtr,	"set dtr on|off" },
    { "set speed",	do_set_speed,	"set speed <speed>" },
    { "shell",		do_shell,	"shell [command] or ![command]" },
    { "show",		do_show,	"show" },

    { NULL },
};

static int do_help(char *args, int extra)
{
    struct command *cmd;
    char *dummy;

    for (cmd = commands; cmd->name; cmd++)
	if (fuzzy(cmd->name, args, &dummy) && cmd->help)
	    printf("    %s\n", cmd->help);
    printf("\n");
    return 0;
}

static int do_set_help(char *args, int extra)
{
    struct command *cmd;
    char *dummy;

    for (cmd = commands; cmd->name; cmd++)
	if (fuzzy(cmd->name, "set", &dummy) && cmd->help)
	    printf("    %s\n", cmd->help);
    printf("\n");
    return 0;
}

static int handle(char *s)
{
    char *p;
    struct command *cmd;
    struct command *match;
    char *args;

    /* strip whitespace on the left */
    while (*s && isspace(*s))
	s++;

    /* strip whitespace on the right */
    p = s + strlen(s) - 1;
    while (p > s && isspace(*p))
	*p-- = '\0';

    /* no command */
    if (!*s)
	return 1;

    /* special commands */
    if (*s == '?')
	return do_help(s+1, 0);

    if (*s == '!')
	return do_shell(s+1, 0);

    match = NULL;
    args = NULL;
    for (cmd = commands; cmd->name; cmd++)
    {
	if (fuzzy(cmd->name, s, &args))
	{
	    if (match)
	    {
		/* already have a match, so this is ambiguous */
		printf("ambiguous command, the following commands match:\n");
		for (cmd = commands; cmd->name; cmd++)
		    if (fuzzy(cmd->name, s, &args) && cmd->help)
			printf("    %s\n", cmd->help);
		printf("\n");
		return 0;
	    }
	    match = cmd;
	}
    }

    if (match)
	return match->func(args, 0);

    printf("unknown command '%s'\n", s);

    return 0;
}

static int script(const char *name)
{
    FILE *fp;
    char fn[FILENAME_MAX];
    char s[256];
    int line;

    snprintf(fn, sizeof(fn)-1, "%s/.tt/%s", getenv("HOME"), name);
    fn[sizeof(fn)-1] = '\0';

    if ((fp = fopen(fn, "r")) == NULL)
    {
	fprintf(stderr, "could not run script \"%s\": %s\n",
		fn, strerror(errno));
	return 0;
    }

    fprintf(stderr, "Running script \"%s\"\n", fn);

    line = 0;
    while (fgets(s, sizeof(s), fp) != NULL)
    {
	line++;
	fputs(s, stdout);
	if (!handle(s))
	{
	    fprintf(stderr, "%s: error at line %d, aborting script\n", name, line);
	    fclose(fp);
	    return 0;
	}
    }

    fclose(fp);

    return 1;
}

int main(int argc, char *argv[])
{
    char s[256];

    term_fd = -1;
    log_fd = -1;

    setenv("TT_PORT", "", 1);

    if (argc > 2)
    {
	printf("Usage: tt [script name]\n");
	exit(1);
    }

    if (argc > 1)
	script(argv[1]);

    printf("> ");
    fflush(stdout);
    while (fgets(s, sizeof(s), stdin) != NULL)
    {
	handle(s);

	printf("> ");
	fflush(stdout);
    }

    do_quit("", 0);
    exit(0);
}
