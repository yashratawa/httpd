/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

#include "apr_hash.h"
#include "apr_strings.h"
#include "apr_pools.h"
#include "apr_portable.h"
#include "apr_file_io.h"
#include "apr_signal.h"

#define APR_WANT_IOVEC
#include "apr_want.h"

#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif
#if APR_HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#define CORE_PRIVATE 
 
#include "ap_config.h"
#include "httpd.h" 
#include "http_main.h" 
#include "http_log.h" 
#include "http_config.h"	/* for read_config */ 
#include "http_core.h"		/* for get_remote_host */ 
#include "http_protocol.h"
#include "http_connection.h"
#include "ap_mpm.h"
#include "unixd.h"
#include "mpm_common.h"
#include "ap_listen.h"
#include "mpm_default.h"
#include "mpm.h"
#include "scoreboard.h"
#include "util_filter.h"

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

/* ### should be APR-ized */
#include <poll.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <setjmp.h>

/*
 * Actual definitions of config globals
 */

static int threads_to_start = 0;         /* Worker threads per child */
static int min_spare_threads = 0;
static int max_spare_threads = 0;
static int max_threads = 0;
static int max_requests_per_child = 0;
static const char *ap_pid_fname=NULL;
static int num_daemons=0;
static int curr_child_num=0;
static int workers_may_exit = 0;
static int requests_this_child;
static int num_listenfds = 0;
static apr_socket_t **listenfds;
static jmp_buf jmpbuffer;

struct child_info_t {
    uid_t uid;
    gid_t gid;
    int sd;
};

typedef struct {
    const char *sockname;    /* The base name for the socket */
    const char *fullsockname;   /* socket base name + extension */
    int        sd;       /* The socket descriptor */
    int        sd2;       /* The socket descriptor */
} perchild_server_conf;

typedef struct child_info_t child_info_t;

/* Tables used to determine the user and group each child process should
 * run as.  The hash table is used to correlate a server name with a child
 * process.
 */
static child_info_t child_info_table[HARD_SERVER_LIMIT];
static int          thread_socket_table[HARD_THREAD_LIMIT];


struct ap_ctable    ap_child_table[HARD_SERVER_LIMIT];

/*
 * The max child slot ever assigned, preserved across restarts.  Necessary
 * to deal with NumServers changes across SIGWINCH restarts.  We use this
 * value to optimize routines that have to scan the entire child table.
 *
 * XXX - It might not be worth keeping this code in. There aren't very
 * many child processes in this MPM.
 */
int ap_max_daemons_limit = -1;
int ap_threads_per_child = HARD_THREAD_LIMIT;

char ap_coredump_dir[MAX_STRING_LEN];

module AP_MODULE_DECLARE_DATA mpm_perchild_module;

static apr_file_t *pipe_of_death_in = NULL;
static apr_file_t *pipe_of_death_out = NULL;
static apr_lock_t *pipe_of_death_mutex;

/* *Non*-shared http_main globals... */

server_rec *ap_server_conf;

/* one_process --- debugging mode variable; can be set from the command line
 * with the -X flag.  If set, this gets you the child_main loop running
 * in the process which originally started up (no detach, no make_child),
 * which is a pretty nice debugging environment.  (You'll get a SIGHUP
 * early in standalone_main; just continue through.  This is the server
 * trying to kill off any child processes which it might have lying
 * around --- Apache doesn't keep track of their pids, it just sends
 * SIGHUP to the process group, ignoring it in the root process.
 * Continue through and you'll be fine.).
 */

static int one_process = 0;

#ifdef DEBUG_SIGSTOP
int raise_sigstop_flags;
#endif

static apr_pool_t *pconf;		/* Pool for config stuff */
static apr_pool_t *pchild;		/* Pool for httpd child stuff */
static apr_pool_t *thread_pool_parent; /* Parent of per-thread pools */
static apr_lock_t *thread_pool_parent_mutex;

static int child_num;
static unsigned int my_pid; /* Linux getpid() doesn't work except in 
                      main thread. Use this instead */
/* Keep track of the number of worker threads currently active */
static int worker_thread_count;
static apr_lock_t *worker_thread_count_mutex;
static int worker_thread_free_ids[HARD_THREAD_LIMIT];
static apr_threadattr_t *worker_thread_attr;

/* Keep track of the number of idle worker threads */
static int idle_thread_count;
static apr_lock_t *idle_thread_count_mutex;

/* Locks for accept serialization */
#ifdef NO_SERIALIZED_ACCEPT
#define SAFE_ACCEPT(stmt) APR_SUCCESS
#else
#define SAFE_ACCEPT(stmt) (stmt)
static apr_lock_t *process_accept_mutex;
#endif /* NO_SERIALIZED_ACCEPT */
static const char *lock_fname;
static apr_lock_t *thread_accept_mutex;

AP_DECLARE(int) ap_get_max_daemons(void)
{
    return ap_max_daemons_limit;
}

/* a clean exit from a child with proper cleanup */
static void clean_child_exit(int code)
{
    if (pchild) {
	apr_pool_destroy(pchild);
    }
    exit(code);
}

/* handle all varieties of core dumping signals */
static void sig_coredump(int sig)
{
    chdir(ap_coredump_dir);
    apr_signal(sig, SIG_DFL);
    kill(getpid(), sig);
    /* At this point we've got sig blocked, because we're still inside
     * the signal handler.  When we leave the signal handler it will
     * be unblocked, and we'll take the signal... and coredump or whatever
     * is appropriate for this particular Unix.  In addition the parent
     * will see the real signal we received -- whereas if we called
     * abort() here, the parent would only see SIGABRT.
     */
}

static void just_die(int sig)
{
    clean_child_exit(0);
}

/*****************************************************************
 * Connection structures and accounting...
 */

/* volatile just in case */
static int volatile shutdown_pending;
static int volatile restart_pending;
static int volatile is_graceful;
/* we don't currently track ap_my_generation, but mod_status 
 * references it so it must be defined */
ap_generation_t volatile ap_my_generation=0;

/*
 * ap_start_shutdown() and ap_start_restart(), below, are a first stab at
 * functions to initiate shutdown or restart without relying on signals. 
 * Previously this was initiated in sig_term() and restart() signal handlers, 
 * but we want to be able to start a shutdown/restart from other sources --
 * e.g. on Win32, from the service manager. Now the service manager can
 * call ap_start_shutdown() or ap_start_restart() as appropiate.  Note that
 * these functions can also be called by the child processes, since global
 * variables are no longer used to pass on the required action to the parent.
 *
 * These should only be called from the parent process itself, since the
 * parent process will use the shutdown_pending and restart_pending variables
 * to determine whether to shutdown or restart. The child process should
 * call signal_parent() directly to tell the parent to die -- this will
 * cause neither of those variable to be set, which the parent will
 * assume means something serious is wrong (which it will be, for the
 * child to force an exit) and so do an exit anyway.
 */

