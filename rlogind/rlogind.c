/* Copyright (C) 1998,2001 Free Software Foundation, Inc.

   This file is part of GNU Inetutils.

   GNU Inetutils is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   GNU Inetutils is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU Inetutils; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <version.h>

#include <signal.h>
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#include <termios.h>
#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#ifdef HAVE_SYS_STREAM_H
# include <sys/stream.h>
#endif
#ifdef HAVE_SYS_TTY_H
# include <sys/tty.h>
#endif
#ifdef HAVE_SYS_PTYVAR_H
# include <sys/ptyvar.h>
#endif

#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_IN_SYSTM_H
# include <netinet/in_systm.h>
#endif
#ifdef HAVE_NETINET_IP_H
# include <netinet/ip.h>
#endif
#include <arpa/inet.h>
#include <netdb.h>

#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif

#ifndef TIOCPKT_WINDOW
# define TIOCPKT_WINDOW 0x80
#endif

/* `defaults' for tty settings.  */
#ifndef TTYDEF_IFLAG
#define	TTYDEF_IFLAG	(BRKINT | ISTRIP | ICRNL | IMAXBEL | IXON | IXANY)
#endif
#ifndef TTYDEF_OFLAG
#ifndef OXTABS
#define OXTABS 0
#endif
#define TTYDEF_OFLAG	(OPOST | ONLCR | OXTABS)
#endif
#ifndef TTYDEF_LFLAG
#define TTYDEF_LFLAG	(ECHO | ICANON | ISIG | IEXTEN | ECHOE|ECHOKE|ECHOCTL)
#endif

#ifdef	KERBEROS
#include <kerberosIV/des.h>
#include <kerberosIV/krb.h>
#define	SECURE_MESSAGE "This rlogin session is using DES encryption for all transmissions.\r\n"
#endif /* KERBEROS */

#define	ENVSIZE	(sizeof("TERM=")-1)	/* skip null for concatenation */

#ifndef DEFMAXCHILDREN
# define DEFMAXCHILDREN 10   /* Default maximum number of children */
#endif
#ifndef DEFPORT
# define DEFPORT 513
#endif

extern int __check_rhosts_file;

struct auth_data
{
  struct sockaddr_in from;
  char *hostname;
  char *lusername;
  char *rusername;
  char *term;
  char *env[2];
};

static char short_options[] = "aD::d::hL::lnkp:rxVv";
static struct option long_options[] =
{
  {"verify-hostname", no_argument, 0, 'a'},
  {"daemon", optional_argument, 0, 'd'},
  {"no-rhosts", no_argument, 0, 'l'},
  {"no-keepalive", no_argument, 0, 'n'},
  {"local-domain", required_argument, 0, 'L'},
  {"kerberos", no_argument, 0, 'k'},
  {"encrypt", no_argument, 0, 'x'},
  {"debug", optional_argument, 0, 'D'},
  {"help", no_argument, 0, 'h'},
  {"version", no_argument, 0, 'V'},
  {"port", required_argument, 0, 'p'},
  {"reverse-required", no_argument, 0, 'r'},
  {0, 0, 0, 0}
};

int verify_hostname = 0;
int keepalive = 1;
#ifdef KERBEROS
int kerberos = 0;
#ifdef CRYPT
int encrypt = 0;
#endif /* CRYPT */
#endif /* KERBEROS */
int reverse_required = 0;
int debug_level = 0;

static int numchildren;
int netf;
char line[1024];		/* FIXME */
int confirmed;
char *path_login = PATH_LOGIN;
char *local_domain_name;
int local_dot_count;

struct winsize win = {0, 0, 0, 0};

