/*-
 * Copyright (c) 1988, 1989, 1992, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static char copyright[] =
"@(#) Copyright (c) 1988, 1989, 1992, 1993, 1994\n\
	The Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)rshd.c	8.2 (Berkeley) 4/6/94";
#endif /* not lint */

/*
 * remote shell server:
 *	[port]\0
 *	remuser\0
 *	locuser\0
 *	command\0
 *	data
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if !defined (__GNUC__) && defined (_AIX)
 #pragma alloca
#endif
#ifndef alloca /* Make alloca work the best possible way.  */
#ifdef __GNUC__
#define alloca __builtin_alloca
#else /* not __GNUC__ */
#if HAVE_ALLOCA_H
#include <alloca.h>
#else /* not __GNUC__ or HAVE_ALLOCA_H */
#ifndef _AIX /* Already did AIX, up at the top.  */
char *alloca ();
#endif /* not _AIX */
#endif /* not HAVE_ALLOCA_H */
#endif /* not __GNUC__ */
#endif /* not alloca */

#include <sys/param.h>
#include <sys/ioctl.h>
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
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#include <pwd.h>
#include <signal.h>
#if defined(HAVE_STDARG_H) && defined(__STDC__) && __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <getopt.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

int	keepalive = 1;		/* flag for SO_KEEPALIVE scoket option */
int	check_all;
int	log_success;		/* If TRUE, log all successful accesses */
int	sent_null;

void	 doit __P((int, struct sockaddr_in *));
void	 error __P((const char *, ...));
char	*getstr __P((char *));
int	 local_domain __P((const char *));
const char *topdomain __P((const char *));
void	 usage __P((void));

#ifdef	KERBEROS
#include <kerberosIV/des.h>
#include <kerberosIV/krb.h>
#define	VERSION_SIZE	9
#define SECURE_MESSAGE  "This rsh session is using DES encryption for all transmissions.\r\n"
#define	OPTIONS		"alnkvxL"
char	authbuf[sizeof(AUTH_DAT)];
char	tickbuf[sizeof(KTEXT_ST)];
int	doencrypt, use_kerberos, vacuous;
Key_schedule	schedule;
#else
#define	OPTIONS	"alnL"
#endif

/* Remote shell server. We're invoked by the rcmd(3) function. */
int
main(argc, argv)
	int argc;
	char *argv[];
{
	extern int __check_rhosts_file; /* hook in rcmd(3) */
	struct linger linger;
	int ch, on = 1, fromlen;
	struct sockaddr_in from;
	int sockfd;

	openlog("rshd", LOG_PID | LOG_ODELAY, LOG_DAEMON);

	opterr = 0;
	while ((ch = getopt(argc, argv, OPTIONS)) != EOF)
		switch (ch) {
		case 'a':
			check_all = 1;
			break;
		case 'l':
		  __check_rhosts_file = 0; /* don't check .rhosts file */
			break;
		case 'n':
		  keepalive = 0; /* don't enable SO_KEEPALIVE */
			break;
#ifdef	KERBEROS
		case 'k':
			use_kerberos = 1;
			break;

		case 'v':
			vacuous = 1;
			break;

#ifdef ENCRYPTION
		case 'x':
			doencrypt = 1;
			break;
#endif
#endif
		case 'L':
			log_success = 1;
			break;
		case '?':
		default:
			usage();
			break;
		}

	argc -= optind;
	argv += optind;

#ifdef	KERBEROS
	if (use_kerberos && vacuous) {
		syslog(LOG_ERR, "only one of -k and -v allowed");
		exit(2);
	}
#ifdef ENCRYPTION
	if (doencrypt && !use_kerberos) {
		syslog(LOG_ERR, "-k is required for -x");
		exit(2);
	}
#endif
#endif

	/*
	 * We assume we're invoked by inetd, so the socket that the
	 * connection is on, is open on descriptors 0, 1 and 2.
	 * STD{IN,OUT,ERR}_FILENO.
	 * We may in the future make it standalone for certain platform.
	 */
	sockfd = STDIN_FILENO;

	/*
	 * First get the Internet address of the client process.
	 * This is requored for all the authentication we perform.
	 */

	fromlen = sizeof (from);
	if (getpeername(sockfd, (struct sockaddr *)&from, &fromlen) < 0) {
		syslog(LOG_ERR, "getpeername: %m");
		_exit(1);
	}

	/* Set the socket options: SO_KEEPALIVE and SO_LINGER */
	if (keepalive &&
	    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (char *)&on,
	    sizeof(on)) < 0)
		syslog(LOG_WARNING, "setsockopt (SO_KEEPALIVE): %m");
	linger.l_onoff = 1;
	linger.l_linger = 60;			/* XXX */
	if (setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (char *)&linger,
	    sizeof (linger)) < 0)
		syslog(LOG_WARNING, "setsockopt (SO_LINGER): %m");
	doit(sockfd, &from);
	/* NOTREACHED */
}