static void ap_start_shutdown(void)
{
    if (shutdown_pending == 1) {
	/* Um, is this _probably_ not an error, if the user has
	 * tried to do a shutdown twice quickly, so we won't
	 * worry about reporting it.
	 */
	return;
    }
    shutdown_pending = 1;
}

/* do a graceful restart if graceful == 1 */
static void ap_start_restart(int graceful)
{

    if (restart_pending == 1) {
	/* Probably not an error - don't bother reporting it */
	return;
    }
    restart_pending = 1;
    is_graceful = graceful;
    if (is_graceful) {
        apr_pool_cleanup_kill(pconf, NULL, ap_cleanup_scoreboard);
    }
}

static void sig_term(int sig)
{
    ap_start_shutdown();
}

static void restart(int sig)
{
#ifndef WIN32
    ap_start_restart(sig == SIGWINCH);
#else
    ap_start_restart(1);
#endif
}

static void set_signals(void)
{
#ifndef NO_USE_SIGACTION
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (!one_process) {
	sa.sa_handler = sig_coredump;
#if defined(SA_ONESHOT)
	sa.sa_flags = SA_ONESHOT;
#elif defined(SA_RESETHAND)
	sa.sa_flags = SA_RESETHAND;
#endif
	if (sigaction(SIGSEGV, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGSEGV)");
#ifdef SIGBUS
	if (sigaction(SIGBUS, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGBUS)");
#endif
#ifdef SIGABORT
	if (sigaction(SIGABORT, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGABORT)");
#endif
#ifdef SIGABRT
	if (sigaction(SIGABRT, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGABRT)");
#endif
#ifdef SIGILL
	if (sigaction(SIGILL, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGILL)");
#endif
	sa.sa_flags = 0;
    }
    sa.sa_handler = sig_term;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGTERM)");
#ifdef SIGINT
    if (sigaction(SIGINT, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGINT)");
#endif
#ifdef SIGXCPU
    sa.sa_handler = SIG_DFL;
    if (sigaction(SIGXCPU, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGXCPU)");
#endif
#ifdef SIGXFSZ
    sa.sa_handler = SIG_DFL;
    if (sigaction(SIGXFSZ, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGXFSZ)");
#endif
#ifdef SIGPIPE
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGPIPE)");
#endif

    /* we want to ignore HUPs and WINCH while we're busy processing one */
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaddset(&sa.sa_mask, SIGWINCH);
    sa.sa_handler = restart;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGHUP)");
    if (sigaction(SIGWINCH, &sa, NULL) < 0)
	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGWINCH)");
#else
    if (!one_process) {
	apr_signal(SIGSEGV, sig_coredump);
#ifdef SIGBUS
	apr_signal(SIGBUS, sig_coredump);
#endif /* SIGBUS */
#ifdef SIGABORT
	apr_signal(SIGABORT, sig_coredump);
#endif /* SIGABORT */
#ifdef SIGABRT
	apr_signal(SIGABRT, sig_coredump);
#endif /* SIGABRT */
#ifdef SIGILL
	apr_signal(SIGILL, sig_coredump);
#endif /* SIGILL */
#ifdef SIGXCPU
	apr_signal(SIGXCPU, SIG_DFL);
#endif /* SIGXCPU */
#ifdef SIGXFSZ
	apr_signal(SIGXFSZ, SIG_DFL);
#endif /* SIGXFSZ */
    }

    apr_signal(SIGTERM, sig_term);
#ifdef SIGHUP
    apr_signal(SIGHUP, restart);
#endif /* SIGHUP */
#ifdef SIGWINCH
    apr_signal(SIGWINCH, restart);
#endif /* SIGWINCH */
#ifdef SIGPIPE
    apr_signal(SIGPIPE, SIG_IGN);
#endif /* SIGPIPE */

#endif
}

/*****************************************************************
 * Here follows a long bunch of generic server bookkeeping stuff...
 */

int ap_graceful_stop_signalled(void)
{
    /* XXX - Does this really work? - Manoj */
    return is_graceful;
}

/*****************************************************************
 * Child process main loop.
 */

static void process_socket(apr_pool_t *p, apr_socket_t *sock, long conn_id)
{
    conn_rec *current_conn;
    int csd;
    apr_status_t rv;
    int thread_num = conn_id % HARD_THREAD_LIMIT;

    if ((rv = apr_os_sock_get(&csd, sock)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL, "apr_os_sock_get");
    }

    if (csd >= FD_SETSIZE) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, NULL,
                     "new file descriptor %d is too large; you probably need "
                     "to rebuild Apache with a larger FD_SETSIZE "
                     "(currently %d)", 
                     csd, FD_SETSIZE);
        apr_socket_close(sock);
        return;
    }

    if (thread_socket_table[thread_num] < 0) {
        ap_sock_disable_nagle(sock);
    }

    current_conn = ap_new_connection(p, ap_server_conf, sock, conn_id);
    if (current_conn) {
        ap_process_connection(current_conn);
        ap_lingering_close(current_conn);
    }
}

static void *worker_thread(void *);

/* Starts a thread as long as we're below max_threads */
static int start_thread(void)
{
    apr_thread_t *thread;
    int rc;

    apr_lock_acquire(worker_thread_count_mutex);
    if (worker_thread_count < max_threads - 1) {
        if ((rc = apr_thread_create(&thread, worker_thread_attr, worker_thread,
	               &worker_thread_free_ids[worker_thread_count], pchild))) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, rc, ap_server_conf,
                         "apr_thread_create: unable to create worker thread");
            /* In case system resources are maxxed out, we don't want
               Apache running away with the CPU trying to fork over and
               over and over again if we exit. */
            sleep(10);
            workers_may_exit = 1;
            apr_lock_release(worker_thread_count_mutex);
            return 0;
        }
	else {
	    worker_thread_count++;
	}
    }
    else {
        static int reported = 0;
        
        if (!reported) {
            ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_ERR, 0, ap_server_conf,
                         "server reached MaxThreadsPerChild setting, consider raising the"
                         " MaxThreadsPerChild or NumServers settings");
            reported = 1;
        }
        apr_lock_release(worker_thread_count_mutex);
        return 0;
    }
    apr_lock_release(worker_thread_count_mutex);
    return 1;

}
/* Sets workers_may_exit if we received a character on the pipe_of_death */
static void check_pipe_of_death(void)
{
    apr_lock_acquire(pipe_of_death_mutex);
    if (!workers_may_exit) {
        int ret;
        char pipe_read_char;
        apr_size_t n = 1;

        ret = apr_recv(listenfds[0], &pipe_read_char, &n);
        if (APR_STATUS_IS_EAGAIN(ret)) {
            /* It lost the lottery. It must continue to suffer
             * through a life of servitude. */
        }
        else {
            /* It won the lottery (or something else is very
             * wrong). Embrace death with open arms. */
            workers_may_exit = 1;
        }
    }
    apr_lock_release(pipe_of_death_mutex);
}