static void usage __P((void));
static void rlogin_daemon __P((int maxchildren, int port));
static int rlogind_auth __P((int fd, struct auth_data *ap));
static void setup_tty __P((int fd, struct auth_data *ap));
static void exec_login __P((int authenticated, struct auth_data *ap));
static int rlogind_mainloop __P((int infd, int outfd));
static int do_rlogin __P((int infd, struct auth_data *ap));
static int do_krb_login __P((int infd, struct auth_data *ap));
static void getstr __P((int infd, char **ptr, char *prefix));
static void protocol __P((int f, int p));
static int control __P((int pty, char *cp, int n));
static RETSIGTYPE cleanup __P((int signo));
static void fatal __P((int f, char *msg, int syserr));
static int in_local_domain __P((char *hostname));
static char *topdomain __P((char *name, int max_dots));

static RETSIGTYPE
rlogind_sigchld(int sig)
{
  pid_t pid;
  int status;

  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    --numchildren;
  signal(sig, rlogind_sigchld);
}

#define MODE_INETD 0
#define MODE_DAEMON 1

#if defined(KERBEROS) && defined(CRYPT)
# define IF_ENCRYPT(stmt) if (encrypt) stmt
# define IF_NOT_ENCRYPT(stmt) if (!encrypt) stmt
# define ENC_READ(c, fd, buf, size) \
 if (encrypt) \
     c = des_read(fd, buf, size); \
 else \
     c = read(fd, buf, size);
# define ENC_WRITE(c, fd, buf, size) \
 if (encrypt) \
     c = des_write(fd, buf, size); \
 else \
     c = write(fd, buf, size);
#else
# define IF_ENCRYPT(stmt)
# define IF_NOT_ENCRYPT(stmt) stmt
# define ENC_READ(c, fd, buf, size) c = read(fd, buf, size)
# define ENC_WRITE(c, fd, buf, size) c = write(fd, buf, size)
#endif

void rlogin_daemon (int maxchildren, int port);
int rlogind_mainloop (int infd, int outfd);

int
main(int argc, char *argv[])
{
  int port = 0;
  int maxchildren = DEFMAXCHILDREN;
  int mode = MODE_INETD;
  int c;

  while ((c = getopt_long(argc, argv, short_options, long_options, NULL))
	 != EOF)
    {
      switch (c)
	{
	case 'a':
	  verify_hostname = 1;
	  break;
	case 'D':
	  if (optarg)
	    debug_level = atoi(optarg);
	  break;
	case 'd':
	  mode = MODE_DAEMON;
	  if (optarg)
	    maxchildren = strtoul (optarg, NULL, 10);
	  if (maxchildren == 0)
	    maxchildren = DEFMAXCHILDREN;
	  break;
	case 'l':
	  __check_rhosts_file = 0; /* FIXME: extern var? */
	  break;
	case 'L':
	  local_domain_name = optarg;
	  break;
	case 'n':
	  keepalive = 0;
	  break;
#ifdef KERBEROS
	case 'k':
	  kerberos = 1;
	  break;
#ifdef CRYPT
	case 'x':
	  encrypt = 1;
	  break;
#endif /* CRYPT */
#endif /* KERBEROS */

	case 'p':
	  port = atoi(optarg);
	  break;

	case 'r':
	  reverse_required = 1;
	  break;

	case 'V':
	  printf ("rlogind (%s %s)\n", inetutils_package, inetutils_version);
	  exit(0);

	case 'h':
	default:
	  usage();
	  exit(0);
	}
    }

  openlog("rlogind", LOG_PID | LOG_CONS, LOG_AUTH);
  argc -= optind;
  if (argc > 0)
    {
      syslog(LOG_ERR, "%d extra arguments", argc);
      exit(1);
    }

  signal(SIGHUP, SIG_IGN);

  if (!local_domain_name)
    {
      char *p = localhost();

      if (!p)
	{
	  syslog(LOG_ERR, "can't determine local hostname");
	  exit(1);
	}
      local_dot_count = 2;
      local_domain_name = topdomain(p, local_dot_count);
    }
  else
    {
      char *p;
      local_dot_count = 0;
      for (p = local_domain_name; *p; p++)
	if (*p == '.')
	  local_dot_count++;
    }

  if (mode == MODE_DAEMON)
    rlogin_daemon (maxchildren, port);
  else
    exit(rlogind_mainloop (fileno (stdin), fileno (stdout)));

  /* To pacify lint */
  return 0;
}


