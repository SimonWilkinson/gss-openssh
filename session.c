/*
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 */
/*
 * SSH2 support by Markus Friedl.
 * Copyright (c) 2000 Markus Friedl. All rights reserved.
 */

#include "includes.h"
RCSID("$OpenBSD: session.c,v 1.20 2000/06/18 04:42:54 markus Exp $");

#include "xmalloc.h"
#include "ssh.h"
#include "pty.h"
#include "packet.h"
#include "buffer.h"
#include "cipher.h"
#include "mpaux.h"
#include "servconf.h"
#include "uidswap.h"
#include "compat.h"
#include "channels.h"
#include "nchan.h"

#include "bufaux.h"
#include "ssh2.h"
#include "auth.h"
#include "auth-options.h"

/* types */

#define TTYSZ 64
typedef struct Session Session;
struct Session {
	int	used;
	int	self;
	int	extended;
	struct	passwd *pw;
	pid_t	pid;
	/* tty */
	char	*term;
	int	ptyfd, ttyfd, ptymaster;
	int	row, col, xpixel, ypixel;
	char	tty[TTYSZ];
	/* X11 */
	char	*display;
	int	screen;
	char	*auth_proto;
	char	*auth_data;
	int	single_connection;
	/* proto 2 */
	int	chanid;
};

/* func */

Session *session_new(void);
void	session_set_fds(Session *s, int fdin, int fdout, int fderr);
void	session_pty_cleanup(Session *s);
void	session_proctitle(Session *s);
void	do_exec_pty(Session *s, const char *command, struct passwd * pw);
void	do_exec_no_pty(Session *s, const char *command, struct passwd * pw);

void
do_child(const char *command, struct passwd * pw, const char *term,
    const char *display, const char *auth_proto,
    const char *auth_data, const char *ttyname);

/* import */
extern ServerOptions options;
#ifdef HAVE___PROGNAME
extern char *__progname;
#else /* HAVE___PROGNAME */
static const char *__progname = "sshd";
#endif /* HAVE___PROGNAME */

extern int log_stderr;
extern int debug_flag;

/* Local Xauthority file. */
static char *xauthfile;

/* data */
#define MAX_SESSIONS 10
Session	sessions[MAX_SESSIONS];
#ifdef WITH_AIXAUTHENTICATE
/* AIX's lastlogin message, set in auth1.c */
char *aixloginmsg;
#endif /* WITH_AIXAUTHENTICATE */

/*
 * Remove local Xauthority file.
 */
void
xauthfile_cleanup_proc(void *ignore)
{
	debug("xauthfile_cleanup_proc called");

	if (xauthfile != NULL) {
		char *p;
		unlink(xauthfile);
		p = strrchr(xauthfile, '/');
		if (p != NULL) {
			*p = '\0';
			rmdir(xauthfile);
		}
		xfree(xauthfile);
		xauthfile = NULL;
	}
}

/*
 * Function to perform cleanup if we get aborted abnormally (e.g., due to a
 * dropped connection).
 */
void
pty_cleanup_proc(void *session)
{
	Session *s=session;
	if (s == NULL)
		fatal("pty_cleanup_proc: no session");
	debug("pty_cleanup_proc: %s", s->tty);

	if (s->pid != 0) {
		/* Record that the user has logged out. */
		record_logout(s->pid, s->tty);
	}

	/* Release the pseudo-tty. */
	pty_release(s->tty);
}

/*
 * Prepares for an interactive session.  This is called after the user has
 * been successfully authenticated.  During this message exchange, pseudo
 * terminals are allocated, X11, TCP/IP, and authentication agent forwardings
 * are requested, etc.
 */