/* idle_thread_count should be incremented before starting a worker_thread */

static void *worker_thread(void *arg)
{
    apr_socket_t *csd = NULL;
    apr_pool_t *tpool;		/* Pool for this thread           */
    apr_pool_t *ptrans;		/* Pool for per-transaction stuff */
    apr_socket_t *sd = NULL;
    int srv;
    int curr_pollfd, last_pollfd = 0;
    int thread_just_started = 1;
    int thread_num = *((int *) arg);
    long conn_id = child_num * HARD_THREAD_LIMIT + thread_num;
    apr_pollfd_t *pollset;
    int n;
    apr_status_t rv;

    apr_lock_acquire(thread_pool_parent_mutex);
    apr_pool_create(&tpool, thread_pool_parent);
    apr_lock_release(thread_pool_parent_mutex);
    apr_pool_create(&ptrans, tpool);

    (void) ap_update_child_status(child_num, thread_num, SERVER_STARTING,
                                  (request_rec *) NULL);

    apr_poll_setup(&pollset, num_listenfds+1, tpool);
    for(n=0 ; n <= num_listenfds ; ++n) {
        apr_poll_socket_add(pollset, listenfds[n], APR_POLLIN);
    }

    while (!workers_may_exit) {
        workers_may_exit |= (max_requests_per_child != 0) && (requests_this_child <= 0);
        if (workers_may_exit) break;
        if (!thread_just_started) {
            apr_lock_acquire(idle_thread_count_mutex);
            if (idle_thread_count < max_spare_threads) {
                idle_thread_count++;
                apr_lock_release(idle_thread_count_mutex);
            }
            else {
                apr_lock_release(idle_thread_count_mutex);
                break;
            }
        }
        else {
            thread_just_started = 0;
        }

        (void) ap_update_child_status(child_num, thread_num, SERVER_READY,
                                      (request_rec *) NULL);

        apr_lock_acquire(thread_accept_mutex);
        if (workers_may_exit) {
            apr_lock_release(thread_accept_mutex);
            break;
        }
        if ((rv = SAFE_ACCEPT(apr_lock_acquire(process_accept_mutex)))
            != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ap_server_conf,
                         "apr_lock_acquire failed. Attempting to shutdown "
                         "process gracefully.");
            workers_may_exit = 1;
        }

        while (!workers_may_exit) {
            apr_int16_t event;
            srv = apr_poll(pollset, &n, -1);

            if (srv != APR_SUCCESS) {
                if (APR_STATUS_IS_EINTR(srv)) {
                    continue;
                }

                /* apr_poll() will only return errors in catastrophic
                 * circumstances. Let's try exiting gracefully, for now. */
                ap_log_error(APLOG_MARK, APLOG_ERR, srv, (const server_rec *)
                             ap_server_conf, "apr_poll: (listen)");
                workers_may_exit = 1;
            }
            if (workers_may_exit) break;

            apr_poll_revents_get(&event, listenfds[0], pollset);
            if (event & APR_POLLIN) {
                /* A process got a signal on the shutdown pipe. Check if we're
                 * the lucky process to die. */
                check_pipe_of_death();
                continue;
            }
            
            apr_poll_revents_get(&event, listenfds[1], pollset);
            if (event & APR_POLLIN || event & APR_POLLOUT) {
                /* This request is from another child in our current process.
                 * We should set a flag here, and then below we will read
                 * two bytes (the socket number and the NULL byte.
                 */
                thread_socket_table[thread_num] = -2;
                goto got_from_other_child;
            }

            if (num_listenfds == 1) {
                sd = ap_listeners->sd;
                goto got_fd;
            }
            else {
                /* find a listener */
                curr_pollfd = last_pollfd;
                do {
                    curr_pollfd++;
                    if (curr_pollfd > num_listenfds) {
                        curr_pollfd = 1;
                    }
                    /* XXX: Should we check for POLLERR? */
                    apr_poll_revents_get(&event, listenfds[curr_pollfd], pollset);
                    if (event & APR_POLLIN) {
                        last_pollfd = curr_pollfd;
                        sd = listenfds[curr_pollfd];
                        goto got_fd;
                    }
                } while (curr_pollfd != last_pollfd);
            }
        }
    got_fd:
        if (!workers_may_exit) {
            if ((rv = apr_accept(&csd, sd, ptrans)) != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf, "apr_accept");
            }
            if ((rv = SAFE_ACCEPT(apr_lock_release(process_accept_mutex)))
                != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ap_server_conf,
                             "apr_lock_release failed. Attempting to shutdown "
                             "process gracefully.");
                workers_may_exit = 1;
            }
            apr_lock_release(thread_accept_mutex);
	    apr_lock_acquire(idle_thread_count_mutex);
            if (idle_thread_count > min_spare_threads) {
                idle_thread_count--;
            }
            else {
                if (!start_thread()) {
                    idle_thread_count--;
                }
            }
            apr_lock_release(idle_thread_count_mutex);
        got_from_other_child:
            if (thread_socket_table[thread_num] == -2) {
                struct msghdr msg;
                struct cmsghdr *cmsg;
                char sockname[80];
                struct iovec iov;
                int ret, sd, dp;

                iov.iov_base = sockname;
                iov.iov_len = 80;

                msg.msg_name = NULL;
                msg.msg_namelen = 0;
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                cmsg = apr_palloc(ptrans, sizeof(*cmsg) + sizeof(sd));
                cmsg->cmsg_len = sizeof(*cmsg) + sizeof(sd);
                msg.msg_control = (caddr_t)cmsg;
                msg.msg_controllen = cmsg->cmsg_len;
                msg.msg_flags = 0;
                
                ret = recvmsg(child_info_table[child_num].sd, &msg, 0);

                memcpy(&dp, CMSG_DATA(cmsg), sizeof(dp));

                thread_socket_table[thread_num] = dp;
                apr_os_sock_put(&csd, &child_info_table[child_num].sd, ptrans);
            }
            if (setjmp(jmpbuffer) != 1) {
                process_socket(ptrans, csd, conn_id);
            }
            else {
                thread_socket_table[thread_num] = -1;
            }  
            requests_this_child--;
	} else {
            if ((rv = SAFE_ACCEPT(apr_lock_release(process_accept_mutex)))
                != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ap_server_conf,
                             "apr_lock_release failed. Attempting to shutdown "
                             "process gracefully.");
                workers_may_exit = 1;
            }
            apr_lock_release(thread_accept_mutex);
	    apr_lock_acquire(idle_thread_count_mutex);
            idle_thread_count--;
            apr_lock_release(idle_thread_count_mutex);
	    break;
	}
        apr_pool_clear(ptrans);
    }

    apr_lock_acquire(thread_pool_parent_mutex);
    ap_update_child_status(child_num, thread_num, SERVER_DEAD,
                           (request_rec *) NULL);
    apr_pool_destroy(tpool);
    apr_lock_release(thread_pool_parent_mutex);
    apr_lock_acquire(worker_thread_count_mutex);
    worker_thread_count--;
    worker_thread_free_ids[worker_thread_count] = thread_num;
    if (worker_thread_count == 0) {
        /* All the threads have exited, now finish the shutdown process
         * by signalling the sigwait thread */
        kill(my_pid, SIGTERM);
    }
    apr_lock_release(worker_thread_count_mutex);

    return NULL;
}