char	username[20] = "USER=";
char	logname[23] = "LOGNAME=";
char	homedir[64] = "HOME=";
char	shell[64] = "SHELL=";
char	path[100] = "PATH=";
char	*envinit[] =
	    {homedir, shell, path, logname, username, 0};
extern char	**environ;

void
doit(sockfd, fromp)
	int sockfd;
	struct sockaddr_in *fromp;
{
	extern char *__rcmd_errstr;	/* syslog hook from libc/net/rcmd.c. */
	struct hostent *hp;
	struct passwd *pwd;
	u_short port;
	fd_set ready, readfrom;
	int cc, nfd, pv[2], pid, s = sockfd;
	int one = 1;
	const char *hostname, *errorstr, *errorhost;
	char *cp, sig, buf[BUFSIZ];
	char *cmdbuf, *locuser, *remuser;

#ifdef	KERBEROS
	AUTH_DAT	*kdata = (AUTH_DAT *) NULL;
	KTEXT		ticket = (KTEXT) NULL;
	char		instance[INST_SZ], version[VERSION_SIZE];
	struct		sockaddr_in	fromaddr;
	int		rc;
	long		authopts;
	int		pv1[2], pv2[2];
	fd_set		wready, writeto;

	fromaddr = *fromp;
#endif

	(void) signal(SIGINT, SIG_DFL);
	(void) signal(SIGQUIT, SIG_DFL);
	(void) signal(SIGTERM, SIG_DFL);
#ifdef DEBUG
	{ int t = open(PATH_TTY, O_RDWR);
	  if (t >= 0) {
		ioctl(t, TIOCNOTTY, (char *)0);
		(void) close(t);
	  }
	}
#endif
	/* Verify that the client's address is an Internet adress. */
	if (fromp->sin_family != AF_INET) {
		syslog(LOG_ERR, "malformed \"from\" address (af %d)\n",
		    fromp->sin_family);
		exit(1);
	}
#ifdef IP_OPTIONS
      {
	u_char optbuf[BUFSIZ/3], *cp;
	char lbuf[BUFSIZ], *lp;
	int optsize = sizeof(optbuf), ipproto;
	struct protoent *ip;

	if ((ip = getprotobyname("ip")) != NULL)
		ipproto = ip->p_proto;
	else
		ipproto = IPPROTO_IP;
	if (!getsockopt(sockfd, ipproto, IP_OPTIONS, (char *)optbuf,
		    &optsize) && optsize != 0) {
		lp = lbuf;
		/* The clent has set IP options.  This isn't allowd.
		 * Use syslog() to record the fact.
		 */
		for (cp = optbuf; optsize > 0; cp++, optsize--, lp += 3)
			sprintf(lp, " %2.2x", *cp);
		syslog(LOG_NOTICE,
		       "Connection received from %s using IP options (ignored):%s",
		    inet_ntoa(fromp->sin_addr), lbuf);

		/* Turn off the options.  If this doesn't work, we quit */
		if (setsockopt(sockfd, ipproto, IP_OPTIONS,
		    (char *)NULL, optsize) != 0) {
			syslog(LOG_ERR, "setsockopt IP_OPTIONS NULL: %m");
			exit(1);
		}
	}
      }
#endif