void
do_authenticated(struct passwd * pw)
{
	Session *s;
	int type;
	int compression_level = 0, enable_compression_after_reply = 0;
	int have_pty = 0;
	char *command;
	int n_bytes;
	int plen;
	unsigned int proto_len, data_len, dlen;

	/*
	 * Cancel the alarm we set to limit the time taken for
	 * authentication.
	 */
	alarm(0);

	/*
	 * Inform the channel mechanism that we are the server side and that
	 * the client may request to connect to any port at all. (The user
	 * could do it anyway, and we wouldn\'t know what is permitted except
	 * by the client telling us, so we can equally well trust the client
	 * not to request anything bogus.)
	 */
	if (!no_port_forwarding_flag)
		channel_permit_all_opens();

	s = session_new();
	s->pw = pw;

	/*
	 * We stay in this loop until the client requests to execute a shell
	 * or a command.
	 */
	for (;;) {
		int success = 0;

		/* Get a packet from the client. */
		type = packet_read(&plen);

		/* Process the packet. */
		switch (type) {
		case SSH_CMSG_REQUEST_COMPRESSION:
			packet_integrity_check(plen, 4, type);
			compression_level = packet_get_int();
			if (compression_level < 1 || compression_level > 9) {
				packet_send_debug("Received illegal compression level %d.",
				     compression_level);
				break;
			}
			/* Enable compression after we have responded with SUCCESS. */
			enable_compression_after_reply = 1;
			success = 1;
			break;

		case SSH_CMSG_REQUEST_PTY:
			if (no_pty_flag) {
				debug("Allocating a pty not permitted for this authentication.");
				break;
			}
			if (have_pty)
				packet_disconnect("Protocol error: you already have a pty.");

			debug("Allocating pty.");

			/* Allocate a pty and open it. */
			if (!pty_allocate(&s->ptyfd, &s->ttyfd, s->tty,
			    sizeof(s->tty))) {
				error("Failed to allocate pty.");
				break;
			}
			fatal_add_cleanup(pty_cleanup_proc, (void *)s);
			pty_setowner(pw, s->tty);

			/* Get TERM from the packet.  Note that the value may be of arbitrary length. */
			s->term = packet_get_string(&dlen);
			packet_integrity_check(dlen, strlen(s->term), type);
			/* packet_integrity_check(plen, 4 + dlen + 4*4 + n_bytes, type); */
			/* Remaining bytes */
			n_bytes = plen - (4 + dlen + 4 * 4);

			if (strcmp(s->term, "") == 0) {
				xfree(s->term);
				s->term = NULL;
			}
			/* Get window size from the packet. */
			s->row = packet_get_int();
			s->col = packet_get_int();
			s->xpixel = packet_get_int();
			s->ypixel = packet_get_int();
			pty_change_window_size(s->ptyfd, s->row, s->col, s->xpixel, s->ypixel);

			/* Get tty modes from the packet. */
			tty_parse_modes(s->ttyfd, &n_bytes);
			packet_integrity_check(plen, 4 + dlen + 4 * 4 + n_bytes, type);

			session_proctitle(s);

			/* Indicate that we now have a pty. */
			success = 1;
			have_pty = 1;
			break;

		case SSH_CMSG_X11_REQUEST_FORWARDING:
			if (!options.x11_forwarding) {
				packet_send_debug("X11 forwarding disabled in server configuration file.");
				break;
			}
			if (!options.xauth_location) {
				packet_send_debug("No xauth program; cannot forward with spoofing.");
				break;
			}
			if (no_x11_forwarding_flag) {
				packet_send_debug("X11 forwarding not permitted for this authentication.");
				break;
			}
			debug("Received request for X11 forwarding with auth spoofing.");
			if (s->display != NULL)
				packet_disconnect("Protocol error: X11 display already set.");

			s->auth_proto = packet_get_string(&proto_len);
			s->auth_data = packet_get_string(&data_len);
			packet_integrity_check(plen, 4 + proto_len + 4 + data_len + 4, type);

			if (packet_get_protocol_flags() & SSH_PROTOFLAG_SCREEN_NUMBER)
				s->screen = packet_get_int();
			else
				s->screen = 0;
			s->display = x11_create_display_inet(s->screen, options.x11_display_offset);

			if (s->display == NULL)
				break;

			/* Setup to always have a local .Xauthority. */
			xauthfile = xmalloc(MAXPATHLEN);
			strlcpy(xauthfile, "/tmp/ssh-XXXXXXXX", MAXPATHLEN);
			temporarily_use_uid(pw->pw_uid);
			if (mkdtemp(xauthfile) == NULL) {
				restore_uid();
				error("private X11 dir: mkdtemp %s failed: %s",
				    xauthfile, strerror(errno));
				xfree(xauthfile);
				xauthfile = NULL;
				/* XXXX remove listening channels */
				break;
			}
			strlcat(xauthfile, "/cookies", MAXPATHLEN);
			open(xauthfile, O_RDWR|O_CREAT|O_EXCL, 0600);
			restore_uid();
			fatal_add_cleanup(xauthfile_cleanup_proc, NULL);
			success = 1;
			break;

		case SSH_CMSG_AGENT_REQUEST_FORWARDING:
			if (no_agent_forwarding_flag || compat13) {
				debug("Authentication agent forwarding not permitted for this authentication.");
				break;
			}
			debug("Received authentication agent forwarding request.");
			success = auth_input_request_forwarding(pw);
			break;

		case SSH_CMSG_PORT_FORWARD_REQUEST:
			if (no_port_forwarding_flag) {
				debug("Port forwarding not permitted for this authentication.");
				break;
			}
			debug("Received TCP/IP port forwarding request.");
			channel_input_port_forward_request(pw->pw_uid == 0, options.gateway_ports);
			success = 1;
			break;

		case SSH_CMSG_MAX_PACKET_SIZE:
			if (packet_set_maxsize(packet_get_int()) > 0)
				success = 1;
			break;

		case SSH_CMSG_EXEC_SHELL:
		case SSH_CMSG_EXEC_CMD:
			/* Set interactive/non-interactive mode. */
			packet_set_interactive(have_pty || s->display != NULL,
			    options.keepalives);

			if (type == SSH_CMSG_EXEC_CMD) {
				command = packet_get_string(&dlen);
				debug("Exec command '%.500s'", command);
				packet_integrity_check(plen, 4 + dlen, type);
			} else {
				command = NULL;
				packet_integrity_check(plen, 0, type);
			}
			if (forced_command != NULL) {
				command = forced_command;
				debug("Forced command '%.500s'", forced_command);
			}
			if (have_pty)
				do_exec_pty(s, command, pw);
			else
				do_exec_no_pty(s, command, pw);

			if (command != NULL)
				xfree(command);
			/* Cleanup user's local Xauthority file. */
			if (xauthfile)
				xauthfile_cleanup_proc(NULL);
			return;

		default:
			/*
			 * Any unknown messages in this phase are ignored,
			 * and a failure message is returned.
			 */
			log("Unknown packet type received after authentication: %d", type);
		}
		packet_start(success ? SSH_SMSG_SUCCESS : SSH_SMSG_FAILURE);
		packet_send();
		packet_write_wait();

		/* Enable compression now that we have replied if appropriate. */
		if (enable_compression_after_reply) {
			enable_compression_after_reply = 0;
			packet_start_compression(compression_level);
		}
	}
}

/*
 * This is called to fork and execute a command when we have no tty.  This
 * will call do_child from the child, and server_loop from the parent after
 * setting up file descriptors and such.
 */