/* Set group privileges.
 *
 * Note that we use the username as set in the config files, rather than
 * the lookup of to uid --- the same uid may have multiple passwd entries,
 * with different sets of groups for each.
 */

static int set_group_privs(uid_t uid, gid_t gid)
{
    if (!geteuid()) {
        const char *name;

        /* Get username if passed as a uid */

        struct passwd *ent;

        if ((ent = getpwuid(uid)) == NULL) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                     "getpwuid: couldn't determine user name from uid %u, "
                     "you probably need to modify the User directive",
                     (unsigned)uid);
            return -1;
        }

        name = ent->pw_name;

        /*
         * Set the GID before initgroups(), since on some platforms
         * setgid() is known to zap the group list.
         */
        if (setgid(gid) == -1) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                        "setgid: unable to set group id to Group %u",
                        (unsigned)gid);
            return -1;
        }

        /* Reset `groups' attributes. */

        if (initgroups(name, gid) == -1) {
            ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                        "initgroups: unable to set groups for User %s "
                        "and Group %u", name, (unsigned)gid);
            return -1;
        }
    }
    return 0;
}


static int perchild_setup_child(int childnum)
{
    child_info_t *ug = &child_info_table[childnum];

    if (ug->uid == -1 && ug->gid == -1) {
        return unixd_setup_child();
    }
    if (set_group_privs(ug->uid, ug->gid)) {
        return -1;
    }
    /* Only try to switch if we're running as root */
    if (!geteuid() && (
#ifdef _OSD_POSIX
        os_init_job_environment(server_conf, unixd_config.user_name, one_process) != 0 ||
#endif
        setuid(ug->uid) == -1)) {
        ap_log_error(APLOG_MARK, APLOG_ALERT, errno, NULL,
                    "setuid: unable to change to uid: %ld",
                    (long) ug->uid);
        return -1;
    }
    return 0;
}

static int check_signal(int signum)
{
    switch (signum) {
        case SIGTERM:
        case SIGINT:
            just_die(signum);
            return 1;
    }
    return 0;
}                                                                               

static void child_main(int child_num_arg)
{
    int i;
    ap_listen_rec *lr;
    apr_status_t rv;
    apr_thread_t *thread;

    my_pid = getpid();
    child_num = child_num_arg;
    apr_pool_create(&pchild, pconf);

    /*stuff to do before we switch id's, so we have permissions.*/

    rv = SAFE_ACCEPT(apr_lock_child_init(&process_accept_mutex, lock_fname,
                                        pchild));
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ap_server_conf,
                     "Couldn't initialize cross-process lock in child");
        clean_child_exit(APEXIT_CHILDFATAL);
    }

    if (perchild_setup_child(child_num)) {
	clean_child_exit(APEXIT_CHILDFATAL);
    }

    ap_child_init_hook(pchild, ap_server_conf);

    /*done with init critical section */

    apr_setup_signal_thread();

    requests_this_child = max_requests_per_child;
    
    /* Set up the pollfd array, num_listenfds + 1 for the pipe and 1 for
     * the child socket.
     */
    listenfds = apr_pcalloc(pchild, sizeof(*listenfds) * (num_listenfds + 2));
#if APR_FILES_AS_SOCKETS
    apr_socket_from_file(&listenfds[0], pipe_of_death_in);
#endif

    /* The child socket */
    apr_os_sock_put(&listenfds[1], &child_info_table[child_num].sd, pchild);

    num_listenfds++;
    for (lr = ap_listeners, i = 2; i <= num_listenfds; lr = lr->next, ++i)
        listenfds[i]=lr->sd;

    /* Setup worker threads */

    if (threads_to_start > max_threads) {
        threads_to_start = max_threads;
    }
    idle_thread_count = threads_to_start;
    worker_thread_count = 0;
    for (i = 0; i < max_threads; i++) {
        worker_thread_free_ids[i] = i;
    }
    apr_pool_create(&thread_pool_parent, pchild);
    apr_lock_create(&thread_pool_parent_mutex, APR_MUTEX, APR_INTRAPROCESS, 
                    NULL, pchild);
    apr_lock_create(&idle_thread_count_mutex, APR_MUTEX, APR_INTRAPROCESS, 
                    NULL, pchild);
    apr_lock_create(&worker_thread_count_mutex, APR_MUTEX, APR_INTRAPROCESS,
                    NULL, pchild);
    apr_lock_create(&pipe_of_death_mutex, APR_MUTEX, APR_INTRAPROCESS,
                    NULL, pchild);
    apr_lock_create(&thread_accept_mutex, APR_MUTEX, APR_INTRAPROCESS,
                    NULL, pchild);

    apr_threadattr_create(&worker_thread_attr, pchild);
    apr_threadattr_detach_set(worker_thread_attr);                                     
    apr_create_signal_thread(&thread, worker_thread_attr, check_signal, pchild);

    /* We are creating worker threads right now */
    for (i=0; i < threads_to_start; i++) {
        /* start_thread shouldn't fail here */
        if (!start_thread()) {
            break;
        }
    }

    /* This thread will be the one responsible for handling signals */
    worker_thread(&worker_thread_free_ids[max_threads]);

}