	/* Need host byte ordered port# to compare */
	fromp->sin_port = ntohs((u_short)fromp->sin_port);
	/* Verify that the client's address was bound to a reserved port */
#ifdef	KERBEROS
	if (!use_kerberos)
#endif
	  if (fromp->sin_port >= IPPORT_RESERVED ||
	      fromp->sin_port < IPPORT_RESERVED/2) {
	    syslog(LOG_NOTICE|LOG_AUTH,
		   "Connection from %s on illegal port %u",
		   inet_ntoa(fromp->sin_addr), fromp->sin_port);
	    exit(1);
	  }

	/* Read the ASCII string specifying the secondary port# from
	 * the socket.  We set a timer of 60 seconds to do this read,
	 * else we assume something is wrong.  If the client doesn't want
	 * the secondary port, they just send the terminating null byte.
	 */
	(void) alarm(60);
	port = 0;
	for (;;) {
		char c;
		if ((cc = read(sockfd, &c, 1)) != 1) {
			if (cc < 0)
				syslog(LOG_NOTICE, "read: %m");
			shutdown(sockfd, 2);
			exit(1);
		}
		/* null byte terminates the string */
		if (c== 0)
			break;
		port = port * 10 + c - '0';
	}

	(void) alarm(0);
	if (port != 0) {
	  /* If the secondary port# is nonzero, the we have to
	   * connect to that port (which the client has already
	   * created and is listening on).  The secondary port#
	   * that the client tells us to connect to has to also be
	   * a reserved port#.  Also, our end of this secondary
	   * connection has to also have a reserved TCP port bond
	   * to it, plus.
	   */
		int lport = IPPORT_RESERVED - 1;
		s = rresvport(&lport);
		if (s < 0) {
			syslog(LOG_ERR, "can't get stderr port: %m");
			exit(1);
		}
#ifdef	KERBEROS
		if (!use_kerberos)
#endif
			if (port >= IPPORT_RESERVED ||
				port < IPPORT_RESERVED/2) {
				syslog(LOG_ERR, "2nd port not reserved\n");
				exit(1);
			}
		/* Use the fromp structure taht we already have.
		 * The 32-bit Internet address is obviously that of the
		 * client's, just change the port# to the one specified
		 * by the clent as the secondary port.
		 */
		fromp->sin_port = htons(port);
		if (connect(s, (struct sockaddr *)fromp,
			    sizeof (*fromp)) < 0) {
			syslog(LOG_INFO, "connect second port %d: %m", port);
			exit(1);
		}
	}

#ifdef	KERBEROS
	if (vacuous) {
		error("rshd: remote host requires Kerberos authentication\n");
		exit(1);
	}
#endif

	/* from inetd, socket is already on 0, 1, 2 */
	if (sockfd != STDIN_FILENO) {
	    dup2(sockfd, STDIN_FILENO);
	    dup2(sockfd, STDOUT_FILENO);
	    dup2(sockfd, STDERR_FILENO);
	}