void
do_exec_no_pty(Session *s, const char *command, struct passwd * pw)
{
	int pid;

#ifdef USE_PIPES
	int pin[2], pout[2], perr[2];
	/* Allocate pipes for communicating with the program. */
	if (pipe(pin) < 0 || pipe(pout) < 0 || pipe(perr) < 0)
		packet_disconnect("Could not create pipes: %.100s",
				  strerror(errno));
#else /* USE_PIPES */
	int inout[2], err[2];
	/* Uses socket pairs to communicate with the program. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, inout) < 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, err) < 0)
		packet_disconnect("Could not create socket pairs: %.100s",
				  strerror(errno));
#endif /* USE_PIPES */
	if (s == NULL)
		fatal("do_exec_no_pty: no session");

	session_proctitle(s);

#ifdef USE_PAM
			do_pam_setcred();
#endif /* USE_PAM */

	/* Fork the child. */
	if ((pid = fork()) == 0) {
		/* Child.  Reinitialize the log since the pid has changed. */
		log_init(__progname, options.log_level, options.log_facility, log_stderr);

		/*
		 * Create a new session and process group since the 4.4BSD
		 * setlogin() affects the entire process group.
		 */
		if (setsid() < 0)
			error("setsid failed: %.100s", strerror(errno));

#ifdef USE_PIPES
		/*
		 * Redirect stdin.  We close the parent side of the socket
		 * pair, and make the child side the standard input.
		 */
		close(pin[1]);
		if (dup2(pin[0], 0) < 0)
			perror("dup2 stdin");
		close(pin[0]);

		/* Redirect stdout. */
		close(pout[0]);
		if (dup2(pout[1], 1) < 0)
			perror("dup2 stdout");
		close(pout[1]);

		/* Redirect stderr. */
		close(perr[0]);
		if (dup2(perr[1], 2) < 0)
			perror("dup2 stderr");
		close(perr[1]);
#else /* USE_PIPES */
		/*
		 * Redirect stdin, stdout, and stderr.  Stdin and stdout will
		 * use the same socket, as some programs (particularly rdist)
		 * seem to depend on it.
		 */
		close(inout[1]);
		close(err[1]);
		if (dup2(inout[0], 0) < 0)	/* stdin */
			perror("dup2 stdin");
		if (dup2(inout[0], 1) < 0)	/* stdout.  Note: same socket as stdin. */
			perror("dup2 stdout");
		if (dup2(err[0], 2) < 0)	/* stderr */
			perror("dup2 stderr");
#endif /* USE_PIPES */

		/* Do processing for the child (exec command etc). */
		do_child(command, pw, NULL, s->display, s->auth_proto, s->auth_data, NULL);
		/* NOTREACHED */
	}
	if (pid < 0)
		packet_disconnect("fork failed: %.100s", strerror(errno));
	s->pid = pid;
#ifdef USE_PIPES
	/* We are the parent.  Close the child sides of the pipes. */
	close(pin[0]);
	close(pout[1]);
	close(perr[1]);

	if (compat20) {
		session_set_fds(s, pin[1], pout[0], s->extended ? perr[0] : -1);
	} else {
		/* Enter the interactive session. */
		server_loop(pid, pin[1], pout[0], perr[0]);
		/* server_loop has closed pin[1], pout[1], and perr[1]. */
	}
#else /* USE_PIPES */
	/* We are the parent.  Close the child sides of the socket pairs. */
	close(inout[0]);
	close(err[0]);

	/*
	 * Enter the interactive session.  Note: server_loop must be able to
	 * handle the case that fdin and fdout are the same.
	 */
	if (compat20) {
		session_set_fds(s, inout[1], inout[1], s->extended ? err[1] : -1);
	} else {
		server_loop(pid, inout[1], inout[1], err[1]);
		/* server_loop has closed inout[1] and err[1]. */
	}
#endif /* USE_PIPES */
}

/*
 * This is called to fork and execute a command when we have a tty.  This
 * will call do_child from the child, and server_loop from the parent after
 * setting up file descriptors, controlling tty, updating wtmp, utmp,
 * lastlog, and other such operations.
 */
void
do_exec_pty(Session *s, const char *command, struct passwd * pw)
{
	FILE *f;
	char buf[100], *time_string;
	char line[256];
	const char *hostname;
	int fdout, ptyfd, ttyfd, ptymaster;
	int quiet_login;
	pid_t pid;
	socklen_t fromlen;
	struct sockaddr_storage from;
	struct stat st;
	time_t last_login_time;

	if (s == NULL)
		fatal("do_exec_pty: no session");
	ptyfd = s->ptyfd;
	ttyfd = s->ttyfd;

	/* Get remote host name. */
	hostname = get_canonical_hostname();

	/*
	 * Get the time when the user last logged in.  Buf will be set to
	 * contain the hostname the last login was from.
	 */
	if (!options.use_login) {
		last_login_time = get_last_login_time(pw->pw_uid, pw->pw_name,
						      buf, sizeof(buf));
	}

#ifdef USE_PAM
			do_pam_session(pw->pw_name, s->tty);
			do_pam_setcred();
#endif /* USE_PAM */

	/* Fork the child. */
	if ((pid = fork()) == 0) {
		pid = getpid();

		/* Child.  Reinitialize the log because the pid has
		   changed. */
		log_init(__progname, options.log_level, options.log_facility, log_stderr);

		/* Close the master side of the pseudo tty. */
		close(ptyfd);

		/* Make the pseudo tty our controlling tty. */
		pty_make_controlling_tty(&ttyfd, s->tty);

		/* Redirect stdin from the pseudo tty. */
		if (dup2(ttyfd, fileno(stdin)) < 0)
			error("dup2 stdin failed: %.100s", strerror(errno));

		/* Redirect stdout to the pseudo tty. */
		if (dup2(ttyfd, fileno(stdout)) < 0)
			error("dup2 stdin failed: %.100s", strerror(errno));

		/* Redirect stderr to the pseudo tty. */
		if (dup2(ttyfd, fileno(stderr)) < 0)
			error("dup2 stdin failed: %.100s", strerror(errno));

		/* Close the extra descriptor for the pseudo tty. */
		close(ttyfd);

/* XXXX ? move to do_child() ??*/
		/*
		 * Get IP address of client.  This is needed because we want
		 * to record where the user logged in from.  If the
		 * connection is not a socket, let the ip address be 0.0.0.0.
		 */
		memset(&from, 0, sizeof(from));
		if (packet_connection_is_on_socket()) {
			fromlen = sizeof(from);
			if (getpeername(packet_get_connection_in(),
			     (struct sockaddr *) & from, &fromlen) < 0) {
				debug("getpeername: %.100s", strerror(errno));
				fatal_cleanup();
			}
		}
		/* Record that there was a login on that terminal. */
		record_login(pid, s->tty, pw->pw_name, pw->pw_uid, hostname,
			     (struct sockaddr *)&from);

		/* Check if .hushlogin exists. */
		snprintf(line, sizeof line, "%.200s/.hushlogin", pw->pw_dir);
		quiet_login = stat(line, &st) >= 0;

#ifdef USE_PAM
		if (!quiet_login)
			print_pam_messages();
#endif /* USE_PAM */

		/*
		 * If the user has logged in before, display the time of last
		 * login. However, don't display anything extra if a command
		 * has been specified (so that ssh can be used to execute
		 * commands on a remote machine without users knowing they
		 * are going to another machine). Login(1) will do this for
		 * us as well, so check if login(1) is used
		 */
		if (command == NULL && last_login_time != 0 && !quiet_login &&
		    !options.use_login) {
			/* Convert the date to a string. */
			time_string = ctime(&last_login_time);
			/* Remove the trailing newline. */
			if (strchr(time_string, '\n'))
				*strchr(time_string, '\n') = 0;
			/* Display the last login time.  Host if displayed
			   if known. */
			if (strcmp(buf, "") == 0)
				printf("Last login: %s\r\n", time_string);
			else
				printf("Last login: %s from %s\r\n", time_string, buf);
		}
		/*
		 * Print /etc/motd unless a command was specified or printing
		 * it was disabled in server options or login(1) will be
		 * used.  Note that some machines appear to print it in
		 * /etc/profile or similar.
		 */
		if (command == NULL && options.print_motd && !quiet_login &&
		    !options.use_login) {
			/* Print /etc/motd if it exists. */
			f = fopen("/etc/motd", "r");
			if (f) {
				while (fgets(line, sizeof(line), f))
					fputs(line, stdout);
				fclose(f);
			}
		}
#if defined(WITH_AIXAUTHENTICATE)
		/*
		 * AIX handles the lastlog info differently.  Display it here.
		 */
		if (command == NULL && aixloginmsg && *aixloginmsg &&
		    !quiet_login && !options.use_login) {
			printf("%s\n", aixloginmsg);
		}
#endif
		/* Do common processing for the child, such as execing the command. */
		do_child(command, pw, s->term, s->display, s->auth_proto,
		    s->auth_data, s->tty);
		/* NOTREACHED */
	}
	if (pid < 0)
		packet_disconnect("fork failed: %.100s", strerror(errno));
	s->pid = pid;

	/* Parent.  Close the slave side of the pseudo tty. */
	close(ttyfd);

	/*
	 * Create another descriptor of the pty master side for use as the
	 * standard input.  We could use the original descriptor, but this
	 * simplifies code in server_loop.  The descriptor is bidirectional.
	 */
	fdout = dup(ptyfd);
	if (fdout < 0)
		packet_disconnect("dup #1 failed: %.100s", strerror(errno));

	/* we keep a reference to the pty master */
	ptymaster = dup(ptyfd);
	if (ptymaster < 0)
		packet_disconnect("dup #2 failed: %.100s", strerror(errno));
	s->ptymaster = ptymaster;

	/* Enter interactive session. */
	if (compat20) {
		session_set_fds(s, ptyfd, fdout, -1);
	} else {
		server_loop(pid, ptyfd, fdout, -1);
		/* server_loop _has_ closed ptyfd and fdout. */
		session_pty_cleanup(s);
	}
}

