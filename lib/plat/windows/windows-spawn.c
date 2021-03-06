/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 - 2020 Andy Green <andy@warmcat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "private-lib-core.h"

#include <tchar.h>
#include <stdio.h>
#include <strsafe.h>

void
lws_spawn_timeout(struct lws_sorted_usec_list *sul)
{
	struct lws_spawn_piped *lsp = lws_container_of(sul,
					struct lws_spawn_piped, sul);

	lwsl_warn("%s: spawn exceeded timeout, killing\n", __func__);

	lws_spawn_piped_kill_child_process(lsp);
}

static struct lws *
lws_create_basic_wsi(struct lws_context *context, int tsi,
		     const struct lws_role_ops *ops)
{
	struct lws *new_wsi;

	if (!context->vhost_list)
		return NULL;

	if ((unsigned int)context->pt[tsi].fds_count ==
	    context->fd_limit_per_thread - 1) {
		lwsl_err("no space for new conn\n");
		return NULL;
	}

	new_wsi = lws_zalloc(sizeof(*new_wsi), "new wsi");
	if (new_wsi == NULL) {
		lwsl_err("Out of memory for new connection\n");
		return NULL;
	}

	new_wsi->tsi = tsi;
	new_wsi->context = context;
	new_wsi->pending_timeout = NO_PENDING_TIMEOUT;
	new_wsi->rxflow_change_to = LWS_RXFLOW_ALLOW;

	/* initialize the instance struct */

	lws_role_transition(new_wsi, 0, LRS_ESTABLISHED, ops);

	new_wsi->hdr_parsing_completed = 0;
	new_wsi->position_in_fds_table = LWS_NO_FDS_POS;

	/*
	 * these can only be set once the protocol is known
	 * we set an unestablished connection's protocol pointer
	 * to the start of the defauly vhost supported list, so it can look
	 * for matching ones during the handshake
	 */

	new_wsi->user_space = NULL;
	new_wsi->desc.sockfd = LWS_SOCK_INVALID;
	context->count_wsi_allocated++;

	return new_wsi;
}

void
lws_spawn_piped_destroy(struct lws_spawn_piped **_lsp)
{
	struct lws_spawn_piped *lsp = *_lsp;
	int n;

	if (!lsp)
		return;

	for (n = 0; n < 3; n++) {
		if (lsp->pipe_fds[n][!!(n == 0)] == 0)
			lwsl_err("ZERO FD IN CGI CLOSE");

		if (lsp->pipe_fds[n][!!(n == 0)] >= 0) {
			CloseHandle(lsp->pipe_fds[n][!!(n == 0)]);
			lsp->pipe_fds[n][!!(n == 0)] = NULL;
		}
	}

	lws_dll2_remove(&lsp->dll);

	lws_sul_schedule(lsp->info.vh->context, lsp->info.tsi, &lsp->sul,
			 NULL, LWS_SET_TIMER_USEC_CANCEL);

	lws_free_set_NULL((*_lsp));
}

int
lws_spawn_reap(struct lws_spawn_piped *lsp)
{
#if 0
	void *opaque = lsp->info.opaque;
	lsp_cb_t cb = lsp->info.reap_cb;
	struct lws_spawn_piped temp;
	int n;

	if (lsp->child_pid < 1)
		return 0;

	/* check if exited, do not reap yet */

	memset(&lsp->si, 0, sizeof(lsp->si));
	n = waitid(P_PID, lsp->child_pid, &lsp->si, WEXITED | WNOHANG | WNOWAIT);
	if (n < 0) {
		lwsl_info("%s: child %d still running\n", __func__, lsp->child_pid);
		return 0;
	}

	if (!lsp->si.si_code)
		return 0;

	/* his process has exited... */

	if (!lsp->reaped) {
		/* mark the earliest time we knew he had gone */
		lsp->reaped = lws_now_usecs();

		/*
		 * Switch the timeout to restrict the amount of grace time
		 * to drain stdwsi
		 */

		lws_sul_schedule(lsp->info.vh->context, lsp->info.tsi,
				 &lsp->sul, lws_spawn_timeout,
				 5 * LWS_US_PER_SEC);
	}

	/*
	 * Stage finalizing our reaction to the process going down until the
	 * stdwsi flushed whatever is in flight and all noticed they were
	 * closed.  For that reason, each stdwsi close must call lws_spawn_reap
	 * to check if that was the last one and we can proceed with the reap.
	 */

	if (!lsp->ungraceful && lsp->pipes_alive) {
		lwsl_debug("%s: stdwsi alive, not reaping\n", __func__);
		return 0;
	}

	/* we reached the reap point, no need for timeout wait */

	lws_sul_schedule(lsp->info.vh->context, lsp->info.tsi, &lsp->sul, NULL,
			 LWS_SET_TIMER_USEC_CANCEL);

	/*
	 * All the stdwsi went down, nothing more is coming... it's over
	 * Collect the final information and then reap the dead process
	 */

	if (times(&tms) != (clock_t) -1) {
		/*
		 * Cpu accounting in us
		 */
		lsp->accounting[0] = ((uint64_t)tms.tms_cstime * 1000000) / hz;
		lsp->accounting[1] = ((uint64_t)tms.tms_cutime * 1000000) / hz;
		lsp->accounting[2] = ((uint64_t)tms.tms_stime * 1000000) / hz;
		lsp->accounting[3] = ((uint64_t)tms.tms_utime * 1000000) / hz;
	}

	temp = *lsp;
	n = waitid(P_PID, lsp->child_pid, &temp.si, WEXITED | WNOHANG);
	temp.si.si_status &= 0xff; /* we use b8 + for flags */
	lwsl_notice("%s: waitd says %d, process exit %d\n",
		    __func__, n, temp.si.si_status);

	lsp->child_pid = -1;

	/* destroy the lsp itself first (it's freed and plsp set NULL */

	if (lsp->info.plsp)
		lws_spawn_piped_destroy(lsp->info.plsp);

	/* then do the parent callback informing it's destroyed */

	if (cb)
		cb(opaque, temp.accounting, &temp.si,
		   temp.we_killed_him_timeout |
			   (temp.we_killed_him_spew << 1));
#endif
	return 1; /* was reaped */
}

