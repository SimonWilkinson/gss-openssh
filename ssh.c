/*

ssh.c

Author: Tatu Ylonen <ylo@cs.hut.fi>

Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
                   All rights reserved

Created: Sat Mar 18 16:36:11 1995 ylo

Ssh client program.  This program can be used to log into a remote machine.
The software supports strong authentication, encryption, and forwarding
of X11, TCP/IP, and authentication connections.

Modified to work with SSL by Niels Provos <provos@citi.umich.edu> in Canada.

*/

#include "includes.h"
RCSID("$Id: ssh.c,v 1.10 1999/11/16 02:37:16 damien Exp $");

#include "xmalloc.h"
#include "ssh.h"
#include "packet.h"
#include "buffer.h"
#include "authfd.h"
#include "readconf.h"
#include "uidswap.h"

#ifdef HAVE___PROGNAME
extern char *__progname;
#else /* HAVE___PROGNAME */
const char *__progname = "ssh";
#endif /* HAVE___PROGNAME */

/* Flag indicating whether debug mode is on.  This can be set on the
   command line. */
int debug_flag = 0;

/* Flag indicating whether to allocate a pseudo tty.  This can be set on the
   command line, and is automatically set if no command is given on the command
   line. */
int tty_flag = 0;

/* Flag indicating that nothing should be read from stdin.  This can be set
   on the command line. */
int stdin_null_flag = 0;

/* Flag indicating that ssh should fork after authentication.  This is useful
   so that the pasphrase can be entered manually, and then ssh goes to the
   background. */
int fork_after_authentication_flag = 0;

/* General data structure for command line options and options configurable
   in configuration files.  See readconf.h. */
Options options;

/* Name of the host we are connecting to.  This is the name given on the
   command line, or the HostName specified for the user-supplied name
   in a configuration file. */
char *host;

/* socket address the host resolves to */
struct sockaddr_in hostaddr;

/* Flag to indicate that we have received a window change signal which has
   not yet been processed.  This will cause a message indicating the new
   window size to be sent to the server a little later.  This is volatile
   because this is updated in a signal handler. */
volatile int received_window_change_signal = 0;

/* Value of argv[0] (set in the main program). */
char *av0;

/* Flag indicating whether we have a valid host private key loaded. */
int host_private_key_loaded = 0;

/* Host private key. */
RSA *host_private_key = NULL;

/* Original real UID. */
uid_t original_real_uid;

/* Prints a help message to the user.  This function never returns. */

void
usage()
{
  fprintf(stderr, "Usage: %s [options] host [command]\n", av0);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -l user     Log in using this user name.\n");
  fprintf(stderr, "  -n          Redirect input from /dev/null.\n");
  fprintf(stderr, "  -a          Disable authentication agent forwarding.\n");
#ifdef AFS
  fprintf(stderr, "  -k          Disable Kerberos ticket and AFS token forwarding.\n");
#endif /* AFS */
  fprintf(stderr, "  -x          Disable X11 connection forwarding.\n");
  fprintf(stderr, "  -i file     Identity for RSA authentication (default: ~/.ssh/identity).\n");
  fprintf(stderr, "  -t          Tty; allocate a tty even if command is given.\n");
  fprintf(stderr, "  -v          Verbose; display verbose debugging messages.\n");
  fprintf(stderr, "  -V          Display version number only.\n");
  fprintf(stderr, "  -P          Don't allocate a privileged port.\n");
  fprintf(stderr, "  -q          Quiet; don't display any warning messages.\n");
  fprintf(stderr, "  -f          Fork into background after authentication.\n");
  fprintf(stderr, "  -e char     Set escape character; ``none'' = disable (default: ~).\n");

  fprintf(stderr, "  -c cipher   Select encryption algorithm: "
		  "``3des'', "
		  "``blowfish''\n");
  fprintf(stderr, "  -p port     Connect to this port.  Server must be on the same port.\n");
  fprintf(stderr, "  -L listen-port:host:port   Forward local port to remote address\n");
  fprintf(stderr, "  -R listen-port:host:port   Forward remote port to local address\n");
  fprintf(stderr, "              These cause %s to listen for connections on a port, and\n", av0);
  fprintf(stderr, "              forward them to the other side by connecting to host:port.\n");
  fprintf(stderr, "  -C          Enable compression.\n");
  fprintf(stderr, "  -g          Allow remote hosts to connect to forwarded ports.\n");
  fprintf(stderr, "  -o 'option' Process the option as if it was read from a configuration file.\n");
  exit(1);
}