/*
 * Sets the value of the given variable in the environment.  If the variable
 * already exists, its value is overriden.
 */
void
child_set_env(char ***envp, unsigned int *envsizep, const char *name,
	      const char *value)
{
	unsigned int i, namelen;
	char **env;

	/*
	 * Find the slot where the value should be stored.  If the variable
	 * already exists, we reuse the slot; otherwise we append a new slot
	 * at the end of the array, expanding if necessary.
	 */
	env = *envp;
	namelen = strlen(name);
	for (i = 0; env[i]; i++)
		if (strncmp(env[i], name, namelen) == 0 && env[i][namelen] == '=')
			break;
	if (env[i]) {
		/* Reuse the slot. */
		xfree(env[i]);
	} else {
		/* New variable.  Expand if necessary. */
		if (i >= (*envsizep) - 1) {
			(*envsizep) += 50;
			env = (*envp) = xrealloc(env, (*envsizep) * sizeof(char *));
		}
		/* Need to set the NULL pointer at end of array beyond the new slot. */
		env[i + 1] = NULL;
	}

	/* Allocate space and format the variable in the appropriate slot. */
	env[i] = xmalloc(strlen(name) + 1 + strlen(value) + 1);
	snprintf(env[i], strlen(name) + 1 + strlen(value) + 1, "%s=%s", name, value);
}

/*
 * Reads environment variables from the given file and adds/overrides them
 * into the environment.  If the file does not exist, this does nothing.
 * Otherwise, it must consist of empty lines, comments (line starts with '#')
 * and assignments of the form name=value.  No other forms are allowed.
 */
void
read_environment_file(char ***env, unsigned int *envsize,
		      const char *filename)
{
	FILE *f;
	char buf[4096];
	char *cp, *value;

	f = fopen(filename, "r");
	if (!f)
		return;

	while (fgets(buf, sizeof(buf), f)) {
		for (cp = buf; *cp == ' ' || *cp == '\t'; cp++)
			;
		if (!*cp || *cp == '#' || *cp == '\n')
			continue;
		if (strchr(cp, '\n'))
			*strchr(cp, '\n') = '\0';
		value = strchr(cp, '=');
		if (value == NULL) {
			fprintf(stderr, "Bad line in %.100s: %.200s\n", filename, buf);
			continue;
		}
		/*
		 * Replace the equals sign by nul, and advance value to
		 * the value string.
		 */
		*value = '\0';
		value++;
		child_set_env(env, envsize, cp, value);
	}
	fclose(f);
}

#ifdef USE_PAM
/*
 * Sets any environment variables which have been specified by PAM
 */
void do_pam_environment(char ***env, int *envsize)
{
	char *equals, var_name[512], var_val[512];
	char **pam_env;
	int i;

	if ((pam_env = fetch_pam_environment()) == NULL)
		return;
	
	for(i = 0; pam_env[i] != NULL; i++) {
		if ((equals = strstr(pam_env[i], "=")) == NULL)
			continue;
			
		if (strlen(pam_env[i]) < (sizeof(var_name) - 1)) {
			memset(var_name, '\0', sizeof(var_name));
			memset(var_val, '\0', sizeof(var_val));

			strncpy(var_name, pam_env[i], equals - pam_env[i]);
			strcpy(var_val, equals + 1);

			debug("PAM environment: %s=%s", var_name, var_val);

			child_set_env(env, envsize, var_name, var_val);
		}
	}
}
#endif /* USE_PAM */

/*
 * Performs common processing for the child, such as setting up the
 * environment, closing extra file descriptors, setting the user and group
 * ids, and executing the command or shell.
 */