static int make_child(server_rec *s, int slot)
{
    int pid;

    if (slot + 1 > ap_max_daemons_limit) {
        ap_max_daemons_limit = slot + 1;
    }

    if (one_process) {
	set_signals();
        ap_child_table[slot].pid = getpid();
        ap_child_table[slot].status = SERVER_ALIVE;
	child_main(slot);
    }
    (void) ap_update_child_status(slot, 0, SERVER_STARTING, (request_rec *) NULL);

    if ((pid = fork()) == -1) {
        ap_log_error(APLOG_MARK, APLOG_ERR, errno, s,
                     "fork: Unable to fork new process");
	/* In case system resources are maxxed out, we don't want
	   Apache running away with the CPU trying to fork over and
	   over and over again. */
	sleep(10);

	return -1;
    }

    if (!pid) {
#ifdef AIX_BIND_PROCESSOR
      /* By default, AIX binds to a single processor.  This bit unbinds
	 children which will then bind to another CPU.
      */
#include <sys/processor.h>
        int status = bindprocessor(BINDPROCESS, (int)getpid(),
			       PROCESSOR_CLASS_ANY);
	if (status != OK)
	    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, errno, 
                         ap_server_conf, "processor unbind failed %d", status);
#endif

        RAISE_SIGSTOP(MAKE_CHILD);

	/* XXX - For an unthreaded server, a signal handler will be necessary
        apr_signal(SIGTERM, just_die);
	*/
        child_main(slot);

	return 0;
    }
    /* else */
    ap_child_table[slot].pid = pid;
    ap_child_table[slot].status = SERVER_ALIVE;

    return 0;
}

/* start up a bunch of children */
static int startup_children(int number_to_start)
{
    int i;

    for (i = 0; number_to_start && i < num_daemons; ++i) {
	if (ap_child_table[i].pid) {
	    continue;
	}
	if (make_child(ap_server_conf, i) < 0) {
	    break;
	}
	--number_to_start;
    }
    return number_to_start;
}


/*
 * spawn_rate is the number of children that will be spawned on the
 * next maintenance cycle if there aren't enough servers.  It is
 * doubled up to MAX_SPAWN_RATE, and reset only when a cycle goes by
 * without the need to spawn.
 */
static int spawn_rate = 1;
#ifndef MAX_SPAWN_RATE
#define MAX_SPAWN_RATE	(32)
#endif
static int hold_off_on_exponential_spawning;

static void perform_child_maintenance(void)
{
    int i;
    int free_length;
    int free_slots[MAX_SPAWN_RATE];
    int last_non_dead = -1;

    /* initialize the free_list */
    free_length = 0;
    
    for (i = 0; i < num_daemons; ++i) {
        if (ap_child_table[i].pid == 0) {
            if (free_length < spawn_rate) {
                free_slots[free_length] = i;
                ++free_length;
            }
        }
        else {
            last_non_dead = i;
        }

	if (i >= ap_max_daemons_limit && free_length >= spawn_rate) {
	    break;
	}
    }
    ap_max_daemons_limit = last_non_dead + 1;

    if (free_length > 0) {
	for (i = 0; i < free_length; ++i) {
	    make_child(ap_server_conf, free_slots[i]);
	}
	/* the next time around we want to spawn twice as many if this
	 * wasn't good enough, but not if we've just done a graceful
	 */
	if (hold_off_on_exponential_spawning) {
	    --hold_off_on_exponential_spawning;
	}
	else if (spawn_rate < MAX_SPAWN_RATE) {
	    spawn_rate *= 2;
	}
    }
    else {
        spawn_rate = 1;
    }
}

static void server_main_loop(int remaining_children_to_start)
{
    int child_slot;
    apr_wait_t status;
    apr_proc_t pid;
    int i;

    while (!restart_pending && !shutdown_pending) {
        ap_wait_or_timeout(&status, &pid, pconf);
        
        if (pid.pid != -1) {
            ap_process_child_status(&pid, status);
            /* non-fatal death... note that it's gone in the child table and
             * clean out the status table. */
            child_slot = -1;
            for (i = 0; i < ap_max_daemons_limit; ++i) {
        	if (ap_child_table[i].pid == pid.pid) {
                    child_slot = i;
                    break;
                }
            }
            if (child_slot >= 0) {
                ap_child_table[child_slot].pid = 0;
                ap_update_child_status(child_slot, i, SERVER_DEAD, (request_rec *) NULL);

                
		if (remaining_children_to_start
		    && child_slot < num_daemons) {
		    /* we're still doing a 1-for-1 replacement of dead
                     * children with new children
                     */
		    make_child(ap_server_conf, child_slot);
		    --remaining_children_to_start;
		}
#if APR_HAS_OTHER_CHILD
	    }
	    else if (apr_proc_other_child_read(&pid, status) == 0) {
		/* handled */
#endif
	    }
	    else if (is_graceful) {
		/* Great, we've probably just lost a slot in the
		 * child table.  Somehow we don't know about this
		 * child.
		 */
		ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, 
                             ap_server_conf,
			    "long lost child came home! (pid %ld)", 
                             (long)pid.pid);
	    }
	    /* Don't perform idle maintenance when a child dies,
             * only do it when there's a timeout.  Remember only a
             * finite number of children can die, and it's pretty
             * pathological for a lot to die suddenly.
             */
	    continue;
	}
	else if (remaining_children_to_start) {
	    /* we hit a 1 second timeout in which none of the previous
	     * generation of children needed to be reaped... so assume
	     * they're all done, and pick up the slack if any is left.
	     */
	    remaining_children_to_start = \
	    	startup_children(remaining_children_to_start);
	    /* In any event we really shouldn't do the code below because
	     * few of the servers we just started are in the IDLE state
	     * yet, so we'd mistakenly create an extra server.
	     */
	    continue;
	}

	perform_child_maintenance();
    }
}