void
rlogin_daemon (int maxchildren, int port)
{
  pid_t pid;
  size_t size;
  struct sockaddr_in saddr;
  int listenfd, fd;

  if (port == 0)
    {
      struct servent *svp;

      svp = getservbyname ("login", "tcp");
      if (svp != NULL)
	port = ntohs(svp->s_port);
      else
	port = DEFPORT;
    }

  pid = fork();
  if (pid == -1)
    {
      perror("fork");
      fatal(fileno(stderr), "exiting", 0);
      exit(1);
    }

  if (pid > 0)
    exit(0);

  setsid();

  {
    /* Close inherited file descriptors.  */
    int i;
#if defined(HAVE_SYSCONF) && defined(_SC_OPEN_MAX)
    size_t fdmax = sysconf(_SC_OPEN_MAX);
#elif defined(HAVE_GETDTABLESIZE)
    size_t fdmax = getdtablesize();
#else
    size_t fdmax = 64;
#endif

    for (i = 0; i < fdmax; i++)
      close(i);

    /* Hold first three descriptors. This is needed so that master/slave
       pty fds do not clash with standard in,out,err. The first three
       descriptors will be dup'ed in openpty() anyway. */
    for (i = 0; i < 3; i++)
      i = open("/dev/null", O_RDWR);
  }

  signal(SIGCHLD, rlogind_sigchld);

  listenfd = socket(AF_INET, SOCK_STREAM, 0);

  if (listenfd == -1)
    {
      syslog (LOG_ERR, "socket: %s", strerror(errno));
      exit (1);
    }

  {
    int on = 1;
    setsockopt (listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  }

  size = sizeof (saddr);
  memset (&saddr, 0, size);
  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = htonl (INADDR_ANY);
  saddr.sin_port = htons (port);

  size = sizeof(saddr);
  if (bind (listenfd, (struct sockaddr *)&saddr, size) == -1)
    {
      syslog (LOG_ERR, "bind: %s", strerror (errno));
      exit (1);
    }

  if (listen (listenfd, 128) == -1)
    {
      syslog (LOG_ERR, "listen: %s", strerror (errno));
      exit (1);
    }

  while (1)
    {
      if (numchildren > maxchildren)
        {
	  syslog (LOG_ERR, "too many children (%d)", numchildren);
          pause();
          continue;
        }

      size = sizeof(saddr);
      fd = accept (listenfd, (struct sockaddr *)&saddr, &size);

      if (fd == -1)
	{
	  if (errno == EINTR)
	    continue;
	  syslog(LOG_ERR, "accept: %s", strerror (errno));
	  exit(1);
	}

      pid = fork();
      if (pid == -1)
	syslog(LOG_ERR, "fork: %s", strerror (errno));
      else if (pid == 0) /* child */
	{
	  close(listenfd);
	  exit(rlogind_mainloop(fd, fd));
	}
      else
	numchildren++;
      close(fd);
    }
}

int
rlogind_auth(int fd, struct auth_data *ap)
{
  struct hostent *hp;
  char *hostname;
  int authenticated = 0;

  confirmed = 0;

  /* Check the remote host name */
  hp = gethostbyaddr((char *) &ap->from.sin_addr, sizeof(struct in_addr),
		     ap->from.sin_family);
  if (hp)
    hostname = hp->h_name;
  else if (reverse_required)
    {
      syslog(LOG_CRIT, "can't resolve remote IP address");
      exit(1);
    }
  else
    hostname = inet_ntoa(ap->from.sin_addr);

  ap->hostname = strdup(hostname);

  if (verify_hostname || in_local_domain(ap->hostname))
    {
      int match = 0;
      for (hp = gethostbyname(ap->hostname); hp && !match; hp->h_addr_list++)
	{
	  if (hp->h_addr_list[0] == NULL)
	    break;
	  match = memcmp(hp->h_addr_list[0], &ap->from.sin_addr,
			 sizeof(ap->from.sin_addr)) == 0;
	}
      if (!match)
	{
	  syslog(LOG_ERR|LOG_AUTH,
		 "cannot find matching IP for %s (%s)",
		 ap->hostname, inet_ntoa(ap->from.sin_addr));
	  fatal(fd, "Permission denied", 0);
	}
    }

#ifdef	KERBEROS
  if (kerberos)
    {
      int rc = do_krb_login (infd, ap);
      if (rc == 0)
	authenticated++;
      else if (rc > 0)
	fatal (fd, krb_err_txt[rc], 0);
      write (fd, &c, 1);
      confirmed = 1;		/* we sent the null! */
    }
  else
#endif
    {
      int port = ntohs(ap->from.sin_port);
      if (ap->from.sin_family != AF_INET ||
	  port >= IPPORT_RESERVED ||
	  port < IPPORT_RESERVED / 2)
	{
	  syslog (LOG_NOTICE, "Connection from %s on illegal port %d",
		  inet_ntoa (ap->from.sin_addr), port);
	  fatal (fd, "Permission denied", 0);
	}
#ifdef IP_OPTIONS
      {
	u_char optbuf[BUFSIZ / 3], *cp;
	char lbuf[BUFSIZ], *lp;
	int optsize = sizeof (optbuf), ipproto;
	struct protoent *ip;

	if ((ip = getprotobyname ("ip")) != NULL)
	  ipproto = ip->p_proto;
	else
	  ipproto = IPPROTO_IP;
	if (getsockopt (0, ipproto, IP_OPTIONS, (char *) optbuf,
			&optsize) == 0 && optsize != 0)
	  {
	    lp = lbuf;
	    for (cp = optbuf; optsize > 0; cp++, optsize--, lp += 3)
	      sprintf (lp, " %2.2x", *cp);
	    syslog (LOG_NOTICE,
	      "Ignoring IP options: %s", lbuf);
	    if (setsockopt (0, ipproto, IP_OPTIONS, (char *) NULL, optsize))
	      {
		syslog (LOG_ERR, "setsockopt IP_OPTIONS NULL: %m");
		exit (1);
	      }
	  }
      }
#endif /* IP_OPTIONS */
      if (do_rlogin (fd, ap) == 0)
	authenticated++;
    }

  if (confirmed == 0)
    {
      write (fd, "", 1);
      confirmed = 1;		/* we sent the null! */
    }

  IF_ENCRYPT(des_write (fd, SECURE_MESSAGE, sizeof (SECURE_MESSAGE) - 1));

  return authenticated;
}

void
setup_tty (int fd, struct auth_data *ap)
{
  register char *cp = strchr (ap->term + ENVSIZE, '/');
  char *speed;
  struct termios tt;

  tcgetattr (fd, &tt);
  if (cp)
    {
      *cp++ = '\0';
      speed = cp;
      cp = strchr (speed, '/');
      if (cp)
	*cp++ = '\0';
#ifdef HAVE_CFSETSPEED
      cfsetspeed (&tt, atoi (speed));
#else
      cfsetispeed (&tt, atoi (speed));
      cfsetospeed (&tt, atoi (speed));
#endif
    }
  tt.c_iflag = TTYDEF_IFLAG;
  tt.c_oflag = TTYDEF_OFLAG;
  tt.c_lflag = TTYDEF_LFLAG;
  tcsetattr (fd, TCSAFLUSH, &tt);
  ap->env[0] = ap->term;
  ap->env[1] = 0;
}

#ifdef UTMPX
char *utmp_ptsid (); /*FIXME*/
void utmp_init ();

void
setup_utmp(char *line)
{
  char *ut_id = utmp_ptsid (line, "rl");
  utmp_init (line + sizeof ("/dev/") - 1, ".rlogin", ut_id);
}
#else
# define setup_utmp(line)
#endif

void
exec_login(int authenticated, struct auth_data *ap)
{
  if (authenticated)
    {
#ifdef SOLARIS
      execle (path_login, "login", "-p",
	      "-h", ap->hostname, ap->term, "-f", "--",
	      ap->lusername, NULL, ap->env);
#else
      execle (path_login, "login", "-p",
	      "-h", ap->hostname, "-f", "--",
	      ap->lusername, NULL, ap->env);
#endif
    }
  else
    {
#ifdef SOLARIS
      execle (path_login, "login", "-p",
	      "-h", ap->hostname, ap->term, "--",
	      ap->lusername, NULL, ap->env);
#else
      execle (PATH_LOGIN, "login", "-p",
	      "-h", ap->hostname, "--",
	      ap->lusername, NULL, ap->env);
#endif
    }
  syslog(LOG_ERR, "can't exec login: %m");
}

int
rlogind_mainloop (int infd, int outfd)
{
  size_t size;
  int true = 1;
  struct auth_data auth_data;
  char c;
  int authenticated;
  pid_t pid;
  int master;

  memset(&auth_data, 0, sizeof(auth_data));
  size = sizeof(auth_data.from);
  if (getpeername(infd, (struct sockaddr *) &auth_data.from, &size) < 0)
    {
      syslog (LOG_ERR, "Can't get peer name of remote host: %m");
      fatal (outfd, "Can't get peer name of remote host", 1);
    }

  syslog(LOG_INFO, "Connect from %s", inet_ntoa(auth_data.from.sin_addr));

  if (keepalive
      && setsockopt(infd, SOL_SOCKET, SO_KEEPALIVE, &true, sizeof(true)) < 0)
    syslog(LOG_WARNING, "setsockopt (SO_KEEPALIVE): %m");

#if defined (IP_TOS) && defined (IPPROTO_IP) && defined (IPTOS_LOWDELAY)
  true = IPTOS_LOWDELAY;
  if (setsockopt(infd, IPPROTO_IP, IP_TOS, (char *) &true, sizeof(true)) < 0)
    syslog (LOG_WARNING, "setsockopt (IP_TOS): %m");
#endif

  alarm(60); /* Wait at most 60 seconds. FIXME: configurable? */

  /* Read the null byte */
  if (read(infd, &c, 1) != 1 || c != 0)
    {
      syslog(LOG_CRIT, "protocol error: expected 0 byte");
      exit(1);
    }

  alarm(0);

  authenticated = rlogind_auth(infd, &auth_data);

  pid = forkpty (&master, line, NULL, &win);

  if (pid < 0)
    {
      if (errno == ENOENT)
	{
	  syslog(LOG_ERR, "Out of ptys");
	  fatal (infd, "Out of ptys", 0);
	}
      else
	{
	  syslog(LOG_ERR, "forkpty: %m");
	  fatal (infd, "Forkpty", 1);
	}
    }

  if (pid == 0)
    {
      /* Child */
      if (infd > 2)
	close(infd);

      setup_tty(0, &auth_data);
      setup_utmp(line);

      exec_login(authenticated, &auth_data);
      fatal(infd, "can't execute login", 1);
    }

  /* Parent */
  true = 1;
  IF_NOT_ENCRYPT(ioctl (infd, FIONBIO, &true));
  ioctl (master, FIONBIO, &true);
  ioctl (master, TIOCPKT, &true);
  netf = infd; /* Needed for cleanup() */
  signal (SIGCHLD, cleanup);
  protocol (infd, master);
  signal (SIGCHLD, SIG_IGN);
  cleanup (0);
}


int
do_rlogin(int infd, struct auth_data *ap)
{
  struct passwd *pwd;
  int rc;

  getstr (infd, &ap->rusername, NULL);
  getstr (infd, &ap->lusername, NULL);
  getstr (infd, &ap->term, "TERM=");

  pwd = getpwnam (ap->lusername);
  if (pwd == NULL)
    {
      syslog(LOG_ERR, "no passwd entry for %s", ap->lusername);
      fatal(infd, "Permission denied", 0);
    }
  if (pwd->pw_uid == 0)
    {
      syslog(LOG_ERR, "root logins not permitted");
      fatal(infd, "Permission denied", 0);
    }

  rc = iruserok (ap->from.sin_addr.s_addr, 0,
		 ap->rusername, ap->lusername);
  if (rc)
    syslog(LOG_ERR, "iruserok failed: rusername=%s, lusername=%s",
	   ap->rusername, ap->lusername);
  return rc;
}

#ifdef KERBEROS
int
do_krb_login(int infd, struct auth_data *ap)
{
  int rc;
  char instance[INST_SZ], version[VERSION_SIZE];
  long authopts = 0L;		/* !mutual */
  struct sockaddr_in faddr;
  u_char auth_buf[sizeof (AUTH_DAT)];
  u_char tick_buf[sizeof (KTEXT_ST)];
  Key_schedule schedule;
  AUTH_DAT *kdata;

  kdata = (AUTH_DAT *) auth_buf;
  ticket = (KTEXT) tick_buf;

  instance[0] = '*';
  instance[1] = '\0';

#ifdef CRYPT
  if (encrypt)
    {
      rc = sizeof (faddr);
      if (getsockname (0, (struct sockaddr *) &faddr, &rc))
	return -1;
      authopts = KOPT_DO_MUTUAL;
      rc = krb_recvauth (authopts, 0,
			 ticket, "rcmd",
			 instance, ap->from, &faddr,
			 kdata, "", schedule, version);
      des_set_key (kdata->session, schedule);

    }
  else
#endif
    rc = krb_recvauth (authopts, 0,
		       ticket, "rcmd",
		       instance, ap->from, NULL,
		       kdata, "", NULL, version);

  if (rc != KSUCCESS)
    return rc;

  getstr (infd, &ap->lusername, NULL);
  /* get the "cmd" in the rcmd protocol */
  getstr (infd, &ap->term, "TERM=");

  pwd = getpwnam (ap->lusername);
  if (pwd == NULL)
    return -1;

  /* returns nonzero for no access */
  if (kuserok (kdata, ap->lusername) != 0)
    return -1;

  if (pwd->pw_uid == 0)
    syslog (LOG_INFO|LOG_AUTH,
	    "ROOT Kerberos login from %s.%s@%s on %s\n",
	    kdata->pname, kdata->pinst, kdata->prealm, ap->hostname);
  else
    syslog (LOG_INFO|LOG_AUTH,
	    "%s Kerberos login from %s.%s@%s on %s\n",
	    pwd->pw_name,
	    kdata->pname, kdata->pinst, kdata->prealm, ap->hostname);

  return 0;
}
#endif

#define BUFFER_SIZE 128

void
getstr(int infd, char **ptr, char *prefix)
{
  char c;
  char *buf;
  int pos;
  int size = BUFFER_SIZE;

  if (prefix)
    {
      int len = strlen(prefix);
      if (size < len + 1)
	size = len + 1;
    }

  buf = malloc(size);
  if (!buf)
    {
      syslog(LOG_ERR, "not enough memory");
      exit(1);
    }

  pos = 0;
  if (prefix)
    {
      strcpy(buf, prefix);
      pos += strlen(buf);
    }

  do
    {
      if (read (infd, &c, 1) != 1)
	{
	  syslog(LOG_ERR, "read error: %m");
	  exit(1);
	}
      if (pos == size)
	{
	  size += BUFFER_SIZE;
	  buf = realloc(buf, size);
	  if (!buf)
	    {
	      syslog(LOG_ERR, "not enough memory");
	      exit(1);
	    }
	}
      buf[pos++] = c;
    }
  while (c != 0);
  *ptr = buf;
}

#define	pkcontrol(c) ((c)&(TIOCPKT_FLUSHWRITE|TIOCPKT_NOSTOP|TIOCPKT_DOSTOP))

char magic[2] = {0377, 0377};
char oobdata[] = {TIOCPKT_WINDOW}; /* May be modified by protocol/control */

void
protocol(int f, int p)
{
  char fibuf[1024], *pbp, *fbp;
  register pcc = 0, fcc = 0;
  int cc, nfd, n;
  char cntl;

  /*
   * Must ignore SIGTTOU, otherwise we'll stop
   * when we try and set slave pty's window shape
   * (our controlling tty is the master pty).
   */
  signal (SIGTTOU, SIG_IGN);
  send (f, oobdata, 1, MSG_OOB);	/* indicate new rlogin */
  if (f > p)
    nfd = f + 1;
  else
    nfd = p + 1;
  if (nfd > FD_SETSIZE)
    {
      syslog (LOG_ERR, "select mask too small, increase FD_SETSIZE");
      fatal (f, "internal error (select mask too small)", 0);
    }

  while (1)
    {
      fd_set ibits, obits, ebits, *omask;

      FD_ZERO (&ebits);
      FD_ZERO (&ibits);
      FD_ZERO (&obits);
      omask = (fd_set *) NULL;

      if (fcc)
	{
	  FD_SET (p, &obits);
	  omask = &obits;
	}
      else
	FD_SET (f, &ibits);

      if (pcc >= 0)
	if (pcc)
	  {
	    FD_SET (f, &obits);
	    omask = &obits;
	  }
	else
	  FD_SET (p, &ibits);

      FD_SET (p, &ebits);

      if ((n = select (nfd, &ibits, omask, &ebits, 0)) < 0)
	{
	  if (errno == EINTR)
	    continue;
	  fatal (f, "select", 1);
	}
      if (n == 0)
	{
	  /* shouldn't happen... */
	  sleep (5);
	  continue;
	}

      if (FD_ISSET (p, &ebits))
	{
	  cc = read (p, &cntl, 1);
	  if (cc == 1 && pkcontrol (cntl))
	    {
	      cntl |= oobdata[0];
	      send (f, &cntl, 1, MSG_OOB);
	      if (cntl & TIOCPKT_FLUSHWRITE)
		{
		  pcc = 0;
		  FD_CLR (p, &ibits);
		}
	    }
	}

      if (FD_ISSET (f, &ibits))
	{
	  ENC_READ (fcc, f, fibuf, sizeof (fibuf));

	  if (fcc < 0 && errno == EWOULDBLOCK)
	    fcc = 0;
	  else
	    {
	      register char *cp;
	      int left, n;

	      if (fcc <= 0)
		break;
	      fbp = fibuf;

	      for (cp = fibuf; cp < fibuf + fcc - 1; cp++)
		if (cp[0] == magic[0] && cp[1] == magic[1])
		  {
		    left = fcc - (cp - fibuf);
		    n = control (p, cp, left);
		    if (n)
		      {
			left -= n;
			if (left > 0)
			  bcopy (cp + n, cp, left);
			fcc -= n;
			cp--;
		      }
		  }
	      FD_SET (p, &obits);	/* try write */
	    }
	}

      if (FD_ISSET (p, &obits) && fcc > 0)
	{
	  cc = write (p, fbp, fcc);
	  if (cc > 0)
	    {
	      fcc -= cc;
	      fbp += cc;
	    }
	}

      if (FD_ISSET (p, &ibits))
	{
	  char dbuf[1024 + 1];

	  pcc = read (p, dbuf, sizeof(dbuf));

	  pbp = dbuf;
	  if (pcc < 0)
	    {
	      if (errno == EWOULDBLOCK)
		pcc = 0;
	      else
		break;
	    }
	  else if (pcc == 0)
	    {
	      break;
	    }
	  else if (dbuf[0] == 0)
	    {
	      pbp++;
	      pcc--;
	      IF_NOT_ENCRYPT(FD_SET(f, &obits));	/* try write */
	    }
	  else
	    {
	      if (pkcontrol(dbuf[0]))
		{
		  dbuf[0] |= oobdata[0];
		  send (f, &dbuf[0], 1, MSG_OOB);
		}
	      pcc = 0;
	    }
	}

      if ((FD_ISSET (f, &obits)) && pcc > 0)
	{
	  ENC_WRITE(cc, f, pbp, pcc);

	  if (cc < 0 && errno == EWOULDBLOCK)
	    {
	      /*
	       * This happens when we try write after read
	       * from p, but some old kernels balk at large
	       * writes even when select returns true.
	       */
	      if (!FD_ISSET (p, &ibits))
		sleep (5);
	      continue;
	    }
	  if (cc > 0)
	    {
	      pcc -= cc;
	      pbp += cc;
	    }
	}
    }
}

/* Handle a "control" request (signaled by magic being present)
   in the data stream.  For now, we are only willing to handle
   window size changes. */
int
control (int pty, char *cp, int n)
{
  struct winsize w;

  if (n < 4 + sizeof (w) || cp[2] != 's' || cp[3] != 's')
    return (0);
  oobdata[0] &= ~TIOCPKT_WINDOW;	/* we know he heard */
  memmove (&w, cp + 4, sizeof (w));
  w.ws_row = ntohs (w.ws_row);
  w.ws_col = ntohs (w.ws_col);
  w.ws_xpixel = ntohs (w.ws_xpixel);
  w.ws_ypixel = ntohs (w.ws_ypixel);
  ioctl (pty, TIOCSWINSZ, &w);
  return (4 + sizeof (w));
}

RETSIGTYPE
cleanup (int signo)
{
  char *p;

  p = line + sizeof (PATH_DEV) - 1;
#ifdef UTMPX
  utmp_logout (p);
  chmod (line, 0644);
  chown (line, 0, 0);
#else
  if (logout (p))
    logwtmp (p, "", "");
  chmod (line, 0666);
  chown (line, 0, 0);
  *p = 'p';
  chmod (line, 0666);
  chown (line, 0, 0);
#endif
  shutdown (netf, 2);
  exit (1);
}

int
in_local_domain(char *hostname)
{
  char *p = topdomain(hostname, local_dot_count);
  return p && strcasecmp(p, local_domain_name) == 0;
}

char *
topdomain(char *name, int max_dots)
{
  char *p;
  int dot_count = 0;

  for (p = name + strlen(name) - 1; p >= name; p--)
    {
      if (*p == '.' && ++dot_count == max_dots)
	return p+1;
    }
  return NULL;
}

void
fatal (int f, char *msg, int syserr)
{
  int len;
  char buf[BUFSIZ], *bp = buf;

  /*
   * Prepend binary one to message if we haven't sent
   * the magic null as confirmation.
   */
  if (!confirmed)
    *bp++ = '\01';		/* error indicator */
  if (syserr)
    snprintf (bp, sizeof buf - (bp - buf),
	      "rlogind: %s: %s.\r\n", msg, strerror (errno));
  else
    snprintf (bp, sizeof buf - (bp - buf), "rlogind: %s.\r\n", msg);
  len = strlen (bp);
  write (f, buf, bp + len - buf);
  exit (1);
}

static char usage_str[] =
"usage: rlogind [options]\n"
"\n"
"Options are:\n"
"   -a, --verify-hostname   Ask hostname for verification\n"
"   -d, --daemon            Daemon mode\n"
"   -l, --no-rhosts         Ignore .rhosts file\n"
"   -L, --local-domain NAME Set local domain name\n"
"   -n, --no-keepalive      Do not set SO_KEEPALIVE\n"
#ifdef KERBEROS
"   -k, --kerberos          Use kerberos IV authentication\n"
#ifdef CRYPT
"   -x, --encrypt           Use DES encryption\n"
#endif /* CRYPT */
#endif /* KERBEROS */
"   -D, --debug[=LEVEL]     Set debug level\n"
"   -h, --help              Display usage instructions\n"
"   -V, --version           Display program version\n"
"   -p, --port PORT         Listen on given port (valid only in daemon mode)\n"
"   -r, reverse-required    Require reverse resolving of a remote host IP\n";

void
usage()
{
  printf("%s\n"
	 "Send bug reports to %s\n",
	 usage_str, inetutils_bugaddr);
}