	/* Get the "name" of the clent form its Internet address.
	 * This is used for the autentication below
	 */
	errorstr = NULL;
	hp = gethostbyaddr((char *)&fromp->sin_addr, sizeof (struct in_addr),
		fromp->sin_family);
	if (hp) {
		/*
		 * If name returned by gethostbyaddr is in our domain,
		 * attempt to verify that we haven't been fooled by someone
		 * in a remote net; look up the name and check that this
		 * address corresponds to the name.
		 */
		hostname = hp->h_name;
#ifdef	KERBEROS
		if (!use_kerberos)
#endif
		if (check_all || local_domain(hp->h_name)) {
			char *remotehost = (char *) alloca (strlen (hp->h_name) + 1);
			if (! remotehost)
				errorstr = "Out of memory\n";
			else {
				strcpy(remotehost, hp->h_name);
				errorhost = remotehost;
				hp = gethostbyname(remotehost);
				if (hp == NULL) {
					syslog(LOG_INFO,
					    "Couldn't look up address for %s",
					    remotehost);
					errorstr =
			       "Couldn't look up address for your host (%s)\n";
					hostname = inet_ntoa(fromp->sin_addr);
				} else for (; ; hp->h_addr_list++) {
					if (hp->h_addr_list[0] == NULL) {
						syslog(LOG_NOTICE,
					 "Host addr %s not listed for host %s",
						    inet_ntoa(fromp->sin_addr),
						    hp->h_name);
						errorstr =
					      "Host address mismatch for %s\n";
						hostname = inet_ntoa(fromp->sin_addr);
						break;
					}
					if (!memcmp(hp->h_addr_list[0],
					    (caddr_t)&fromp->sin_addr,
					    sizeof(fromp->sin_addr))) {
						hostname = hp->h_name;
						break; /* equal, OK */
					}
				}
			}
		}
	} else
		errorhost = hostname = inet_ntoa(fromp->sin_addr);

#ifdef	KERBEROS
	if (use_kerberos) {
		kdata = (AUTH_DAT *) authbuf;
		ticket = (KTEXT) tickbuf;
		authopts = 0L;
		strcpy(instance, "*");
		version[VERSION_SIZE - 1] = '\0';
#ifdef ENCRYPTION
		if (doencrypt) {
			struct sockaddr_in local_addr;
			rc = sizeof(local_addr);
			if (getsockname(STDIN_FILENO,
				    (struct sockaddr *)&local_addr, &rc) < 0) {
				syslog(LOG_ERR, "getsockname: %m");
				error("rlogind: getsockname: %m");
				exit(1);
			}
			authopts = KOPT_DO_MUTUAL;
			rc = krb_recvauth(authopts, 0, ticket,
				"rcmd", instance, &fromaddr,
				&local_addr, kdata, "", schedule,
				version);
			des_set_key(kdata->session, schedule);
		} else
#endif
			rc = krb_recvauth(authopts, 0, ticket, "rcmd",
				instance, &fromaddr,
				(struct sockaddr_in *) 0,
				kdata, "", (bit_64 *) 0, version);
		if (rc != KSUCCESS) {
			error("Kerberos authentication failure: %s\n",
				  krb_err_txt[rc]);
			exit(1);
		}
	} else
#endif
		remuser = getstr ("remuser");

	/* Read three strings from the client. */
	locuser = getstr ("locuser");
	cmdbuf = getstr ("command");