int ap_mpm_run(apr_pool_t *_pconf, apr_pool_t *plog, server_rec *s)
{
    int remaining_children_to_start;
    int i;
    apr_status_t rv;
    apr_size_t one = 1;

    pconf = _pconf;
    ap_server_conf = s;
    if ((rv = apr_file_pipe_create(&pipe_of_death_in, &pipe_of_death_out, pconf)) 
        != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv,
                     (const server_rec*) ap_server_conf,
                     "apr_file_pipe_create (pipe_of_death)");
        exit(1);
    }
    if ((rv = apr_file_pipe_timeout_set(pipe_of_death_in, 0)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv,
                     (const server_rec*) ap_server_conf,
                     "apr_file_pipe_timeout_set (pipe_of_death)");
        exit(1);
    }
    ap_server_conf = s;
    if ((num_listenfds = ap_setup_listeners(ap_server_conf)) < 1) {
        /* XXX: hey, what's the right way for the mpm to indicate a fatal error? */
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_ALERT, 0, s,
            "no listening sockets available, shutting down");
        return 1;
    }
    ap_log_pid(pconf, ap_pid_fname);

    /* Initialize cross-process accept lock */
    lock_fname = apr_psprintf(_pconf, "%s.%u",
                             ap_server_root_relative(_pconf, lock_fname),
                             my_pid);
    rv = SAFE_ACCEPT(apr_lock_create(&process_accept_mutex, APR_MUTEX,
                                    APR_CROSS_PROCESS, lock_fname, _pconf));
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
                     "Couldn't create cross-process lock");
        return 1;
    }

    if (!is_graceful) {
        ap_create_scoreboard(pconf, SB_SHARED);
    }
    /* Initialize the child table */
    if (!is_graceful) {
        for (i = 0; i < HARD_SERVER_LIMIT; i++) {
            ap_child_table[i].pid = 0;
        }
    }

    set_signals();

    /* If we're doing a graceful_restart then we're going to see a lot
     * of children exiting immediately when we get into the main loop
     * below (because we just sent them SIGWINCH).  This happens pretty
     * rapidly... and for each one that exits we'll start a new one until
     * we reach at least daemons_min_free.  But we may be permitted to
     * start more than that, so we'll just keep track of how many we're
     * supposed to start up without the 1 second penalty between each fork.
     */
    remaining_children_to_start = num_daemons;
    if (!is_graceful) {
	remaining_children_to_start = \
	    startup_children(remaining_children_to_start);
    }
    else {
	/* give the system some time to recover before kicking into
	    * exponential mode */
	hold_off_on_exponential_spawning = 10;
    }

    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
		"%s configured -- resuming normal operations",
		ap_get_server_version());
    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, 0, ap_server_conf,
		"Server built: %s", ap_get_server_built());
    restart_pending = shutdown_pending = 0;

    server_main_loop(remaining_children_to_start);

    if (shutdown_pending) {
        /* Time to gracefully shut down:
         * Kill child processes, tell them to call child_exit, etc...
         */
        if (unixd_killpg(getpgrp(), SIGTERM) < 0) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "killpg SIGTERM");
        }
        ap_reclaim_child_processes(1);		/* Start with SIGTERM */
    
        /* cleanup pid file on normal shutdown */
        {
            const char *pidfile = NULL;
            pidfile = ap_server_root_relative (pconf, ap_pid_fname);
            if ( pidfile != NULL && unlink(pidfile) == 0)
                ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, 0,
            		 ap_server_conf,
            		 "removed PID file %s (pid=%ld)",
            		 pidfile, (long)getpid());
        }
    
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0,
                     ap_server_conf, "caught SIGTERM, shutting down");
    
	return 1;
    }

    /* we've been told to restart */
    apr_signal(SIGHUP, SIG_IGN);

    if (one_process) {
	/* not worth thinking about */
	return 1;
    }

    if (is_graceful) {
        char char_of_death = '!';

	ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
		    "SIGWINCH received.  Doing graceful restart");

	/* This is mostly for debugging... so that we know what is still
         * gracefully dealing with existing request.
         */
	
	for (i = 0; i < num_daemons; ++i) {
	    if (ap_child_table[i].pid) {
	        ap_child_table[i].status = SERVER_DYING;
	    } 
	}
	/* give the children the signal to die */
        for (i = 0; i < num_daemons;) {
            if ((rv = apr_file_write(pipe_of_death_out, &char_of_death, &one)) != APR_SUCCESS) {
                if (APR_STATUS_IS_EINTR(rv)) continue;
                ap_log_error(APLOG_MARK, APLOG_WARNING, rv, ap_server_conf,
                             "write pipe_of_death");
            }
            i++;
        }
    }
    else {
      /* Kill 'em all.  Since the child acts the same on the parents SIGTERM 
       * and a SIGHUP, we may as well use the same signal, because some user
       * pthreads are stealing signals from us left and right.
       */
	if (unixd_killpg(getpgrp(), SIGTERM) < 0) {
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
                         "killpg SIGTERM");
	}
        ap_reclaim_child_processes(1);		/* Start with SIGTERM */
	ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0,
                     ap_server_conf, "SIGHUP received.  Attempting to restart");
    }
    return 0;
}

static void perchild_pre_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp)
{
    static int restart_num = 0;
    int no_detach = 0;
    int i;

    one_process = !!ap_exists_config_define("ONE_PROCESS");
    no_detach = !!ap_exists_config_define("NO_DETACH");

    /* sigh, want this only the second time around */
    if (restart_num++ == 1) {
	is_graceful = 0;

	if (!one_process && !no_detach) {
	    apr_proc_detach();
	}

	my_pid = getpid();
    }

    unixd_pre_config(ptemp);
    ap_listen_pre_config();
    num_daemons = DEFAULT_NUM_DAEMON;
    threads_to_start = DEFAULT_START_THREAD;
    min_spare_threads = DEFAULT_MIN_SPARE_THREAD;
    max_spare_threads = DEFAULT_MAX_SPARE_THREAD;
    max_threads = HARD_THREAD_LIMIT;
    ap_pid_fname = DEFAULT_PIDLOG;
    ap_scoreboard_fname = DEFAULT_SCOREBOARD;
    lock_fname = DEFAULT_LOCKFILE;
    max_requests_per_child = DEFAULT_MAX_REQUESTS_PER_CHILD;
    curr_child_num = 0;

    apr_cpystrn(ap_coredump_dir, ap_server_root, sizeof(ap_coredump_dir));

    for (i = 0; i < HARD_SERVER_LIMIT; i++) {
        child_info_table[i].uid = -1;
        child_info_table[i].gid = -1;
        child_info_table[i].sd = -1;
    }
    for (i = 0; i < HARD_THREAD_LIMIT; i++) {
        thread_socket_table[i] = -1;
    }
}