int
lws_spawn_piped_kill_child_process(struct lws_spawn_piped *lsp)
{
	if (!lsp->child_pid)
		return 1;

	lsp->ungraceful = 1; /* don't wait for flushing, just kill it */

	if (lws_spawn_reap(lsp))
		/* that may have invalidated lsp */
		return 0;

	TerminateProcess(lsp->child_pid, 252);
	lws_spawn_reap(lsp);

	/* that may have invalidated lsp */

	return 0;
}

/*
 * Deals with spawning a subprocess and executing it securely with stdin/out/err
 * diverted into pipes
 */

struct lws_spawn_piped *
lws_spawn_piped(const struct lws_spawn_piped_info *i)
{
	const struct lws_protocols *pcol = i->vh->context->vhost_list->protocols;
	struct lws_context *context = i->vh->context;
	struct lws_spawn_piped *lsp;
	PROCESS_INFORMATION pi;
	SECURITY_ATTRIBUTES sa;
	char cli[300], *p;
	STARTUPINFO si;
	int n;

	if (i->protocol_name)
		pcol = lws_vhost_name_to_protocol(i->vh, i->protocol_name);
	if (!pcol) {
		lwsl_err("%s: unknown protocol %s\n", __func__,
			 i->protocol_name ? i->protocol_name : "default");

		return NULL;
	}

	lsp = lws_zalloc(sizeof(*lsp), __func__);
	if (!lsp)
		return NULL;

	/* wholesale take a copy of info */
	lsp->info = *i;

	/*
	 * Prepare the stdin / out / err pipes
	 */

	for (n = 0; n < 3; n++) {
		lsp->pipe_fds[n][0] = NULL;
		lsp->pipe_fds[n][1] = NULL;
	}

	/* create pipes for [stdin|stdout] and [stderr] */

	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE; /* inherit the pipes */
	sa.lpSecurityDescriptor = NULL;

	for (n = 0; n < 3; n++) {
		if (!CreatePipe(&lsp->pipe_fds[n][0], &lsp->pipe_fds[n][1], &sa, 0))
		   goto bail1;

		/* don't inherit the pipe side that belongs to the parent */

		if (!SetHandleInformation(&lsp->pipe_fds[n][!n],
					  HANDLE_FLAG_INHERIT, 0))
		   goto bail1;
	}

	/* create wsis for each stdin/out/err fd */

	for (n = 0; n < 3; n++) {
		lsp->stdwsi[n] = lws_create_basic_wsi(i->vh->context, i->tsi,
					  i->ops ? i->ops : &role_ops_raw_file);
		if (!lsp->stdwsi[n]) {
			lwsl_err("%s: unable to create lsp stdwsi\n", __func__);
			goto bail2;
		}
		lsp->stdwsi[n]->lsp_channel = n;
		lws_vhost_bind_wsi(i->vh, lsp->stdwsi[n]);
		lsp->stdwsi[n]->protocol = pcol;
		lsp->stdwsi[n]->opaque_user_data = i->opaque;

		lwsl_debug("%s: lsp stdwsi %p: pipe idx %d -> fd %d / %d\n", __func__,
			   lsp->stdwsi[n], n, lsp->pipe_fds[n][!!(n == 0)],
			   lsp->pipe_fds[n][!(n == 0)]);

#if 0

		/* read side is 0, stdin we want the write side, others read */

		lsp->stdwsi[n]->desc.filefd = lsp->pipe_fds[n][!!(n == 0)];
		if (fcntl(lsp->pipe_fds[n][!!(n == 0)], F_SETFL, O_NONBLOCK) < 0) {
			lwsl_err("%s: setting NONBLOCK failed\n", __func__);
			goto bail2;
		}
#endif
	}

	for (n = 0; n < 3; n++) {
		if (context->event_loop_ops->sock_accept)
			if (context->event_loop_ops->sock_accept(lsp->stdwsi[n]))
				goto bail3;

		if (__insert_wsi_socket_into_fds(context, lsp->stdwsi[n]))
			goto bail3;
		if (i->opt_parent) {
			lsp->stdwsi[n]->parent = i->opt_parent;
			lsp->stdwsi[n]->sibling_list = i->opt_parent->child_list;
			i->opt_parent->child_list = lsp->stdwsi[n];
		}
	}

	if (lws_change_pollfd(lsp->stdwsi[LWS_STDIN], LWS_POLLIN, LWS_POLLOUT))
		goto bail3;
	if (lws_change_pollfd(lsp->stdwsi[LWS_STDOUT], LWS_POLLOUT, LWS_POLLIN))
		goto bail3;
	if (lws_change_pollfd(lsp->stdwsi[LWS_STDERR], LWS_POLLOUT, LWS_POLLIN))
		goto bail3;

	lwsl_notice("%s: pipe handles in %p, out %p, err %p\n", __func__,
		   lsp->stdwsi[LWS_STDIN]->desc.sockfd,
		   lsp->stdwsi[LWS_STDOUT]->desc.sockfd,
		   lsp->stdwsi[LWS_STDERR]->desc.sockfd);

	/*
	 * Windows wants a single string commandline
	 */
	p = cli;
	n = 0;
	while (i->exec_array[n]) {
		lws_strncpy(p, i->exec_array[n],
			    sizeof(cli) - lws_ptr_diff(p, cli));
		if (sizeof(cli) - lws_ptr_diff(p, cli) < 4)
			break;
		p += strlen(p);
		*p++ = ' ';
		*p = '\0';
		n++;
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));

	si.cb		= sizeof(STARTUPINFO);
	si.hStdInput	= lsp->pipe_fds[LWS_STDIN][0];
	si.hStdOutput	= lsp->pipe_fds[LWS_STDOUT][1];
	si.hStdError	= lsp->pipe_fds[LWS_STDERR][1];
	si.dwFlags	= STARTF_USESTDHANDLES | CREATE_NO_WINDOW;
	si.wShowWindow	= TRUE;

	if (!CreateProcess(NULL, cli, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		lwsl_err("%s: CreateProcess failed\n", __func__);
		goto bail3;
	}

	lsp->child_pid = pi.hProcess;

	/*
	 * Close our copies of the child-side pipe handles
	 */

	CloseHandle(lsp->pipe_fds[LWS_STDIN][0]);
	CloseHandle(lsp->pipe_fds[LWS_STDOUT][1]);
	CloseHandle(lsp->pipe_fds[LWS_STDERR][1]);

	lwsl_notice("%s: lsp %p spawned PID %d\n", __func__, lsp, lsp->child_pid);

	lws_sul_schedule(context, i->tsi, &lsp->sul, lws_spawn_timeout,
			 i->timeout_us ? i->timeout_us : 300 * LWS_US_PER_SEC);

	/*
	 *  close:                stdin:r, stdout:w, stderr:w
	 */
	for (n = 0; n < 3; n++)
		CloseHandle(lsp->pipe_fds[n][!(n == 0)]);

	lsp->pipes_alive = 3;
	lsp->created = lws_now_usecs();

	if (i->owner)
		lws_dll2_add_head(&lsp->dll, i->owner);

	if (i->timeout_us)
		lws_sul_schedule(context, i->tsi, &lsp->sul,
				 lws_spawn_timeout, i->timeout_us);

	return lsp;

bail3:

	while (--n >= 0)
		__remove_wsi_socket_from_fds(lsp->stdwsi[n]);
bail2:
	for (n = 0; n < 3; n++)
		if (lsp->stdwsi[n])
			__lws_free_wsi(lsp->stdwsi[n]);

bail1:
	for (n = 0; n < 3; n++) {
		if (lsp->pipe_fds[n][0] >= 0)
			CloseHandle(lsp->pipe_fds[n][0]);
		if (lsp->pipe_fds[n][1] >= 0)
			CloseHandle(lsp->pipe_fds[n][1]);
	}

	lws_free(lsp);

	lwsl_err("%s: failed\n", __func__);

	return NULL;
}

void
lws_spawn_stdwsi_closed(struct lws_spawn_piped *lsp)
{
	assert(lsp);
	lsp->pipes_alive--;
	lwsl_debug("%s: pipes alive %d\n", __func__, lsp->pipes_alive);
	lws_spawn_reap(lsp);
}

int
lws_spawn_get_stdfd(struct lws *wsi)
{
	return wsi->lsp_channel;
}
