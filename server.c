#include "server.h"
#include "util.h"
#include "sockstream.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <assert.h>

#include <apr_signal.h>
#include <apr_general.h>
#include <apr_pools.h>
#include <apr_errno.h>

apr_status_t
jrs_server_init(jrs_server_t **server, apr_pool_t *pool, int port)
{
    apr_pool_t *subpool = NULL;
    jrs_server_t *ret = NULL;
    apr_status_t rv = 0;
    struct sockaddr_in sin;

    rv = apr_pool_create(&subpool, pool);
    if (rv != APR_SUCCESS)
        goto out;

    rv = APR_ENOMEM;
    ret = apr_pcalloc(subpool, sizeof(jrs_server_t));
    if (!ret)
        goto out;

    ret->pool = subpool;
    ret->port = port;
    DLIST_INIT(&ret->conns);
    DLIST_INIT(&ret->jobs);

    /* open the socket and bind to the port now so that we can bail if we don't
     * get the port */
    ret->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ret->socket == -1) {
        rv = APR_FROM_OS_ERROR(errno);
        goto out;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);

    if (bind(ret->socket, (struct sockaddr *)&sin, sizeof(sin))) {
        rv = APR_FROM_OS_ERROR(errno);
        goto out;
    }

    if (listen(ret->socket, 10)) {
        rv = APR_FROM_OS_ERROR(errno);
        goto out;
    }

    *server = ret;
    return APR_SUCCESS;

out:
    if (ret && ret->socket)
        close(ret->socket);
    if (subpool != NULL)
        apr_pool_destroy(subpool);
    *server = NULL;
    return rv;
}

static jrs_server_t *active_server = NULL;

static void
jrs_server_sigchld(int sig)
{
    if (active_server)
        active_server->sigchld_flag = 1;
}

static void
handle_listen(jrs_server_t *server)
{
    jrs_conn_t *conn;
    apr_pool_t *subpool;
    apr_status_t rv;
    int sockfd;
    struct sockaddr_in sin;
    socklen_t sin_len = sizeof(sin);

    sockfd = accept(server->socket, (struct sockaddr *)&sin, &sin_len);
    if (sockfd == -1)
        return;

    unsigned char *ipaddr = (unsigned char *)&sin.sin_addr.s_addr;
    jrs_log("connection from %d.%d.%d.%d:%d", ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3],
            sin.sin_port);

    rv = apr_pool_create(&subpool, server->pool);
    if (rv != APR_SUCCESS) {
        close(sockfd);
        return;
    }

    conn = apr_pcalloc(subpool, sizeof(jrs_conn_t));
    if (!conn) {
        close(sockfd);
        apr_pool_destroy(subpool);
        return;
    }

    conn->linebuf_size = 4096;
    conn->linebuf = apr_pcalloc(subpool, conn->linebuf_size);
    if (!conn->linebuf) {
        close(sockfd);
        apr_pool_destroy(subpool);
        return;
    }

    conn->pool = subpool;
    conn->socket = sockfd;
    conn->server = server;

    rv = jrs_sockstream_create(&conn->sockstream, sockfd, subpool);
    if (rv != APR_SUCCESS) {
        close(sockfd);
        apr_pool_destroy(subpool);
        return;
    }

    DLIST_INSERT(DLIST_TAIL(&server->conns), conn);
}

static int
handle_conn(jrs_server_t *server, jrs_conn_t *conn, int rflag, int eflag, int wflag)
{
    /* handle any pending I/O on the connection in a nonblocking way */
    if (jrs_sockstream_sendrecv(conn->sockstream, rflag))
        return 1;
    if (jrs_sockstream_closed(conn->sockstream))
        return 1;

    /* is there a line available? If not, we're done here. */
    if (!jrs_sockstream_hasline(conn->sockstream))
        return 0;

    /* OK, grab the line and parse it. */
    int size = conn->linebuf_size;
    if (jrs_sockstream_readline(conn->sockstream, conn->linebuf, &size))
        return 1; /* on error, close connection */

    /* command (opcode) is first character/byte of line */
    if (size < 1) return 1;
    switch (conn->linebuf[0]) {
        case 'N': /* new job */
            {
                /* first arg: CWD; second arg: command line. Returns ID. */
                jrs_log("new job: %s", conn->linebuf);
                jrs_sockstream_write(conn->sockstream, "new job!\n", 9);
            }
            break;

        case 'L': /* list jobs */
            {
                /* return list of running job IDs, space-separated. */
                jrs_log("list jobs");
                jrs_sockstream_write(conn->sockstream, "list jobs!\n", 10);
            }
            break;
    }

    jrs_sockstream_sendrecv(conn->sockstream, 0);
    return 0;
}