void
do_child(const char *command, struct passwd * pw, const char *term,
	 const char *display, const char *auth_proto,
	 const char *auth_data, const char *ttyname)
{
	const char *shell, *cp = NULL;
	char buf[256];
	char cmd[1024];
	FILE *f;
	unsigned int envsize, i;
	char **env;
	extern char **environ;
	struct stat st;
	char *argv[10];

	/* login(1) is only called if we execute the login shell */
	if (options.use_login && command != NULL)
		options.use_login = 0;

#ifndef USE_PAM /* pam_nologin handles this */
	f = fopen("/etc/nologin", "r");
	if (f) {
		/* /etc/nologin exists.  Print its contents and exit. */
		while (fgets(buf, sizeof(buf), f))
			fputs(buf, stderr);
		fclose(f);
		if (pw->pw_uid != 0)
			exit(254);
	}
#endif /* USE_PAM */

	/* Set login name in the kernel. */
	if (setlogin(pw->pw_name) < 0)
		error("setlogin failed: %s", strerror(errno));

	/* Set uid, gid, and groups. */
	/* Login(1) does this as well, and it needs uid 0 for the "-h"
	   switch, so we let login(1) to this for us. */
	if (!options.use_login) {
		if (getuid() == 0 || geteuid() == 0) {
			if (setgid(pw->pw_gid) < 0) {
				perror("setgid");
				exit(1);
			}
			/* Initialize the group list. */
			if (initgroups(pw->pw_name, pw->pw_gid) < 0) {
				perror("initgroups");
				exit(1);
			}
			endgrent();

			/* Permanently switch to the desired uid. */
			permanently_set_uid(pw->pw_uid);
		}
		if (getuid() != pw->pw_uid || geteuid() != pw->pw_uid)
			fatal("Failed to set uids to %d.", (int) pw->pw_uid);
	}
	/*
	 * Get the shell from the password data.  An empty shell field is
	 * legal, and means /bin/sh.
	 */
	shell = (pw->pw_shell[0] == '\0') ? _PATH_BSHELL : pw->pw_shell;

#ifdef AFS
	/* Try to get AFS tokens for the local cell. */
	if (k_hasafs()) {
		char cell[64];

		if (k_afs_cell_of_file(pw->pw_dir, cell, sizeof(cell)) == 0)
			krb_afslog(cell, 0);

		krb_afslog(0, 0);
	}
#endif /* AFS */

	/* Initialize the environment. */
	envsize = 100;
	env = xmalloc(envsize * sizeof(char *));
	env[0] = NULL;

	if (!options.use_login) {
		/* Set basic environment. */
		child_set_env(&env, &envsize, "USER", pw->pw_name);
		child_set_env(&env, &envsize, "LOGNAME", pw->pw_name);
		child_set_env(&env, &envsize, "HOME", pw->pw_dir);
		child_set_env(&env, &envsize, "PATH", _PATH_STDPATH);

		snprintf(buf, sizeof buf, "%.200s/%.50s",
			 _PATH_MAILDIR, pw->pw_name);
		child_set_env(&env, &envsize, "MAIL", buf);

		/* Normal systems set SHELL by default. */
		child_set_env(&env, &envsize, "SHELL", shell);
	}
	if (getenv("TZ"))
		child_set_env(&env, &envsize, "TZ", getenv("TZ"));

	/* Set custom environment options from RSA authentication. */
	while (custom_environment) {
		struct envstring *ce = custom_environment;
		char *s = ce->s;
		int i;
		for (i = 0; s[i] != '=' && s[i]; i++);
		if (s[i] == '=') {
			s[i] = 0;
			child_set_env(&env, &envsize, s, s + i + 1);
		}
		custom_environment = ce->next;
		xfree(ce->s);
		xfree(ce);
	}

	snprintf(buf, sizeof buf, "%.50s %d %d",
		 get_remote_ipaddr(), get_remote_port(), get_local_port());
	child_set_env(&env, &envsize, "SSH_CLIENT", buf);

	if (ttyname)
		child_set_env(&env, &envsize, "SSH_TTY", ttyname);
	if (term)
		child_set_env(&env, &envsize, "TERM", term);
	if (display)
		child_set_env(&env, &envsize, "DISPLAY", display);

#ifdef _AIX
	{
           char *authstate,*krb5cc;

	   if ((authstate = getenv("AUTHSTATE")) != NULL)
		 child_set_env(&env,&envsize,"AUTHSTATE",authstate);

	   if ((krb5cc = getenv("KRB5CCNAME")) != NULL)
		 child_set_env(&env,&envsize,"KRB5CCNAME",krb5cc);
	}
#endif

#ifdef KRB4
	{
		extern char *ticket;

		if (ticket)
			child_set_env(&env, &envsize, "KRBTKFILE", ticket);
	}
#endif /* KRB4 */

#ifdef USE_PAM
	/* Pull in any environment variables that may have been set by PAM. */
	do_pam_environment(&env, &envsize);
#endif /* USE_PAM */

	read_environment_file(&env,&envsize,"/etc/environment");

	if (xauthfile)
		child_set_env(&env, &envsize, "XAUTHORITY", xauthfile);
	if (auth_get_socket_name() != NULL)
		child_set_env(&env, &envsize, SSH_AUTHSOCKET_ENV_NAME,
			      auth_get_socket_name());

	/* read $HOME/.ssh/environment. */
	if (!options.use_login) {
		snprintf(buf, sizeof buf, "%.200s/.ssh/environment",
		    pw->pw_dir);
		read_environment_file(&env, &envsize, buf);
	}
	if (debug_flag) {
		/* dump the environment */
		fprintf(stderr, "Environment:\n");
		for (i = 0; env[i]; i++)
			fprintf(stderr, "  %.200s\n", env[i]);
	}
	/*
	 * Close the connection descriptors; note that this is the child, and
	 * the server will still have the socket open, and it is important
	 * that we do not shutdown it.  Note that the descriptors cannot be
	 * closed before building the environment, as we call
	 * get_remote_ipaddr there.
	 */
	if (packet_get_connection_in() == packet_get_connection_out())
		close(packet_get_connection_in());
	else {
		close(packet_get_connection_in());
		close(packet_get_connection_out());
	}
	/*
	 * Close all descriptors related to channels.  They will still remain
	 * open in the parent.
	 */
	/* XXX better use close-on-exec? -markus */
	channel_close_all();

	/*
	 * Close any extra file descriptors.  Note that there may still be
	 * descriptors left by system functions.  They will be closed later.
	 */
	endpwent();

	/*
	 * Close any extra open file descriptors so that we don\'t have them
	 * hanging around in clients.  Note that we want to do this after
	 * initgroups, because at least on Solaris 2.3 it leaves file
	 * descriptors open.
	 */
	for (i = 3; i < 64; i++)
		close(i);

	/* Change current directory to the user\'s home directory. */
	if (chdir(pw->pw_dir) < 0)
		fprintf(stderr, "Could not chdir to home directory %s: %s\n",
			pw->pw_dir, strerror(errno));

	/*
	 * Must take new environment into use so that .ssh/rc, /etc/sshrc and
	 * xauth are run in the proper environment.
	 */
	environ = env;

	/*
	 * Run $HOME/.ssh/rc, /etc/sshrc, or xauth (whichever is found first
	 * in this order).
	 */
	if (!options.use_login) {
		if (stat(SSH_USER_RC, &st) >= 0) {
			if (debug_flag)
				fprintf(stderr, "Running /bin/sh %s\n", SSH_USER_RC);

			f = popen("/bin/sh " SSH_USER_RC, "w");
			if (f) {
				if (auth_proto != NULL && auth_data != NULL)
					fprintf(f, "%s %s\n", auth_proto, auth_data);
				pclose(f);
			} else
				fprintf(stderr, "Could not run %s\n", SSH_USER_RC);
		} else if (stat(SSH_SYSTEM_RC, &st) >= 0) {
			if (debug_flag)
				fprintf(stderr, "Running /bin/sh %s\n", SSH_SYSTEM_RC);

			f = popen("/bin/sh " SSH_SYSTEM_RC, "w");
			if (f) {
				if (auth_proto != NULL && auth_data != NULL)
					fprintf(f, "%s %s\n", auth_proto, auth_data);
				pclose(f);
			} else
				fprintf(stderr, "Could not run %s\n", SSH_SYSTEM_RC);
		} else if (options.xauth_location != NULL) {
			/* Add authority data to .Xauthority if appropriate. */
			if (auth_proto != NULL && auth_data != NULL) {
				char *screen = strchr(display, ':');
				if (debug_flag) {
					fprintf(stderr,
					    "Running %.100s add %.100s %.100s %.100s\n",
					    options.xauth_location, display,
					    auth_proto, auth_data);
					if (screen != NULL)
						fprintf(stderr,
						    "Adding %.*s/unix%s %s %s\n",
						    screen-display, display,
						    screen, auth_proto, auth_data);
				}
				snprintf(cmd, sizeof cmd, "%s -q -",
				    options.xauth_location);
				f = popen(cmd, "w");
				if (f) {
					fprintf(f, "add %s %s %s\n", display,
					    auth_proto, auth_data);
					if (screen != NULL) 
						fprintf(f, "add %.*s/unix%s %s %s\n",
						    screen-display, display,
						    screen, auth_proto, auth_data);
					pclose(f);
				} else {
					fprintf(stderr, "Could not run %s\n",
					    cmd);
				}
			}
		}
		/* Get the last component of the shell name. */
		cp = strrchr(shell, '/');
		if (cp)
			cp++;
		else
			cp = shell;
	}
	/*
	 * If we have no command, execute the shell.  In this case, the shell
	 * name to be passed in argv[0] is preceded by '-' to indicate that
	 * this is a login shell.
	 */
	if (!command) {
		if (!options.use_login) {
			char buf[256];

			/*
			 * Check for mail if we have a tty and it was enabled
			 * in server options.
			 */
			if (ttyname && options.check_mail) {
				char *mailbox;
				struct stat mailstat;
				mailbox = getenv("MAIL");
				if (mailbox != NULL) {
					if (stat(mailbox, &mailstat) != 0 ||
					    mailstat.st_size == 0)
						printf("No mail.\n");
					else if (mailstat.st_mtime < mailstat.st_atime)
						printf("You have mail.\n");
					else
						printf("You have new mail.\n");
				}
			}
			/* Start the shell.  Set initial character to '-'. */
			buf[0] = '-';
			strncpy(buf + 1, cp, sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = 0;

			/* Execute the shell. */
			argv[0] = buf;
			argv[1] = NULL;
			execve(shell, argv, env);

			/* Executing the shell failed. */
			perror(shell);
			exit(1);

		} else {
			/* Launch login(1). */

			execl("/usr/bin/login", "login", "-h", get_remote_ipaddr(),
			      "-p", "-f", "--", pw->pw_name, NULL);

			/* Login couldn't be executed, die. */

			perror("login");
			exit(1);
		}
	}
	/*
	 * Execute the command using the user's shell.  This uses the -c
	 * option to execute the command.
	 */
	argv[0] = (char *) cp;
	argv[1] = "-c";
	argv[2] = (char *) command;
	argv[3] = NULL;
	execve(shell, argv, env);
	perror(shell);
	exit(1);
}

Session *
session_new(void)
{
	int i;
	static int did_init = 0;
	if (!did_init) {
		debug("session_new: init");
		for(i = 0; i < MAX_SESSIONS; i++) {
			sessions[i].used = 0;
			sessions[i].self = i;
		}
		did_init = 1;
	}
	for(i = 0; i < MAX_SESSIONS; i++) {
		Session *s = &sessions[i];
		if (! s->used) {
			s->pid = 0;
			s->extended = 0;
			s->chanid = -1;
			s->ptyfd = -1;
			s->ttyfd = -1;
			s->term = NULL;
			s->pw = NULL;
			s->display = NULL;
			s->screen = 0;
			s->auth_data = NULL;
			s->auth_proto = NULL;
			s->used = 1;
			s->pw = NULL;
			debug("session_new: session %d", i);
			return s;
		}
	}
	return NULL;
}

void
session_dump(void)
{
	int i;
	for(i = 0; i < MAX_SESSIONS; i++) {
		Session *s = &sessions[i];
		debug("dump: used %d session %d %p channel %d pid %d",
		    s->used,
		    s->self,
		    s,
		    s->chanid,
		    s->pid);
	}
}

int
session_open(int chanid)
{
	Session *s = session_new();
	debug("session_open: channel %d", chanid);
	if (s == NULL) {
		error("no more sessions");
		return 0;
	}
	s->pw = auth_get_user();
	if (s->pw == NULL)
		fatal("no user for session %i", s->self);
	debug("session_open: session %d: link with channel %d", s->self, chanid);
	s->chanid = chanid;
	return 1;
}

Session *
session_by_channel(int id)
{
	int i;
	for(i = 0; i < MAX_SESSIONS; i++) {
		Session *s = &sessions[i];
		if (s->used && s->chanid == id) {
			debug("session_by_channel: session %d channel %d", i, id);
			return s;
		}
	}
	debug("session_by_channel: unknown channel %d", id);
	session_dump();
	return NULL;
}

Session *
session_by_pid(pid_t pid)
{
	int i;
	debug("session_by_pid: pid %d", pid);
	for(i = 0; i < MAX_SESSIONS; i++) {
		Session *s = &sessions[i];
		if (s->used && s->pid == pid)
			return s;
	}
	error("session_by_pid: unknown pid %d", pid);
	session_dump();
	return NULL;
}

int
session_window_change_req(Session *s)
{
	s->col = packet_get_int();
	s->row = packet_get_int();
	s->xpixel = packet_get_int();
	s->ypixel = packet_get_int();
	packet_done();
	pty_change_window_size(s->ptyfd, s->row, s->col, s->xpixel, s->ypixel);
	return 1;
}

int
session_pty_req(Session *s)
{
	unsigned int len;
	char *term_modes;	/* encoded terminal modes */

	if (no_pty_flag)
		return 0;
	if (s->ttyfd != -1)
		return 0;
	s->term = packet_get_string(&len);
	s->col = packet_get_int();
	s->row = packet_get_int();
	s->xpixel = packet_get_int();
	s->ypixel = packet_get_int();
	term_modes = packet_get_string(&len);
	packet_done();

	if (strcmp(s->term, "") == 0) {
		xfree(s->term);
		s->term = NULL;
	}
	/* Allocate a pty and open it. */
	if (!pty_allocate(&s->ptyfd, &s->ttyfd, s->tty, sizeof(s->tty))) {
		xfree(s->term);
		s->term = NULL;
		s->ptyfd = -1;
		s->ttyfd = -1;
		error("session_pty_req: session %d alloc failed", s->self);
		xfree(term_modes);
		return 0;
	}
	debug("session_pty_req: session %d alloc %s", s->self, s->tty);
	/*
	 * Add a cleanup function to clear the utmp entry and record logout
	 * time in case we call fatal() (e.g., the connection gets closed).
	 */
	fatal_add_cleanup(pty_cleanup_proc, (void *)s);
	pty_setowner(s->pw, s->tty);
	/* Get window size from the packet. */
	pty_change_window_size(s->ptyfd, s->row, s->col, s->xpixel, s->ypixel);

	session_proctitle(s);

	/* XXX parse and set terminal modes */
	xfree(term_modes);
	return 1;
}

int
session_subsystem_req(Session *s)
{
	unsigned int len;
	int success = 0;
	char *subsys = packet_get_string(&len);
	int i;

	packet_done();
	log("subsystem request for %s", subsys);

	for (i = 0; i < options.num_subsystems; i++) {
		if(strcmp(subsys, options.subsystem_name[i]) == 0) {
			debug("subsystem: exec() %s", options.subsystem_command[i]);
			do_exec_no_pty(s, options.subsystem_command[i], s->pw);
			success = 1;
		}
	}

	if (!success)
		log("subsystem request for %s failed, subsystem not found", subsys);

	xfree(subsys);
	return success;
}

int
session_x11_req(Session *s)
{
	if (!no_port_forwarding_flag) {
		debug("X11 forwarding disabled in user configuration file.");
		return 0;
	}
	if (!options.x11_forwarding) {
		debug("X11 forwarding disabled in server configuration file.");
		return 0;
	}
	if (xauthfile != NULL) {
		debug("X11 fwd already started.");
		return 0;
	}

	debug("Received request for X11 forwarding with auth spoofing.");
	if (s->display != NULL)
		packet_disconnect("Protocol error: X11 display already set.");

	s->single_connection = packet_get_char();
	s->auth_proto = packet_get_string(NULL);
	s->auth_data = packet_get_string(NULL);
	s->screen = packet_get_int();
	packet_done();

	s->display = x11_create_display_inet(s->screen, options.x11_display_offset);
	if (s->display == NULL) {
		xfree(s->auth_proto);
		xfree(s->auth_data);
		return 0;
	}
	xauthfile = xmalloc(MAXPATHLEN);
	strlcpy(xauthfile, "/tmp/ssh-XXXXXXXX", MAXPATHLEN);
	temporarily_use_uid(s->pw->pw_uid);
	if (mkdtemp(xauthfile) == NULL) {
		restore_uid();
		error("private X11 dir: mkdtemp %s failed: %s",
		    xauthfile, strerror(errno));
		xfree(xauthfile);
		xauthfile = NULL;
		xfree(s->auth_proto);
		xfree(s->auth_data);
		/* XXXX remove listening channels */
		return 0;
	}
	strlcat(xauthfile, "/cookies", MAXPATHLEN);
	open(xauthfile, O_RDWR|O_CREAT|O_EXCL, 0600);
	restore_uid();
	fatal_add_cleanup(xauthfile_cleanup_proc, s);
	return 1;
}

int
session_shell_req(Session *s)
{
	/* if forced_command == NULL, the shell is execed */
	char *shell = forced_command;
	packet_done();
	s->extended = 1;
	if (s->ttyfd == -1)
		do_exec_no_pty(s, shell, s->pw);
	else
		do_exec_pty(s, shell, s->pw);
	return 1;
}

int
session_exec_req(Session *s)
{
	unsigned int len;
	char *command = packet_get_string(&len);
	packet_done();
	if (forced_command) {
		xfree(command);
		command = forced_command;
		debug("Forced command '%.500s'", forced_command);
	}
	s->extended = 1;
	if (s->ttyfd == -1)
		do_exec_no_pty(s, command, s->pw);
	else
		do_exec_pty(s, command, s->pw);
	if (forced_command == NULL)
		xfree(command);
	return 1;
}

void
session_input_channel_req(int id, void *arg)
{
	unsigned int len;
	int reply;
	int success = 0;
	char *rtype;
	Session *s;
	Channel *c;

	rtype = packet_get_string(&len);
	reply = packet_get_char();

	s = session_by_channel(id);
	if (s == NULL)
		fatal("session_input_channel_req: channel %d: no session", id);
	c = channel_lookup(id);
	if (c == NULL)
		fatal("session_input_channel_req: channel %d: bad channel", id);

	debug("session_input_channel_req: session %d channel %d request %s reply %d",
	    s->self, id, rtype, reply);

	/*
	 * a session is in LARVAL state until a shell
	 * or programm is executed
	 */
	if (c->type == SSH_CHANNEL_LARVAL) {
		if (strcmp(rtype, "shell") == 0) {
			success = session_shell_req(s);
		} else if (strcmp(rtype, "exec") == 0) {
			success = session_exec_req(s);
		} else if (strcmp(rtype, "pty-req") == 0) {
			success =  session_pty_req(s);
		} else if (strcmp(rtype, "x11-req") == 0) {
			success = session_x11_req(s);
		} else if (strcmp(rtype, "subsystem") == 0) {
			success = session_subsystem_req(s);
		}
	}
	if (strcmp(rtype, "window-change") == 0) {
		success = session_window_change_req(s);
	}

	if (reply) {
		packet_start(success ?
		    SSH2_MSG_CHANNEL_SUCCESS : SSH2_MSG_CHANNEL_FAILURE);
		packet_put_int(c->remote_id);
		packet_send();
	}
	xfree(rtype);
}

void
session_set_fds(Session *s, int fdin, int fdout, int fderr)
{
	if (!compat20)
		fatal("session_set_fds: called for proto != 2.0");
	/*
	 * now that have a child and a pipe to the child,
	 * we can activate our channel and register the fd's
	 */
	if (s->chanid == -1)
		fatal("no channel for session %d", s->self);
	channel_set_fds(s->chanid,
	    fdout, fdin, fderr,
	    fderr == -1 ? CHAN_EXTENDED_IGNORE : CHAN_EXTENDED_READ);
}

void
session_pty_cleanup(Session *s)
{
	if (s == NULL || s->ttyfd == -1)
		return;

	debug("session_pty_cleanup: session %i release %s", s->self, s->tty);

	/* Cancel the cleanup function. */
	fatal_remove_cleanup(pty_cleanup_proc, (void *)s);

	/* Record that the user has logged out. */
	record_logout(s->pid, s->tty);

	/* Release the pseudo-tty. */
	pty_release(s->tty);

	/*
	 * Close the server side of the socket pairs.  We must do this after
	 * the pty cleanup, so that another process doesn't get this pty
	 * while we're still cleaning up.
	 */
	if (close(s->ptymaster) < 0)
		error("close(s->ptymaster): %s", strerror(errno));
}

void
session_exit_message(Session *s, int status)
{
	Channel *c;
	if (s == NULL)
		fatal("session_close: no session");
	c = channel_lookup(s->chanid);
	if (c == NULL)
		fatal("session_close: session %d: no channel %d",
		    s->self, s->chanid);
	debug("session_exit_message: session %d channel %d pid %d",
	    s->self, s->chanid, s->pid);

	if (WIFEXITED(status)) {
		channel_request_start(s->chanid,
		    "exit-status", 0);
		packet_put_int(WEXITSTATUS(status));
		packet_send();
	} else if (WIFSIGNALED(status)) {
		channel_request_start(s->chanid,
		    "exit-signal", 0);
		packet_put_int(WTERMSIG(status));
#ifdef WCOREDUMP
		packet_put_char(WCOREDUMP(status));
#else /* WCOREDUMP */
		packet_put_char(0);
#endif /* WCOREDUMP */
		packet_put_cstring("");
		packet_put_cstring("");
		packet_send();
	} else {
		/* Some weird exit cause.  Just exit. */
		packet_disconnect("wait returned status %04x.", status);
	}

	/* disconnect channel */
	debug("session_exit_message: release channel %d", s->chanid);
	channel_cancel_cleanup(s->chanid);
	/*
	 * emulate a write failure with 'chan_write_failed', nobody will be
	 * interested in data we write.
	 * Note that we must not call 'chan_read_failed', since there could
	 * be some more data waiting in the pipe.
	 */
	if (c->ostate != CHAN_OUTPUT_CLOSED)
		chan_write_failed(c);
	s->chanid = -1;
}

void
session_free(Session *s)
{
	debug("session_free: session %d pid %d", s->self, s->pid);
	if (s->term)
		xfree(s->term);
	if (s->display)
		xfree(s->display);
	if (s->auth_data)
		xfree(s->auth_data);
	if (s->auth_proto)
		xfree(s->auth_proto);
	s->used = 0;
}

void
session_close(Session *s)
{
	session_pty_cleanup(s);
	session_free(s);
	session_proctitle(s);
}

void
session_close_by_pid(pid_t pid, int status)
{
	Session *s = session_by_pid(pid);
	if (s == NULL) {
		debug("session_close_by_pid: no session for pid %d", s->pid);
		return;
	}
	if (s->chanid != -1)
		session_exit_message(s, status);
	session_close(s);
}

/*
 * this is called when a channel dies before
 * the session 'child' itself dies
 */
void
session_close_by_channel(int id, void *arg)
{
	Session *s = session_by_channel(id);
	if (s == NULL) {
		debug("session_close_by_channel: no session for channel %d", id);
		return;
	}
	/* disconnect channel */
	channel_cancel_cleanup(s->chanid);
	s->chanid = -1;

	debug("session_close_by_channel: channel %d kill %d", id, s->pid);
	if (s->pid == 0) {
		/* close session immediately */
		session_close(s);
	} else {
		/* notify child, delay session cleanup */
		if (s->pid <= 1) 
			fatal("session_close_by_channel: Unsafe s->pid = %d", s->pid);
		if (kill(s->pid, (s->ttyfd == -1) ? SIGTERM : SIGHUP) < 0)
			error("session_close_by_channel: kill %d: %s",
			    s->pid, strerror(errno));
	}
}

char *
session_tty_list(void)
{
	static char buf[1024];
	int i;
	buf[0] = '\0';
	for(i = 0; i < MAX_SESSIONS; i++) {
		Session *s = &sessions[i];
		if (s->used && s->ttyfd != -1) {
			if (buf[0] != '\0')
				strlcat(buf, ",", sizeof buf);
			strlcat(buf, strrchr(s->tty, '/') + 1, sizeof buf);
		}
	}
	if (buf[0] == '\0')
		strlcpy(buf, "notty", sizeof buf);
	return buf;
}

void
session_proctitle(Session *s)
{
	if (s->pw == NULL)
		error("no user for session %d", s->self);
	else
		setproctitle("%s@%s", s->pw->pw_name, session_tty_list());
}

void
do_authenticated2(void)
{
	/*
	 * Cancel the alarm we set to limit the time taken for
	 * authentication.
	 */
	alarm(0);
	server_loop2();
	if (xauthfile)
		xauthfile_cleanup_proc(NULL);
}