static int pass_request(request_rec *r)
{
    apr_socket_t *thesock = r->connection->client_socket;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    int sfd;
    struct iovec iov;
    apr_bucket_brigade *bb = apr_brigade_create(r->pool);
    perchild_server_conf *sconf = (perchild_server_conf *)
                            ap_get_module_config(r->server->module_config, 
                                                 &mpm_perchild_module);
    char *foo;
    int len;

    apr_pool_userdata_get((void **)&foo, "PERCHILD_BUFFER", r->connection->pool);
    len = strlen(foo);

    apr_pool_userdata_set(NULL, "PERCHILD_BUFFER", apr_pool_cleanup_null, 
                     r->connection->pool);

    apr_os_sock_get(&sfd, thesock);

    iov.iov_base = NULL;
    iov.iov_len = 0;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    cmsg = apr_palloc(r->pool, sizeof(*cmsg) + sizeof(sfd));
    cmsg->cmsg_len = sizeof(*cmsg) + sizeof(int);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    memcpy(CMSG_DATA(cmsg), &sfd, sizeof(sfd));

    msg.msg_control = (caddr_t)cmsg;
    msg.msg_controllen = cmsg->cmsg_len;
    msg.msg_flags=0;

    if (sendmsg(sconf->sd2, &msg, 0) == -1) {
        apr_pool_destroy(r->pool);
        return -1;
    }

    write(sconf->sd2, foo, len);
   
    while (ap_get_brigade(r->input_filters, bb, AP_MODE_NONBLOCKING) == APR_SUCCESS) {
        apr_bucket *e;
        APR_BRIGADE_FOREACH(e, bb) {
            const char *str;

            apr_bucket_read(e, &str, &len, APR_NONBLOCK_READ);
            write(sconf->sd2, str, len);
        }
    }

    apr_pool_destroy(r->pool);
    return 1;
}

static char *make_perchild_socket(const char *fullsockname, int sd[2])
{
    socketpair(PF_UNIX, SOCK_STREAM, 0, sd);
    return NULL;
}


static void perchild_post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    int i;
    server_rec *sr;
    perchild_server_conf *sconf;
    int def_sd[2];
    
    def_sd[0] = -1;
    def_sd[1] = -1;

    for (sr = s; sr; sr = sr->next) {
        sconf = (perchild_server_conf *)ap_get_module_config(sr->module_config,
                                                      &mpm_perchild_module);

        if (sconf->sd == -1) {
            sconf->fullsockname = apr_pstrcat(sr->process->pool, 
                                             sconf->sockname, ".DEFAULT", NULL);
            if (def_sd[0] == -1) {
                if (!make_perchild_socket(sconf->fullsockname, def_sd)) {
                    /* log error */
                }
            }
            sconf->sd = def_sd[0];
            sconf->sd2 = def_sd[1];
        }
    }

    for (i = 0; i < num_daemons; i++) {
        if (child_info_table[i].uid == -1) {
            child_info_table[i].sd = def_sd[0];
        }
    }
}

static int perchild_post_read(request_rec *r)
{
    ap_filter_t *f = r->connection->input_filters;
    int thread_num = r->connection->id % HARD_THREAD_LIMIT;
    perchild_server_conf *sconf = (perchild_server_conf *)
                            ap_get_module_config(r->server->module_config, 
                                                 &mpm_perchild_module);

    while (f) {
        if (!strcmp("PERCHILD_BUFFER", f->frec->name)) {
            ap_remove_output_filter(f);
            break;
        }
        f = f->next;
    }

    if (thread_socket_table[thread_num] != -1) {
        apr_socket_t *csd = NULL;

        apr_os_sock_put(&csd, &thread_socket_table[thread_num], 
                             r->connection->pool);
        ap_sock_disable_nagle(csd);
        r->connection->client_socket = csd;
        return OK;
    }
    else {
        if (sconf->sd != child_info_table[child_num].sd) {
            if (pass_request(r) == -1) {
                ap_log_error(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0,
                             ap_server_conf, "Could not pass request to proper "
                             "child, request will not be honored.");
            }
            longjmp(jmpbuffer, 1); 
        }
        return OK;
    }
    return OK;
}

static apr_status_t perchild_buffer(ap_filter_t *f, apr_bucket_brigade *b, ap_input_mode_t mode)
{
    apr_bucket *e;
    apr_status_t rv;
    char *buffer = NULL;
    const char *str;
    apr_size_t len;

    if ((rv = ap_get_brigade(f->next, b, mode)) != APR_SUCCESS) {
        return rv;
    }

    apr_pool_userdata_get((void **)&buffer, "PERCHILD_BUFFER", f->c->pool);

    APR_BRIGADE_FOREACH(e, b) {
        if (e->length != 0) {
            apr_bucket_read(e, &str, &len, APR_NONBLOCK_READ);
       
            if (buffer == NULL) {
                buffer = apr_pstrndup(f->c->pool, str, len);
            }
            else {
               buffer = apr_pstrcat(f->c->pool, buffer, 
                                    apr_pstrndup(f->c->pool, str, len), NULL);
            } 
        }
    }
    apr_pool_userdata_set(buffer, "PERCHILD_BUFFER", apr_pool_cleanup_null, f->c->pool);
    
    return APR_SUCCESS;
}

static int perchild_pre_connection(conn_rec *c)
{
    ap_add_input_filter("PERCHILD_BUFFER", NULL, NULL, c);
    return OK;
}

static void perchild_hooks(apr_pool_t *p)
{
    one_process = 0;

    ap_hook_pre_config(perchild_pre_config, NULL, NULL, APR_HOOK_MIDDLE); 
    ap_hook_post_config(perchild_post_config, NULL, NULL, APR_HOOK_MIDDLE); 
    ap_hook_pre_connection(perchild_pre_connection,NULL,NULL, APR_HOOK_MIDDLE);

    /* This must be run absolutely first.  If this request isn't for this
     * server then we need to forward it to the proper child.  No sense
     * tying up this server running more post_read request hooks if it is
     * just going to be forwarded along.
     */
    ap_hook_post_read_request(perchild_post_read, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_register_input_filter("PERCHILD_BUFFER", perchild_buffer, AP_FTYPE_CONTENT);
}

static const char *set_pidfile(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    if (cmd->server->is_virtual) {
	return "PidFile directive not allowed in <VirtualHost>";
    }
    ap_pid_fname = arg;
    return NULL;
}

static const char *set_scoreboard(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_scoreboard_fname = arg;
    return NULL;
}

static const char *set_lockfile(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    lock_fname = arg;
    return NULL;
}
static const char *set_num_daemons (cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    num_daemons = atoi(arg);
    if (num_daemons > HARD_SERVER_LIMIT) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: NumServers of %d exceeds compile time limit "
                    "of %d servers,", num_daemons, HARD_SERVER_LIMIT);
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    " lowering NumServers to %d.  To increase, please "
                    "see the", HARD_SERVER_LIMIT);
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    " HARD_SERVER_LIMIT define in %s.",
                    AP_MPM_HARD_LIMITS_FILE);
       num_daemons = HARD_SERVER_LIMIT;
    } 
    else if (num_daemons < 1) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: Require NumServers > 0, setting to 1");
	num_daemons = 1;
    }
    return NULL;
}