	/* Look up locuser in the passerd file.  The locuser has\* to be a
	 * valid account on this system.
	 */
	setpwent();
	pwd = getpwnam(locuser);
	if (pwd == NULL) {
		syslog(LOG_INFO|LOG_AUTH,
		    "%s@%s as %s: unknown login. cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
		if (errorstr == NULL)
			errorstr = "Login incorrect.\n";
		goto fail;
	}

	/* We'll execute the client's command in the home directory
	 * of locuser.
	 */
	if (chdir(pwd->pw_dir) < 0) {
		(void) chdir("/");
		syslog(LOG_INFO|LOG_AUTH,
		    "%s@%s as %s: no home directory. cmd='%.80s'",
		    remuser, hostname, locuser, cmdbuf);
		error("No remote directory.\n");
#ifdef notdef
		exit(1);
#endif
	}

#ifdef	KERBEROS
	if (use_kerberos) {
		if (pwd->pw_passwd != 0 && *pwd->pw_passwd != '\0') {
			if (kuserok(kdata, locuser) != 0) {
				syslog(LOG_INFO|LOG_AUTH,
				    "Kerberos rsh denied to %s.%s@%s",
				    kdata->pname, kdata->pinst, kdata->prealm);
				error("Permission denied.\n");
				exit(1);
			}
		}
	} else
#endif

		if (errorstr ||
		    pwd->pw_passwd != 0 && *pwd->pw_passwd != '\0' &&
		    iruserok(fromp->sin_addr.s_addr, pwd->pw_uid == 0,
		    remuser, locuser) < 0) {
			if (__rcmd_errstr)
				syslog(LOG_INFO|LOG_AUTH,
			    "%s@%s as %s: permission denied (%s). cmd='%.80s'",
				    remuser, hostname, locuser, __rcmd_errstr,
				    cmdbuf);
			else
				syslog(LOG_INFO|LOG_AUTH,
			    "%s@%s as %s: permission denied. cmd='%.80s'",
				    remuser, hostname, locuser, cmdbuf);
fail:
			if (errorstr == NULL)
				errorstr = "Permission denied.\n";
			error(errorstr, errorhost);
			exit(1);
		}

	/* If the locuser isn't root, then check if logins are disabled. */
	if (pwd->pw_uid && !access(PATH_NOLOGIN, F_OK)) {
		error("Logins currently disabled.\n");
		exit(1);
	}

	/* Now write the null byte back to the client telling it
	 * that everything is OK.
	 * Note that this means that any error message that we generate
	 * from now on (such as the perror() if the execl() fails), won't
	 * be seen by the rcomd() fucntion, but will be seen by the
	 * application that called rcmd() when it reads from the socket.
	 */
	if (write(STDERR_FILENO, "\0", 1) < 0) {
		error("Lost connection.\n");
		exit(1);
	}
	sent_null = 1;

	if (port) {
	      /* We nee a secondary channel,  Here's where we create
	       * the control process that'll handle this secondary
	       * channel.
	       * First create a pipe to use for communication between
	       * the parent and child, then fork.
	       */
		if (pipe(pv) < 0) {
			error("Can't make pipe.\n");
			exit(1);
		}
#ifdef ENCRYPTION
#ifdef KERBEROS
		if (doencrypt) {
			if (pipe(pv1) < 0) {
				error("Can't make 2nd pipe.\n");
				exit(1);
			}
			if (pipe(pv2) < 0) {
				error("Can't make 3rd pipe.\n");
				exit(1);
			}
		}
#endif
#endif
		pid = fork();
		if (pid == -1)  {
			error("Can't fork; try again.\n");
			exit(1);
		}
		if (pid) {
		      /* Parent process == control process.
		       * We: (1) read from the pipe and write to s;
		       *     (2) read from s and send corresponding
		       *         signal.
		       */
#ifdef ENCRYPTION
#ifdef KERBEROS
			if (doencrypt) {
				static char msg[] = SECURE_MESSAGE;
				(void) close(pv1[1]);
				(void) close(pv2[1]);
				des_write(s, msg, sizeof(msg) - 1);

			} else
#endif
#endif
			{
			        /* child handles the original socket */
				(void) close(STDIN_FILENO);
				/* (0, 1, and 2 were from inetd */
				(void) close(STDOUT_FILENO);
			}
			(void) close(STDERR_FILENO);
			(void) close(pv[1]); /* close write end of pipe */

			FD_ZERO(&readfrom);
			FD_SET(s, &readfrom);
			FD_SET(pv[0], &readfrom);
			/* set max fd + 1 for select */
			if (pv[0] > s)
				nfd = pv[0];
			else
				nfd = s;
#ifdef ENCRYPTION
#ifdef KERBEROS
			if (doencrypt) {
				FD_ZERO(&writeto);
				FD_SET(pv2[0], &writeto);
				FD_SET(pv1[0], &readfrom);

				nfd = MAX(nfd, pv2[0]);
				nfd = MAX(nfd, pv1[0]);
			} else
#endif
#endif
				ioctl(pv[0], FIONBIO, (char *)&one);

			/* should set s nbio! */
			nfd++;
			do {
				ready = readfrom;
#ifdef ENCRYPTION
#ifdef KERBEROS
				if (doencrypt) {
					wready = writeto;
					if (select(nfd, &ready,
					    &wready, (fd_set *) 0,
					    (struct timeval *) 0) < 0)
						break;
				} else
#endif
#endif
					if (select(nfd, &ready, (fd_set *)0,
					  (fd_set *)0, (struct timeval *)0) < 0)
					    /* wait until something to read */
						break;
				if (FD_ISSET(s, &ready)) {
					int	ret;
#ifdef ENCRYPTION
#ifdef KERBEROS
					if (doencrypt)
						ret = des_read(s, &sig, 1);
					else
#endif
#endif
						ret = read(s, &sig, 1);
					if (ret <= 0)
						FD_CLR(s, &readfrom);
					else
						killpg(pid, sig);
				}
				if (FD_ISSET(pv[0], &ready)) {
					errno = 0;
					cc = read(pv[0], buf, sizeof(buf));
					if (cc <= 0) {
						shutdown(s, 1+1);
						FD_CLR(pv[0], &readfrom);
					} else {
#ifdef ENCRYPTION
#ifdef KERBEROS
						if (doencrypt)
							(void)
							  des_write(s, buf, cc);
						else
#endif
#endif
							(void)
							  write(s, buf, cc);
					}
				}
#ifdef ENCRYPTION
#ifdef KERBEROS
				if (doencrypt && FD_ISSET(pv1[0], &ready)) {
					errno = 0;
					cc = read(pv1[0], buf, sizeof(buf));
					if (cc <= 0) {
						shutdown(pv1[0], 1+1);
						FD_CLR(pv1[0], &readfrom);
					} else
						(void) des_write(STDOUT_FILENO,
						    buf, cc);
				}

				if (doencrypt && FD_ISSET(pv2[0], &wready)) {
					errno = 0;
					cc = des_read(STDIN_FILENO,
					    buf, sizeof(buf));
					if (cc <= 0) {
						shutdown(pv2[0], 1+1);
						FD_CLR(pv2[0], &writeto);
					} else
						(void) write(pv2[0], buf, cc);
				}
#endif
#endif

			} while (FD_ISSET(s, &readfrom) ||
#ifdef ENCRYPTION
#ifdef KERBEROS
			    (doencrypt && FD_ISSET(pv1[0], &readfrom)) ||
#endif
#endif
			    FD_ISSET(pv[0], &readfrom));
			/* The pipe will generat an EOR whe the shell
			 * terminates.  The socket will terninate whe the
			 * client process terminates.
			 */
			exit(0);
		}

		/* Child process. Become a process group leader, so that
		 * the control process above can send signals to all the
		 * processes we may be the parent of.  The process group ID
		 * (the getpid() value below) equals the childpid value from
		 * the fork above.
		 */
		setpgid (0, getpid ());
		(void) close(s); /* control process handles this fd */
		(void) close(pv[0]); /* close read end of pipe */
#ifdef ENCRYPTION
#ifdef KERBEROS
		if (doencrypt) {
			close(pv1[0]); close(pv2[0]);
			dup2(pv1[1], 1);
			dup2(pv2[1], 0);
			close(pv1[1]);
			close(pv2[1]);
		}
#endif
#endif
		dup2(pv[1], STDERR_FILENO); /* stderr of shell has to go
					       pipe to control process */
		close(pv[1]);
	}
	if (*pwd->pw_shell == '\0')
		pwd->pw_shell = PATH_BSHELL;
#if	BSD > 43
	if (setlogin(pwd->pw_name) < 0)
		syslog(LOG_ERR, "setlogin() failed: %m");
#endif

	/* Set the fid, then uid to become the user specified by "locuser" */
	(void) setegid((gid_t)pwd->pw_gid);
	(void) setgid((gid_t)pwd->pw_gid);
	initgroups(pwd->pw_name, pwd->pw_gid); /* BSD groups */
	(void) seteuid((uid_t)pwd->pw_uid);
	(void) setuid((uid_t)pwd->pw_uid);

	/* Set up an initial environment for the shell that we exec() */
	environ = envinit;
	strncat(homedir, pwd->pw_dir, sizeof(homedir)-6);
	strcat(path, PATH_DEFPATH);
	strncat(shell, pwd->pw_shell, sizeof(shell)-7);
	strncat(username, pwd->pw_name, sizeof(username)-6);
	cp = strrchr(pwd->pw_shell, '/');
	if (cp)
		cp++; /* step past first slash */
	else
		cp = pwd->pw_shell; /* no slash in shell string */
	endpwent();
	if (log_success || pwd->pw_uid == 0) {
#ifdef	KERBEROS
		if (use_kerberos)
		    syslog(LOG_INFO|LOG_AUTH,
			"Kerberos shell from %s.%s@%s on %s as %s, cmd='%.80s'",
			kdata->pname, kdata->pinst, kdata->prealm,
			hostname, locuser, cmdbuf);
		else
#endif
		    syslog(LOG_INFO|LOG_AUTH, "%s@%s as %s: cmd='%.80s'",
			remuser, hostname, locuser, cmdbuf);
	}
	execl(pwd->pw_shell, cp, "-c", cmdbuf, 0);
	perror(pwd->pw_shell);
	exit(1);
}

/*
 * Report error to client.  Note: can't be used until second socket has
 * connected to client, or older clients will hang waiting for that
 * connection first.
 */

void
#if defined(HAVE_STDARG_H) && defined(__STDC__) && __STDC__
error(const char *fmt, ...)
#else
error(fmt, va_alist)
	char *fmt;
        va_dcl
#endif
{
	va_list ap;
	int len;
	char *bp, buf[BUFSIZ];
#if defined(HAVE_STDARG_H) && defined(__STDC__) && __STDC__
	va_start(ap, fmt);
#else
	va_start(ap);
#endif
	bp = buf;
	if (sent_null == 0) {
		*bp++ = 1;
		len = 1;
	} else
		len = 0;
	(void)vsnprintf(bp, sizeof(buf) - 1, fmt, ap);
	(void)write(STDERR_FILENO, buf, len + strlen(bp));
}

char *
getstr(err)
	char *err;
{
	size_t buf_len = 100;
	char *buf = malloc (buf_len), *end = buf;

	if (! buf) {
		error ("Out of space reading %s\n", err);
		exit (1);
	}

	do {
		/* Oh this is efficient, oh yes.  [But what can be done?] */
		int rd = read(STDIN_FILENO, end, 1);
		if (rd <= 0) {
			if (rd == 0)
				error ("EOF reading %s\n", err);
			else
				perror (err);
			exit(1);
		}

		end += rd;
		if ((buf + buf_len - end) < (buf_len >> 3)) {
			/* Not very much room left in our buffer, grow it. */
			size_t end_offs = end - buf;
			buf_len += buf_len;
			buf = realloc (buf, buf_len);
			if (! buf) {
				error ("Out of space reading %s\n", err);
				exit (1);
			}
			end = buf + end_offs;
		}
	} while (*(end - 1));

	return buf;
}

/*
 * Check whether host h is in our local domain,
 * defined as sharing the last two components of the domain part,
 * or the entire domain part if the local domain has only one component.
 * If either name is unqualified (contains no '.'),
 * assume that the host is local, as it will be
 * interpreted as such.
 */
int
local_domain(h)
	const char *h;
{
	extern char *localhost ();
	char *hostname = localhost ();

	if (! hostname)
		return 0;
	else {
		int is_local = 0;
		const char *p1 = topdomain (hostname);
		const char *p2 = topdomain (h);

		if (p1 == NULL || p2 == NULL || !strcasecmp(p1, p2))
			is_local = 1;

		free (hostname);

		return is_local;
	}
}

const char *
topdomain(h)
	const char *h;
{
	const char *p, *maybe = NULL;
	int dots = 0;

	for (p = h + strlen(h); p >= h; p--) {
		if (*p == '.') {
			if (++dots == 2)
				return (p);
			maybe = p;
		}
	}
	return (maybe);
}

void
usage()
{

	syslog(LOG_ERR, "usage: rshd [-%s]", OPTIONS);
	exit(2);
}