void
jrs_server_run(jrs_server_t *server)
{
    apr_status_t rv;
    jrs_job_t *job;
    jrs_conn_t *conn, *nconn;
    fd_set rfds, efds, wfds;

    assert(!active_server);

    /* set SIGCHLD handler */
    apr_signal(SIGCHLD, jrs_server_sigchld);
    active_server = server; /* global used by signal handler */

    /* main event loop */
    while (!shutdown_signal) {
        int nsig = 0;
        int maxfd = 0;

        /* select-poll on listen socket, connection sockets, and signals
         * (SIGCHLD for job completion) */

        /* initialize the set of fds on which we select() */
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_ZERO(&wfds);
        FD_SET(server->socket, &rfds);
        FD_SET(server->socket, &efds);
        jrs_log("waiting on listen socket fd %d", server->socket);
        maxfd = server->socket + 1;
        DLIST_FOREACH(&server->conns, conn) {
            FD_SET(conn->socket, &rfds);
            FD_SET(conn->socket, &efds);
            if (!jrs_sockstream_flushed(conn->sockstream))
                    FD_SET(conn->socket, &wfds);
            jrs_log("waiting on connection socket fd %d", conn->socket);
            if (conn->socket >= maxfd)
                maxfd = conn->socket + 1;
        }
        
        jrs_log("select");
        nsig = select(maxfd, &rfds, &wfds, &efds, NULL);
        jrs_log("select woke up");

        if (shutdown_signal)
            break;

        /* Handle any completed jobs. One or more SIGCHLD signals may have
         * arrived during the poll (and/or may have woken up the poll). */
        if (server->sigchld_flag) {
            pid_t pid;
            int status;

            while ((pid = waitpid(-1, &status, WNOHANG)) != 0) {
                job = NULL;

                /* a child exited. Find the matching job. */
                DLIST_FOREACH(&server->jobs, job) {
                    if (job->pid == pid) {
                        DLIST_REMOVE(job);
                        /* this frees *job as well */
                        apr_pool_destroy(job->pool);
                        break;
                    }
                }
            }
        }

        /* If listen socket is ready, start a new connection */
        if (FD_ISSET(server->socket, &rfds)) {
            handle_listen(server);
        }

        /* If any connect socket is ready, receive on it */
        for (conn = DLIST_HEAD(&server->conns);
                conn != DLIST_END(&server->conns);
                conn = nconn) {
            /* this call may remove the connection, so we explicitly grab
             * the next ptr off conn before calling it. */
            nconn = DLIST_NEXT(conn);
            if (FD_ISSET(conn->socket, &rfds) ||
                    FD_ISSET(conn->socket, &efds) ||
                    FD_ISSET(conn->socket, &wfds)) {
                if (handle_conn(server, conn,
                            FD_ISSET(conn->socket, &rfds),
                            FD_ISSET(conn->socket, &efds),
                            FD_ISSET(conn->socket, &wfds))) {
                    DLIST_REMOVE(conn);
                }
            }
        }
    }

    active_server = NULL;
}

void
jrs_server_destroy(jrs_server_t *server)
{
    jrs_job_t *job;
    jrs_conn_t *conn;

    /* kill all the jobs */
    DLIST_FOREACH(&server->jobs, job) {
        kill(job->pid, SIGTERM);
    }

    /* close down all the connections */
    DLIST_FOREACH(&server->conns, conn) {
        close(conn->socket);
    }

    /* close the listening socket */
    close(server->socket);

    /* free all memory (including the list nodes above) */
    apr_pool_destroy(server->pool);

    memset(server, 0, sizeof(jrs_server_t));
}