static const char *set_threads_to_start (cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    threads_to_start = atoi(arg);
    if (threads_to_start > HARD_THREAD_LIMIT) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: StartThreads of %d exceeds compile time"
                     " limit of %d threads,", threads_to_start,
                     HARD_THREAD_LIMIT);
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     " lowering StartThreads to %d. To increase, please"
                     " see the", HARD_THREAD_LIMIT);
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     " HARD_THREAD_LIMIT define in %s.",
                     AP_MPM_HARD_LIMITS_FILE);
    }
    else if (threads_to_start < 1) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: Require StartThreads > 0, setting to 1");
	threads_to_start = 1;
    }
    return NULL;
}

static const char *set_min_spare_threads(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    min_spare_threads = atoi(arg);
    if (min_spare_threads <= 0) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: detected MinSpareThreads set to non-positive.");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "Resetting to 1 to avoid almost certain Apache failure.");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "Please read the documentation.");
       min_spare_threads = 1;
    }
       
    return NULL;
}

static const char *set_max_spare_threads(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    max_spare_threads = atoi(arg);
    if (max_spare_threads >= HARD_THREAD_LIMIT) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: detected MinSpareThreads set higher than");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "HARD_THREAD_LIMIT. Resetting to %d", HARD_THREAD_LIMIT);
       max_spare_threads = HARD_THREAD_LIMIT;
    }
    return NULL;
}

static const char *set_max_threads(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    max_threads = atoi(arg);
    if (max_threads > HARD_THREAD_LIMIT) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: detected MaxThreadsPerChild set higher than");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "HARD_THREAD_LIMIT. Resetting to %d", HARD_THREAD_LIMIT);
       max_threads = HARD_THREAD_LIMIT;
    }
    return NULL;
}

static const char *set_max_requests(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    max_requests_per_child = atoi(arg);

    return NULL;
}

static const char *set_coredumpdir (cmd_parms *cmd, void *dummy,
				    const char *arg) 
{
    apr_finfo_t finfo;
    const char *fname;
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    fname = ap_server_root_relative(cmd->pool, arg);
    if ((apr_stat(&finfo, fname, APR_FINFO_TYPE, cmd->pool) != APR_SUCCESS) 
        || (finfo.filetype != APR_DIR)) {
	return apr_pstrcat(cmd->pool, "CoreDumpDirectory ", fname, 
			  " does not exist or is not a directory", NULL);
    }
    apr_cpystrn(ap_coredump_dir, fname, sizeof(ap_coredump_dir));
    return NULL;
}

static const char *set_child_per_uid(cmd_parms *cmd, void *dummy, const char *u,
                                     const char *g, const char *num)
{
    int i;
    int max_this_time = atoi(num) + curr_child_num;
    for (i = curr_child_num; i < max_this_time; i++, curr_child_num++); {
        child_info_t *ug = &child_info_table[i - 1];

        if (i > num_daemons) {
            return "Trying to use more child ID's than NumServers.  Increase "
                   "NumServers in your config file.";
        }
    
        ug->uid = atoi(u);
        ug->gid = atoi(g); 
    }
    return NULL;
}

static const char *assign_childuid(cmd_parms *cmd, void *dummy, const char *uid,
                                   const char *gid)
{
    int i;
    int u = atoi(uid);
    int g = atoi(gid);
    const char *errstr;
    int socks[2];
    perchild_server_conf *sconf = (perchild_server_conf *)
                            ap_get_module_config(cmd->server->module_config, 
                                                 &mpm_perchild_module);

    sconf->fullsockname = apr_pstrcat(cmd->pool, sconf->sockname, ".", uid, ":", gid, NULL);

    if ((errstr = make_perchild_socket(sconf->fullsockname, socks))) {
        return errstr;
    }

    sconf->sd = socks[0]; 
    sconf->sd2 = socks[1];

    for (i = 0; i < num_daemons; i++) {
        if (u == child_info_table[i].uid && g == child_info_table[i].gid) {
            child_info_table[i].sd = sconf->sd;
        }
    }

    return NULL;
}


static const command_rec perchild_cmds[] = {
UNIX_DAEMON_COMMANDS
LISTEN_COMMANDS
AP_INIT_TAKE1("PidFile", set_pidfile, NULL, RSRC_CONF,
              "A file for logging the server process ID"),
AP_INIT_TAKE1("ScoreBoardFile", set_scoreboard, NULL, RSRC_CONF,
              "A file for Apache to maintain runtime process management information"),
AP_INIT_TAKE1("LockFile", set_lockfile, NULL, RSRC_CONF,
              "The lockfile used when Apache needs to lock the accept() call"),
AP_INIT_TAKE1("NumServers", set_num_daemons, NULL, RSRC_CONF,
              "Number of children alive at the same time"),
AP_INIT_TAKE1("StartThreads", set_threads_to_start, NULL, RSRC_CONF,
              "Number of threads each child creates"),
AP_INIT_TAKE1("MinSpareThreads", set_min_spare_threads, NULL, RSRC_CONF,
              "Minimum number of idle threads per child, to handle request spikes"),
AP_INIT_TAKE1("MaxSpareThreads", set_max_spare_threads, NULL, RSRC_CONF,
              "Maximum number of idle threads per child"),
AP_INIT_TAKE1("MaxThreadsPerChild", set_max_threads, NULL, RSRC_CONF,
              "Maximum number of threads per child"),
AP_INIT_TAKE1("MaxRequestsPerChild", set_max_requests, NULL, RSRC_CONF,
              "Maximum number of requests a particular child serves before dying."),
AP_INIT_TAKE1("CoreDumpDirectory", set_coredumpdir, NULL, RSRC_CONF,
              "The location of the directory Apache changes to before dumping core"),
AP_INIT_TAKE3("ChildperUserID", set_child_per_uid, NULL, RSRC_CONF,
              "Specify a User and Group for a specific child process."),
AP_INIT_TAKE2("AssignUserID", assign_childuid, NULL, RSRC_CONF,
              "Tie a virtual host to a specific child process."),
{ NULL }
};

static void *perchild_create_config(apr_pool_t *p, server_rec *s)
{
    perchild_server_conf *c =
    (perchild_server_conf *) apr_pcalloc(p, sizeof(perchild_server_conf));

    c->sd = -1;
    return c;
}

module AP_MODULE_DECLARE_DATA mpm_perchild_module = {
    MPM20_MODULE_STUFF,
    NULL,                       /* hook to run before apache parses args */
    NULL,			/* create per-directory config structure */
    NULL,			/* merge per-directory config structures */
    perchild_create_config,	/* create per-server config structure */
    NULL,			/* merge per-server config structures */
    perchild_cmds,		/* command apr_table_t */
    perchild_hooks 		/* register_hooks */
};