/* Connects to the given host using rsh (or prints an error message and exits
   if rsh is not available).  This function never returns. */

void
rsh_connect(char *host, char *user, Buffer *command)
{
  char *args[10];
  int i;
  
  log("Using rsh.  WARNING: Connection will not be encrypted.");
  /* Build argument list for rsh. */
  i = 0;
  args[i++] = _PATH_RSH;
  args[i++] = host;    /* may have to come after user on some systems */
  if (user)
    {
      args[i++] = "-l";
      args[i++] = user;
    }
  if (buffer_len(command) > 0)
    {
      buffer_append(command, "\0", 1);
      args[i++] = buffer_ptr(command);
    }
  args[i++] = NULL;
  if (debug_flag)
    {
      for (i = 0; args[i]; i++)
	{
	  if (i != 0)
	    fprintf(stderr, " ");
	  fprintf(stderr, "%s", args[i]);
	}
      fprintf(stderr, "\n");
    }
  execv(_PATH_RSH, args);
  perror(_PATH_RSH);
  exit(1);
}

/* Main program for the ssh client. */

int
main(int ac, char **av)
{
  int i, opt, optind, type, exit_status, ok, fwd_port, fwd_host_port, authfd;
  char *optarg, *cp, buf[256];
  Buffer command;
  struct winsize ws;
  struct stat st;
  struct passwd *pw, pwcopy;
  int interactive = 0, dummy;
  uid_t original_effective_uid;
  int plen;

  /* Save the original real uid.  It will be needed later (uid-swapping may
     clobber the real uid).  */
  original_real_uid = getuid();
  original_effective_uid = geteuid();

  /* If we are installed setuid root be careful to not drop core. */
  if (original_real_uid != original_effective_uid)
    {
      struct rlimit rlim;
      rlim.rlim_cur = rlim.rlim_max = 0;
      if (setrlimit(RLIMIT_CORE, &rlim) < 0)
	fatal("setrlimit failed: %.100s", strerror(errno));
    }

  /* Use uid-swapping to give up root privileges for the duration of option
     processing.  We will re-instantiate the rights when we are ready to
     create the privileged port, and will permanently drop them when the
     port has been created (actually, when the connection has been made, as
     we may need to create the port several times). */
  temporarily_use_uid(original_real_uid);

  /* Set our umask to something reasonable, as some files are created with 
     the default umask.  This will make them world-readable but writable 
     only by the owner, which is ok for all files for which we don't set
     the modes explicitly. */
  umask(022);
  
  /* Save our own name. */
  av0 = av[0];

  /* Initialize option structure to indicate that no values have been set. */
  initialize_options(&options);

  /* Parse command-line arguments. */
  host = NULL;

  /* If program name is not one of the standard names, use it as host name. */
  if (strchr(av0, '/'))
    cp = strrchr(av0, '/') + 1;
  else
    cp = av0;
  if (strcmp(cp, "rsh") != 0 && strcmp(cp, "ssh") != 0 &&
      strcmp(cp, "rlogin") != 0 && strcmp(cp, "slogin") != 0)
    host = cp;
  
  for (optind = 1; optind < ac; optind++)
    {
      if (av[optind][0] != '-')
	{
	  if (host)
	    break;
          if ((cp = strchr(av[optind], '@'))) {
            options.user = av[optind];
            *cp = '\0';
            host = ++cp;
          }
          else
	    host = av[optind];
	  continue;
	}
      opt = av[optind][1];
      if (!opt)
	usage();
      if (strchr("eilcpLRo", opt)) /* options with arguments */
	{
	  optarg = av[optind] + 2;
	  if (strcmp(optarg, "") == 0)
	    {
	      if (optind >= ac - 1)
		usage();
	      optarg = av[++optind];
	    }
	}
      else
	{
	  if (av[optind][2])
	    usage();
	  optarg = NULL;
	}
      switch (opt)
	{
	case 'n':
	  stdin_null_flag = 1;
	  break;

	case 'f':
	  fork_after_authentication_flag = 1;
	  stdin_null_flag = 1;
	  break;

	case 'x':
	  options.forward_x11 = 0;
	  break;

	case 'X':
	  options.forward_x11 = 1;
	  break;

	case 'g':
	  options.gateway_ports = 1;
	  break;

	case 'P':
	  options.use_privileged_port = 0;
	  break;

	case 'a':
	  options.forward_agent = 0;
	  break;
#ifdef AFS
	case 'k':
	  options.kerberos_tgt_passing = 0;
	  options.afs_token_passing = 0;
	  break;
#endif
	case 'i':
	  if (stat(optarg, &st) < 0)
	    {
	      fprintf(stderr, "Warning: Identity file %s does not exist.\n",
		      optarg);
	      break;
	    }
	  if (options.num_identity_files >= SSH_MAX_IDENTITY_FILES)
	    fatal("Too many identity files specified (max %d)",
		  SSH_MAX_IDENTITY_FILES);
	  options.identity_files[options.num_identity_files++] = 
	    xstrdup(optarg);
	  break;

	case 't':
	  tty_flag = 1;
	  break;

	case 'v':
	case 'V':
	  fprintf(stderr, "SSH Version %s, protocol version %d.%d.\n",
		  SSH_VERSION, PROTOCOL_MAJOR, PROTOCOL_MINOR);
	  fprintf(stderr, "Compiled with SSL.\n");
	  if (opt == 'V')
	    exit(0);
	  debug_flag = 1;
	  options.log_level = SYSLOG_LEVEL_DEBUG;
	  break;

	case 'q':
	  options.log_level = SYSLOG_LEVEL_QUIET;
	  break;

	case 'e':
	  if (optarg[0] == '^' && optarg[2] == 0 &&
	      (unsigned char)optarg[1] >= 64 && (unsigned char)optarg[1] < 128)
	    options.escape_char = (unsigned char)optarg[1] & 31;
	  else
	    if (strlen(optarg) == 1)
	      options.escape_char = (unsigned char)optarg[0];
	    else
	      if (strcmp(optarg, "none") == 0)
		options.escape_char = -2;
	      else
		{
		  fprintf(stderr, "Bad escape character '%s'.\n", optarg);
		  exit(1);
		}
	  break;

	case 'c':
	  options.cipher = cipher_number(optarg);
	  if (options.cipher == -1)
	    {
	      fprintf(stderr, "Unknown cipher type '%s'\n", optarg);
	      exit(1);
	    }
	  break;

	case 'p':
	  options.port = atoi(optarg);
	  if (options.port < 1 || options.port > 65535)
	    {
	      fprintf(stderr, "Bad port %s.\n", optarg);
	      exit(1);
	    }
	  break;

	case 'l':
	  options.user = optarg;
	  break;

	case 'R':
	  if (sscanf(optarg, "%d:%255[^:]:%d", &fwd_port, buf, 
		     &fwd_host_port) != 3)
	    {
	      fprintf(stderr, "Bad forwarding specification '%s'.\n", optarg);
	      usage();
	      /*NOTREACHED*/
	    }
	  add_remote_forward(&options, fwd_port, buf, fwd_host_port);
	  break;

	case 'L':
	  if (sscanf(optarg, "%d:%255[^:]:%d", &fwd_port, buf, 
		     &fwd_host_port) != 3)
	    {
	      fprintf(stderr, "Bad forwarding specification '%s'.\n", optarg);
	      usage();
	      /*NOTREACHED*/
	    }
	  add_local_forward(&options, fwd_port, buf, fwd_host_port);
	  break;

	case 'C':
	  options.compression = 1;
	  break;

	case 'o':
	  dummy = 1;
	  if (process_config_line(&options, host ? host : "", optarg,
			          "command-line", 0, &dummy) != 0)
            exit(1);
	  break;

	default:
	  usage();
	}
    }

 /* Check that we got a host name. */
  if (!host)
    usage();

  /* check if RSA support exists */
  if (rsa_alive() == 0) {

    fprintf(stderr,
      "%s: no RSA support in libssl and libcrypto.  See ssl(8).\n",
      __progname);
    exit(1);
  }

  /* Initialize the command to execute on remote host. */
  buffer_init(&command);

  /* Save the command to execute on the remote host in a buffer.  There is
     no limit on the length of the command, except by the maximum packet
     size.  Also sets the tty flag if there is no command. */
  if (optind == ac)
    {
      /* No command specified - execute shell on a tty. */
      tty_flag = 1;
    }
  else
    {
      /* A command has been specified.  Store it into the buffer. */
      for (i = optind; i < ac; i++)
	{
	  if (i > optind)
	    buffer_append(&command, " ", 1);
	  buffer_append(&command, av[i], strlen(av[i]));
	}
    }

  /* Cannot fork to background if no command. */
  if (fork_after_authentication_flag && buffer_len(&command) == 0)
    fatal("Cannot fork into background without a command to execute.");
  
  /* Allocate a tty by default if no command specified. */
  if (buffer_len(&command) == 0)
    tty_flag = 1;

  /* Do not allocate a tty if stdin is not a tty. */
  if (!isatty(fileno(stdin)))
    {
      if (tty_flag)
	fprintf(stderr, "Pseudo-terminal will not be allocated because stdin is not a terminal.\n");
      tty_flag = 0;
    }

  /* Get user data. */
  pw = getpwuid(original_real_uid);
  if (!pw)
    {
      fprintf(stderr, "You don't exist, go away!\n");
      exit(1);
    }
  
  /* Take a copy of the returned structure. */
  memset(&pwcopy, 0, sizeof(pwcopy));
  pwcopy.pw_name = xstrdup(pw->pw_name);
  pwcopy.pw_passwd = xstrdup(pw->pw_passwd);
  pwcopy.pw_uid = pw->pw_uid;
  pwcopy.pw_gid = pw->pw_gid;
  pwcopy.pw_dir = xstrdup(pw->pw_dir);
  pwcopy.pw_shell = xstrdup(pw->pw_shell);
  pw = &pwcopy;

  /* Initialize "log" output.  Since we are the client all output actually
     goes to the terminal. */
  log_init(av[0], options.log_level, SYSLOG_FACILITY_USER, 0);

  /* Read per-user configuration file. */
  snprintf(buf, sizeof buf, "%.100s/%.100s", pw->pw_dir, SSH_USER_CONFFILE);
  read_config_file(buf, host, &options);

  /* Read systemwide configuration file. */
  read_config_file(HOST_CONFIG_FILE, host, &options);

  /* Fill configuration defaults. */
  fill_default_options(&options);

  /* reinit */
  log_init(av[0], options.log_level, SYSLOG_FACILITY_USER, 0);

  if (options.user == NULL)
    options.user = xstrdup(pw->pw_name);

  if (options.hostname != NULL)
    host = options.hostname;

  /* Find canonic host name. */
  if (strchr(host, '.') == 0)
    {
      struct hostent *hp = gethostbyname(host);
      if (hp != 0)
	{
	  if (strchr(hp->h_name, '.') != 0)
	    host = xstrdup(hp->h_name);
	  else if (hp->h_aliases != 0
		   && hp->h_aliases[0] != 0
		   && strchr(hp->h_aliases[0], '.') != 0)
	    host = xstrdup(hp->h_aliases[0]);
	}
    }

  /* Disable rhosts authentication if not running as root. */
  if (original_effective_uid != 0 || !options.use_privileged_port)
    {
      options.rhosts_authentication = 0;
      options.rhosts_rsa_authentication = 0;
    }

  /* If using rsh has been selected, exec it now (without trying anything
     else).  Note that we must release privileges first. */
  if (options.use_rsh)
    {
      /* Restore our superuser privileges.  This must be done before
         permanently setting the uid. */
      restore_uid();

      /* Switch to the original uid permanently. */
      permanently_set_uid(original_real_uid);

      /* Execute rsh. */
      rsh_connect(host, options.user, &command);
      fatal("rsh_connect returned");
    }

  /* Restore our superuser privileges. */
  restore_uid();

  /* Open a connection to the remote host.  This needs root privileges if
     rhosts_{rsa_}authentication is enabled. */

  ok = ssh_connect(host, &hostaddr, options.port, options.connection_attempts,
		   !options.rhosts_authentication &&
		   !options.rhosts_rsa_authentication,
		   original_real_uid, options.proxy_command);

  /* If we successfully made the connection, load the host private key in
     case we will need it later for combined rsa-rhosts authentication. 
     This must be done before releasing extra privileges, because the file
     is only readable by root. */
  if (ok)
    {
      host_private_key = RSA_new();
      if (load_private_key(HOST_KEY_FILE, "", host_private_key, NULL))
	host_private_key_loaded = 1;
    }

  /* Get rid of any extra privileges that we may have.  We will no longer need
     them.  Also, extra privileges could make it very hard to read identity
     files and other non-world-readable files from the user's home directory
     if it happens to be on a NFS volume where root is mapped to nobody. */

  /* Note that some legacy systems need to postpone the following call to
     permanently_set_uid() until the private hostkey is destroyed with
     RSA_free().  Otherwise the calling user could ptrace() the process,
     read the private hostkey and impersonate the host.  OpenBSD does not
     allow ptracing of setuid processes. */

  permanently_set_uid(original_real_uid);

  /* Now that we are back to our own permissions, create ~/.ssh directory
     if it doesn\'t already exist. */
  snprintf(buf, sizeof buf, "%.100s/%.100s", pw->pw_dir, SSH_USER_DIR);
  if (stat(buf, &st) < 0)
    if (mkdir(buf, 0755) < 0)
      error("Could not create directory '%.200s'.", buf);

  /* Check if the connection failed, and try "rsh" if appropriate. */
  if (!ok)
    {
      if (options.port != 0)
	log("Secure connection to %.100s on port %d refused%.100s.", 
	    host, options.port,
	    options.fallback_to_rsh ? "; reverting to insecure method" : "");
      else
	log("Secure connection to %.100s refused%.100s.", host,
	    options.fallback_to_rsh ? "; reverting to insecure method" : "");

      if (options.fallback_to_rsh)
	{
	  rsh_connect(host, options.user, &command);
	  fatal("rsh_connect returned");
	}
      exit(1);
    }

  /* Expand ~ in options.identity_files. */
  for (i = 0; i < options.num_identity_files; i++)
    options.identity_files[i] = 
      tilde_expand_filename(options.identity_files[i], original_real_uid);

  /* Expand ~ in known host file names. */
  options.system_hostfile = tilde_expand_filename(options.system_hostfile,
						  original_real_uid);
  options.user_hostfile = tilde_expand_filename(options.user_hostfile,
						original_real_uid);

  /* Log into the remote system.  This never returns if the login fails. */
  ssh_login(host_private_key_loaded, host_private_key, 
	    host, &hostaddr, original_real_uid);

  /* We no longer need the host private key.  Clear it now. */
  if (host_private_key_loaded)
    RSA_free(host_private_key); /* Destroys contents safely */

  /* Close connection cleanly after attack. */
  cipher_attack_detected = packet_disconnect;

  /* Enable compression if requested. */
  if (options.compression)
    {
      debug("Requesting compression at level %d.", options.compression_level);

      if (options.compression_level < 1 || options.compression_level > 9)
	fatal("Compression level must be from 1 (fast) to 9 (slow, best).");

      /* Send the request. */
      packet_start(SSH_CMSG_REQUEST_COMPRESSION);
      packet_put_int(options.compression_level);
      packet_send();
      packet_write_wait();
      type = packet_read(&plen);
      if (type == SSH_SMSG_SUCCESS)
	packet_start_compression(options.compression_level);
      else if (type == SSH_SMSG_FAILURE)
	log("Warning: Remote host refused compression.");
      else
	packet_disconnect("Protocol error waiting for compression response.");
    }

  /* Allocate a pseudo tty if appropriate. */
  if (tty_flag)
    {
      debug("Requesting pty.");

      /* Start the packet. */
      packet_start(SSH_CMSG_REQUEST_PTY);

      /* Store TERM in the packet.  There is no limit on the length of the
         string. */
      cp = getenv("TERM");
      if (!cp)
	cp = "";
      packet_put_string(cp, strlen(cp));

      /* Store window size in the packet. */
      if (ioctl(fileno(stdin), TIOCGWINSZ, &ws) < 0)
	memset(&ws, 0, sizeof(ws));
      packet_put_int(ws.ws_row);
      packet_put_int(ws.ws_col);
      packet_put_int(ws.ws_xpixel);
      packet_put_int(ws.ws_ypixel);
      
      /* Store tty modes in the packet. */
      tty_make_modes(fileno(stdin));

      /* Send the packet, and wait for it to leave. */
      packet_send();
      packet_write_wait();

      /* Read response from the server. */
      type = packet_read(&plen);
      if (type == SSH_SMSG_SUCCESS)
	interactive = 1;
      else if (type == SSH_SMSG_FAILURE)
	log("Warning: Remote host failed or refused to allocate a pseudo tty.");
      else
	packet_disconnect("Protocol error waiting for pty request response.");
    }

  /* Request X11 forwarding if enabled and DISPLAY is set. */
  if (options.forward_x11 && getenv("DISPLAY") != NULL)
    {
      char line[512], proto[512], data[512];
      FILE *f;
      int forwarded = 0, got_data = 0, i;

#ifdef XAUTH_PATH
      /* Try to get Xauthority information for the display. */
      snprintf(line, sizeof line, "%.100s list %.200s 2>/dev/null", 
	      XAUTH_PATH, getenv("DISPLAY"));
      f = popen(line, "r");
      if (f && fgets(line, sizeof(line), f) && 
	  sscanf(line, "%*s %s %s", proto, data) == 2)
	got_data = 1;
      if (f)
	pclose(f);
#endif /* XAUTH_PATH */
      /* If we didn't get authentication data, just make up some data.  The
	 forwarding code will check the validity of the response anyway, and
	 substitute this data.  The X11 server, however, will ignore this
	 fake data and use whatever authentication mechanisms it was using
	 otherwise for the local connection. */
      if (!got_data)
	{
          u_int32_t rand = 0;

	  strlcpy(proto, "MIT-MAGIC-COOKIE-1", sizeof proto);
          for (i = 0; i < 16; i++) {
            if (i % 4 == 0)
              rand = arc4random();
            snprintf(data + 2 * i, sizeof data - 2 * i, "%02x", rand & 0xff);
            rand >>= 8;
          }
	}

      /* Got local authentication reasonable information.  Request forwarding
	 with authentication spoofing. */
      debug("Requesting X11 forwarding with authentication spoofing.");
      x11_request_forwarding_with_spoofing(proto, data);

      /* Read response from the server. */
      type = packet_read(&plen);
      if (type == SSH_SMSG_SUCCESS)
	{
	  forwarded = 1;
	  interactive = 1;
	}
      else if (type == SSH_SMSG_FAILURE)
	log("Warning: Remote host denied X11 forwarding.");
      else
	packet_disconnect("Protocol error waiting for X11 forwarding");
    }

  /* Tell the packet module whether this is an interactive session. */
  packet_set_interactive(interactive, options.keepalives);

  /* Clear agent forwarding if we don\'t have an agent. */
  authfd = ssh_get_authentication_socket();
  if (authfd < 0)
    options.forward_agent = 0;
  else
    ssh_close_authentication_socket(authfd);

  /* Request authentication agent forwarding if appropriate. */
  if (options.forward_agent)
    {
      debug("Requesting authentication agent forwarding.");
      auth_request_forwarding();
      
      /* Read response from the server. */
      type = packet_read(&plen);
      packet_integrity_check(plen, 0, type);
      if (type != SSH_SMSG_SUCCESS)
	log("Warning: Remote host denied authentication agent forwarding.");
    }

  /* Initiate local TCP/IP port forwardings. */
  for (i = 0; i < options.num_local_forwards; i++)
    {
      debug("Connections to local port %d forwarded to remote address %.200s:%d",
	    options.local_forwards[i].port, options.local_forwards[i].host, 
	    options.local_forwards[i].host_port);
      channel_request_local_forwarding(options.local_forwards[i].port,
				       options.local_forwards[i].host,
				       options.local_forwards[i].host_port);
    }

  /* Initiate remote TCP/IP port forwardings. */
  for (i = 0; i < options.num_remote_forwards; i++)
    {
      debug("Connections to remote port %d forwarded to local address %.200s:%d",
	    options.remote_forwards[i].port, options.remote_forwards[i].host, 
	    options.remote_forwards[i].host_port);
      channel_request_remote_forwarding(options.remote_forwards[i].port,
					options.remote_forwards[i].host,
					options.remote_forwards[i].host_port);
    }

  /* If requested, fork and let ssh continue in the background. */
  if (fork_after_authentication_flag)
    {
      int ret = fork();
      if (ret == -1)
	fatal("fork failed: %.100s", strerror(errno));
      if (ret != 0)
	exit(0);
      setsid();
    }

  /* If a command was specified on the command line, execute the command now.
     Otherwise request the server to start a shell. */
  if (buffer_len(&command) > 0)
    {
      int len = buffer_len(&command);
      if (len > 900)
	len = 900;
      debug("Sending command: %.*s", len, buffer_ptr(&command));
      packet_start(SSH_CMSG_EXEC_CMD);
      packet_put_string(buffer_ptr(&command), buffer_len(&command));
      packet_send();
      packet_write_wait();
    }
  else
    {
      debug("Requesting shell.");
      packet_start(SSH_CMSG_EXEC_SHELL);
      packet_send();
      packet_write_wait();
    }

  /* Enter the interactive session. */
  exit_status = client_loop(tty_flag, tty_flag ? options.escape_char : -1);

  /* Close the connection to the remote host. */
  packet_close();
  
  /* Exit with the status returned by the program on the remote side. */
  exit(exit_status);
}
