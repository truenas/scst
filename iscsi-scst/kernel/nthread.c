/*
 *  Network threads.
 *
 *  Copyright (C) 2004 - 2005 FUJITA Tomonori <tomof@acm.org>
 *  Copyright (C) 2007 - 2018 Vladislav Bolkhovitin
 *  Copyright (C) 2007 - 2018 Western Digital Corporation
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/sched.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <net/tcp_states.h>
#ifdef INSIDE_KERNEL_TREE
#include <scst/iscsit_transport.h>
#else
#include "iscsit_transport.h"
#endif
#include "iscsi_trace_flag.h"
#include "iscsi.h"
#include "digest.h"

#undef DEFAULT_SYMBOL_NAMESPACE
#define DEFAULT_SYMBOL_NAMESPACE	SCST_NAMESPACE

/* Read data states */
enum rx_state {
	RX_INIT_BHS, /* Must be zero for better "switch" optimization. */
	RX_BHS,
	RX_CMD_START,
	RX_DATA,
	RX_END,

	RX_CMD_CONTINUE,
	RX_INIT_HDIGEST,
	RX_CHECK_HDIGEST,
	RX_INIT_DDIGEST,
	RX_CHECK_DDIGEST,
	RX_AHS,
	RX_PADDING,
};

enum tx_state {
	TX_INIT = 0, /* Must be zero for better "switch" optimization. */
	TX_BHS_DATA,
	TX_INIT_PADDING,
	TX_PADDING,
	TX_INIT_DDIGEST,
	TX_DDIGEST,
	TX_END,
};

static void free_pending_commands(struct iscsi_conn *conn)
{
	struct iscsi_session *session = conn->session;
	struct list_head *pending_list = &session->pending_list;
	int req_freed;
	struct iscsi_cmnd *cmnd;

	spin_lock(&session->sn_lock);
	do {
		req_freed = 0;
		list_for_each_entry(cmnd, pending_list, pending_list_entry) {
			TRACE_CONN_CLOSE_DBG("Pending cmd %p (conn %p, cmd_sn %u, exp_cmd_sn %u)",
					     cmnd, conn, cmnd->pdu.bhs.sn,
					     session->exp_cmd_sn);
			if (cmnd->conn == conn && session->exp_cmd_sn == cmnd->pdu.bhs.sn) {
				TRACE_MGMT_DBG("Freeing pending cmd %p (cmd_sn %u, exp_cmd_sn %u)",
					       cmnd, cmnd->pdu.bhs.sn,
					       session->exp_cmd_sn);

				list_del(&cmnd->pending_list_entry);
				cmnd->pending = 0;

				session->exp_cmd_sn++;

				spin_unlock(&session->sn_lock);

				req_cmnd_release_force(cmnd);

				req_freed = 1;
				spin_lock(&session->sn_lock);
				break;
			}
		}
	} while (req_freed);
	spin_unlock(&session->sn_lock);
}

static void free_orphaned_pending_commands(struct iscsi_conn *conn)
{
	struct iscsi_session *session = conn->session;
	struct list_head *pending_list = &session->pending_list;
	int req_freed;
	struct iscsi_cmnd *cmnd;

	spin_lock(&session->sn_lock);
	do {
		req_freed = 0;
		list_for_each_entry(cmnd, pending_list, pending_list_entry) {
			TRACE_CONN_CLOSE_DBG("Pending cmd %p (conn %p, cmd_sn %u, exp_cmd_sn %u)",
					     cmnd, conn, cmnd->pdu.bhs.sn,
					     session->exp_cmd_sn);
			if (cmnd->conn == conn) {
				TRACE_MGMT_DBG("Freeing orphaned pending cmnd %p (cmd_sn %u, exp_cmd_sn %u)",
					       cmnd, cmnd->pdu.bhs.sn,
					       session->exp_cmd_sn);

				list_del(&cmnd->pending_list_entry);
				cmnd->pending = 0;

				if (session->exp_cmd_sn == cmnd->pdu.bhs.sn)
					session->exp_cmd_sn++;

				spin_unlock(&session->sn_lock);

				req_cmnd_release_force(cmnd);

				req_freed = 1;
				spin_lock(&session->sn_lock);
				break;
			}
		}
	} while (req_freed);
	spin_unlock(&session->sn_lock);
}

#ifdef CONFIG_SCST_DEBUG
static void trace_conn_close(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd;

#if 0
	if (time_after(jiffies, start_waiting + 10 * HZ))
		trace_flag |= TRACE_CONN_OC_DBG;
#endif

	spin_lock_bh(&conn->cmd_list_lock);
	list_for_each_entry(cmnd, &conn->cmd_list, cmd_list_entry) {
		TRACE_CONN_CLOSE_DBG("cmd %p, scst_cmd %p, scst_state %x, scst_cmd state %d, r2t_len_to_receive %d, ref_cnt %d, sn %u, parent_req %p, pending %d",
				     cmnd, cmnd->scst_cmd, cmnd->scst_state,
				     !cmnd->parent_req && cmnd->scst_cmd ? cmnd->scst_cmd->state : -1,
				     cmnd->r2t_len_to_receive, atomic_read(&cmnd->ref_cnt),
				     cmnd->pdu.bhs.sn, cmnd->parent_req, cmnd->pending);
	}
	spin_unlock_bh(&conn->cmd_list_lock);
}
#else /* CONFIG_SCST_DEBUG */
static void trace_conn_close(struct iscsi_conn *conn) {}
#endif /* CONFIG_SCST_DEBUG */

void iscsi_task_mgmt_affected_cmds_done(struct scst_mgmt_cmd *scst_mcmd)
{
	int fn = scst_mgmt_cmd_get_fn(scst_mcmd);
	void *priv = scst_mgmt_cmd_get_tgt_priv(scst_mcmd);

	TRACE_MGMT_DBG("scst_mcmd %p, fn %d, priv %p", scst_mcmd, fn, priv);

	switch (fn) {
	case SCST_NEXUS_LOSS_SESS:
	{
		struct iscsi_conn *conn = priv;
		struct iscsi_session *sess = conn->session;
		struct iscsi_conn *c;

		if (sess->sess_reinst_successor)
			scst_reassign_retained_sess_states(sess->sess_reinst_successor->scst_sess,
							   sess->scst_sess);

		mutex_lock(&sess->target->target_mutex);

		/*
		 * We can't mark sess as shutting down earlier, because until
		 * now it might have pending commands. Otherwise, in case of
		 * reinstatement, it might lead to data corruption, because
		 * commands in being reinstated session can be executed
		 * after commands in the new session.
		 */
		sess->sess_shutting_down = 1;
		list_for_each_entry(c, &sess->conn_list, conn_list_entry) {
			if (!test_bit(ISCSI_CONN_SHUTTINGDOWN, &c->conn_aflags)) {
				sess->sess_shutting_down = 0;
				break;
			}
		}

		if (conn->conn_reinst_successor) {
			sBUG_ON(!test_bit(ISCSI_CONN_REINSTATING,
					  &conn->conn_reinst_successor->conn_aflags));
			conn_reinst_finished(conn->conn_reinst_successor);
			conn->conn_reinst_successor = NULL;
		} else if (sess->sess_reinst_successor) {
			sess_reinst_finished(sess->sess_reinst_successor);
			sess->sess_reinst_successor = NULL;
		}
		mutex_unlock(&sess->target->target_mutex);

		complete_all(&conn->ready_to_free);
		break;
	}
	case SCST_ABORT_ALL_TASKS_SESS:
	case SCST_ABORT_ALL_TASKS:
	case SCST_NEXUS_LOSS:
		sBUG();
		break;
	default:
		/* Nothing to do */
		break;
	}
}

/* No locks */
static void close_conn(struct iscsi_conn *conn)
{
	struct iscsi_session *session = conn->session;
	struct iscsi_target *target = conn->target;
	typeof(jiffies) start_waiting = jiffies;
	typeof(jiffies) shut_start_waiting = start_waiting;
	bool pending_reported = 0, wait_expired = 0, shut_expired = 0;
	uint32_t tid, cid;
	uint64_t sid;
	int rc;
	int lun = 0;

#define CONN_PENDING_TIMEOUT	((typeof(jiffies))10 * HZ)
#define CONN_WAIT_TIMEOUT	((typeof(jiffies))10 * HZ)
#define CONN_REG_SHUT_TIMEOUT	((typeof(jiffies))125 * HZ)
#define CONN_DEL_SHUT_TIMEOUT	((typeof(jiffies))10 * HZ)

	TRACE_ENTRY();

	TRACE_MGMT_DBG("Closing connection %p (conn_ref_cnt=%d)",
		       conn, atomic_read(&conn->conn_ref_cnt));

	iscsi_extracheck_is_rd_thread(conn);

	sBUG_ON(!conn->closing);

	if (conn->active_close) {
		/* We want all our already send operations to complete */
		conn->transport->iscsit_conn_close(conn, RCV_SHUTDOWN);
	} else {
		conn->transport->iscsit_conn_close(conn, RCV_SHUTDOWN | SEND_SHUTDOWN);
	}

	mutex_lock(&session->target->target_mutex);

	set_bit(ISCSI_CONN_SHUTTINGDOWN, &conn->conn_aflags);

	mutex_unlock(&session->target->target_mutex);

	rc = scst_rx_mgmt_fn_lun(session->scst_sess, SCST_NEXUS_LOSS_SESS, &lun, sizeof(lun),
				 SCST_NON_ATOMIC, conn);
	if (rc != 0)
		PRINT_ERROR("SCST_NEXUS_LOSS_SESS failed %d", rc);

	if (conn->read_state != RX_INIT_BHS) {
		struct iscsi_cmnd *cmnd = conn->read_cmnd;

		if (cmnd->scst_state == ISCSI_CMD_STATE_RX_CMD) {
			TRACE_CONN_CLOSE_DBG("Going to wait for cmnd %p to change state from RX_CMD",
					     cmnd);
		}
		wait_event(conn->read_state_waitQ, cmnd->scst_state != ISCSI_CMD_STATE_RX_CMD);

		TRACE_CONN_CLOSE_DBG("Releasing conn->read_cmnd %p (conn %p)",
				     conn->read_cmnd, conn);

		conn->read_cmnd = NULL;
		conn->read_state = RX_INIT_BHS;
		req_cmnd_release_force(cmnd);
	}

	conn_abort(conn);

	/* ToDo: not the best way to wait */
	while (atomic_read(&conn->conn_ref_cnt) != 0) {
		if (conn->conn_tm_active)
			iscsi_check_tm_data_wait_timeouts(conn, true);

		mutex_lock(&target->target_mutex);
		spin_lock(&session->sn_lock);
		if (session->tm_rsp && session->tm_rsp->conn == conn) {
			struct iscsi_cmnd *tm_rsp = session->tm_rsp;

			iscsi_drop_delayed_tm_rsp(tm_rsp);
			spin_unlock(&session->sn_lock);
			mutex_unlock(&target->target_mutex);

			rsp_cmnd_release(tm_rsp);
		} else {
			spin_unlock(&session->sn_lock);
			mutex_unlock(&target->target_mutex);
		}

		/* It's safe to check it without sn_lock */
		if (!list_empty(&session->pending_list)) {
			TRACE_CONN_CLOSE_DBG("Disposing pending commands on connection %p (conn_ref_cnt=%d)",
					     conn, atomic_read(&conn->conn_ref_cnt));

			free_pending_commands(conn);

			if (time_after(jiffies, start_waiting + CONN_PENDING_TIMEOUT)) {
				if (!pending_reported) {
					TRACE_CONN_CLOSE("Pending wait time expired");
					pending_reported = 1;
				}
				free_orphaned_pending_commands(conn);
			}
		}

		conn->transport->iscsit_make_conn_wr_active(conn);

		/* That's for active close only, actually */
		if (time_after(jiffies, start_waiting + CONN_WAIT_TIMEOUT) &&
		    !wait_expired) {
			TRACE_CONN_CLOSE("Wait time expired (conn %p)", conn);
			conn->transport->iscsit_conn_close(conn, SEND_SHUTDOWN);
			wait_expired = 1;
			shut_start_waiting = jiffies;
		}

		if (wait_expired && !shut_expired &&
		    time_after(jiffies, shut_start_waiting +
				conn->deleting ? CONN_DEL_SHUT_TIMEOUT :
						 CONN_REG_SHUT_TIMEOUT)) {
			TRACE_CONN_CLOSE("Wait time after shutdown expired (conn %p)",
					 conn);
			conn->transport->iscsit_conn_close(conn, 0);
			shut_expired = 1;
		}

		if (conn->deleting)
			msleep(200);
		else
			msleep(1000);

		TRACE_CONN_CLOSE_DBG("conn %p, conn_ref_cnt %d left, wr_state %d, exp_cmd_sn %u",
				     conn, atomic_read(&conn->conn_ref_cnt),
				     conn->wr_state, session->exp_cmd_sn);

		trace_conn_close(conn);

		if (!conn->session->sess_params.rdma_extensions) {
			/* It might never be called for being closed conn */
			__iscsi_write_space_ready(conn);
		}
	}

	if (!conn->session->sess_params.rdma_extensions) {
		write_lock_bh(&conn->sock->sk->sk_callback_lock);
		conn->sock->sk->sk_state_change = conn->old_state_change;
		conn->sock->sk->sk_data_ready = conn->old_data_ready;
		conn->sock->sk->sk_write_space = conn->old_write_space;
		write_unlock_bh(&conn->sock->sk->sk_callback_lock);
	}

	while (1) {
		bool t;

		spin_lock_bh(&conn->conn_thr_pool->wr_lock);
		t = (conn->wr_state == ISCSI_CONN_WR_STATE_IDLE);
		spin_unlock_bh(&conn->conn_thr_pool->wr_lock);

		if (t && (atomic_read(&conn->conn_ref_cnt) == 0))
			break;

		TRACE_CONN_CLOSE_DBG("Waiting for wr thread (conn %p), wr_state %x",
				     conn, conn->wr_state);
		msleep(50);
	}

	wait_for_completion(&conn->ready_to_free);

	tid = target->tid;
	sid = session->sid;
	cid = conn->cid;

	mutex_lock(&target->target_mutex);
	conn_free(conn);
	mutex_unlock(&target->target_mutex);

	/*
	 * We can't send E_CONN_CLOSE earlier, because otherwise we would have
	 * a race, when the user space tried to destroy session, which still
	 * has connections.
	 *
	 * !! All target, session and conn can be already dead here !!
	 */
	TRACE_CONN_CLOSE("Notifying user space about closing connection %p",
			 conn);
	event_send(tid, sid, cid, 0, E_CONN_CLOSE, NULL, NULL);

	TRACE_EXIT();
}

static int close_conn_thr(void *arg)
{
	struct iscsi_conn *conn = arg;

	TRACE_ENTRY();

#ifdef CONFIG_SCST_EXTRACHECKS
	/*
	 * To satisfy iscsi_extracheck_is_rd_thread() in functions called
	 * on the connection close. It is safe, because at this point conn
	 * can't be used by any other thread.
	 */
	conn->rd_task = current;
#endif
	close_conn(conn);

	TRACE_EXIT();
	return 0;
}

/* No locks */
void start_close_conn(struct iscsi_conn *conn)
{
	struct task_struct *t;

	TRACE_ENTRY();

	t = kthread_run(close_conn_thr, conn, "iscsi_conn_cleanup");
	if (IS_ERR(t)) {
		PRINT_ERROR("kthread_run() failed (%ld), closing conn %p directly",
			    PTR_ERR(t), conn);
		close_conn(conn);
	}

	TRACE_EXIT();
}
EXPORT_SYMBOL(start_close_conn);

static inline void iscsi_conn_init_read(struct iscsi_conn *conn, void *data, size_t len)
{
	conn->read_iov[0].iov_base = data;
	conn->read_iov[0].iov_len = len;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	iov_iter_kvec(&conn->read_msg.msg_iter, READ, conn->read_iov, 1, len);
#else
	conn->read_msg.msg_iov = conn->read_iov;
	conn->read_msg.msg_iovlen = 1;
	conn->read_size = len;
#endif
}

static void iscsi_conn_prepare_read_ahs(struct iscsi_conn *conn, struct iscsi_cmnd *cmnd)
{
	int asize = (cmnd->pdu.ahssize + 3) & -4;

	/* ToDo: __GFP_NOFAIL ?? */
	cmnd->pdu.ahs = kmalloc(asize, __GFP_NOFAIL | GFP_KERNEL);
	sBUG_ON(!cmnd->pdu.ahs);
	iscsi_conn_init_read(conn, cmnd->pdu.ahs, asize);
}

struct iscsi_cmnd *iscsi_get_send_cmnd(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd = NULL;

	spin_lock_bh(&conn->write_list_lock);
	if (!list_empty(&conn->write_list)) {
		cmnd = list_first_entry(&conn->write_list, struct iscsi_cmnd, write_list_entry);
		cmd_del_from_write_list(cmnd);
		cmnd->write_processing_started = 1;
	} else {
		spin_unlock_bh(&conn->write_list_lock);
		goto out;
	}
	spin_unlock_bh(&conn->write_list_lock);

	if (unlikely(test_bit(ISCSI_CMD_ABORTED, &cmnd->parent_req->prelim_compl_flags))) {
		TRACE_MGMT_DBG("Going to send acmd %p (scst cmd %p, state %d, parent_req %p)",
			       cmnd, cmnd->scst_cmd, cmnd->scst_state,
			       cmnd->parent_req);
	}

	if (unlikely(cmnd_opcode(cmnd) == ISCSI_OP_SCSI_TASK_MGT_RSP)) {
#ifdef CONFIG_SCST_DEBUG
		struct iscsi_task_mgt_hdr *req_hdr =
			(struct iscsi_task_mgt_hdr *)&cmnd->parent_req->pdu.bhs;
		struct iscsi_task_rsp_hdr *rsp_hdr =
			(struct iscsi_task_rsp_hdr *)&cmnd->pdu.bhs;
		TRACE_MGMT_DBG("Going to send TM response %p (status %d, fn %d, parent_req %p)",
			       cmnd, rsp_hdr->response,
			       req_hdr->function & ISCSI_FUNCTION_MASK,
			       cmnd->parent_req);
#endif
	}

out:
	return cmnd;
}
EXPORT_SYMBOL(iscsi_get_send_cmnd);

/* Returns number of bytes left to receive or <0 for error */
static int do_recv(struct iscsi_conn *conn)
{
	int res;
	mm_segment_t oldfs;
	struct msghdr *msg;
	int read_size;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	struct iovec *first_iov;
	int first_len;
#endif

	EXTRACHECKS_BUG_ON(!conn->read_cmnd);

	if (unlikely(conn->closing)) {
		res = -EIO;
		goto out;
	}

	/*
	 * We suppose that if sock_recvmsg() returned less data than requested,
	 * then next time it will return -EAGAIN, so there's no point to call
	 * it again.
	 */

restart:
	msg = &conn->read_msg;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	read_size = msg->msg_iter.count;
#else
	read_size = conn->read_size;
	first_iov = msg->msg_iov;
	first_len = first_iov->iov_len;
#endif

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	res = sock_recvmsg(conn->sock, msg,
#if SOCK_RECVMSG_HAS_FOUR_ARGS
			   read_size,
#endif
			   MSG_DONTWAIT | MSG_NOSIGNAL);
	set_fs(oldfs);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	TRACE_DBG("nr_segs %ld, bytes_left %zd, res %d",
		  msg->msg_iter.nr_segs, msg->msg_iter.count, res);
#else
	TRACE_DBG("msg_iovlen %zd, read_size %d, res %d", msg->msg_iovlen,
		  read_size, res);
#endif

	if (res > 0) {
		/*
		 * To save CPU cycles we suppose that sock_recvmsg() adjusts
		 * msg->msg_iov and msg->msg_iovlen. The BUG_ON() statement
		 * below verifies this.
		 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
		sBUG_ON(msg->msg_iter.count + res != read_size);
		res = msg->msg_iter.count;
#else
		sBUG_ON((res >= first_len) && (first_iov->iov_len != 0));
		conn->read_size -= res;
		res = conn->read_size;
#endif
	} else {
		switch (res) {
		case -EAGAIN:
			TRACE_DBG("EAGAIN received for conn %p", conn);
			res = read_size;
			break;
		case -ERESTARTSYS:
			TRACE_DBG("ERESTARTSYS received for conn %p", conn);
			goto restart;
		default:
			if (!conn->closing) {
				PRINT_ERROR("sock_recvmsg() failed: %d (conn %p)",
					    res, conn);
				mark_conn_closed(conn);
			}
			if (res == 0)
				res = -EIO;
			break;
		}
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

static int iscsi_rx_check_ddigest(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd = conn->read_cmnd;
	int res;

	res = do_recv(conn);
	if (res == 0) {
		conn->read_state = RX_END;

		if (cmnd->pdu.datasize <= 16 * 1024) {
			/*
			 * It's cache hot, so let's compute it inline. The
			 * choice here about what will expose more latency:
			 * possible cache misses or the digest calculation.
			 */
			TRACE_DBG("cmnd %p, opcode %x: checking RX ddigest inline",
				  cmnd, cmnd_opcode(cmnd));
			cmnd->ddigest_checked = 1;
			res = digest_rx_data(cmnd);
			if (unlikely(res != 0)) {
				struct iscsi_cmnd *orig_req;

				if (cmnd_opcode(cmnd) == ISCSI_OP_SCSI_DATA_OUT)
					orig_req = cmnd->cmd_req;
				else
					orig_req = cmnd;
				if (unlikely(!orig_req->scst_cmd)) {
					/* Just drop it */
					iscsi_preliminary_complete(cmnd,
								   orig_req,
								   false);
				} else {
					set_scst_preliminary_status_rsp(orig_req, false,
							SCST_LOAD_SENSE(iscsi_sense_crc_error));
					/*
					 * Let's prelim complete cmnd too to
					 * handle the DATA OUT case
					 */
					iscsi_preliminary_complete(cmnd,
								   orig_req,
								   false);
				}
				res = 0;
			}
		} else if (cmnd_opcode(cmnd) == ISCSI_OP_SCSI_CMD) {
			cmd_add_on_rx_ddigest_list(cmnd, cmnd);
			cmnd_get(cmnd);
		} else if (cmnd_opcode(cmnd) != ISCSI_OP_SCSI_DATA_OUT) {
			/*
			 * We could get here only for Nop-Out. ISCSI RFC
			 * doesn't specify how to deal with digest errors in
			 * this case. Let's just drop the command.
			 */
			TRACE_DBG("cmnd %p, opcode %x: checking NOP RX ddigest",
				  cmnd, cmnd_opcode(cmnd));
			res = digest_rx_data(cmnd);
			if (unlikely(res != 0)) {
				iscsi_preliminary_complete(cmnd, cmnd, false);
				res = 0;
			}
		}
	}

	return res;
}

/* No locks, conn is rd processing */
static int process_read_io(struct iscsi_conn *conn, int *closed)
{
	struct iscsi_cmnd *cmnd = conn->read_cmnd;
	int bytes_left, res;

	TRACE_ENTRY();

	/* In case of error cmnd will be freed in close_conn() */

	do {
		switch (conn->read_state) {
		case RX_INIT_BHS:
			EXTRACHECKS_BUG_ON(conn->read_cmnd);
			cmnd = conn->transport->iscsit_alloc_cmd(conn, NULL);
			conn->read_cmnd = cmnd;
			iscsi_conn_init_read(cmnd->conn, &cmnd->pdu.bhs,
					     sizeof(cmnd->pdu.bhs));
			conn->read_state = RX_BHS;
			fallthrough;

		case RX_BHS:
			res = do_recv(conn);
			if (res == 0) {
				/*
				 * This command not yet received on the
				 * aborted time, so shouldn't be affected by
				 * any abort.  It should not be affected by
				 * conn_abort() as well, because close
				 * connection is initiated from single (this)
				 * read thread, so conn_abort() call stack can
				 * not be initiated in parallel to receive all
				 * the data event (do_recv() has check of
				 * conn->closing in the beginning)
				 */
				EXTRACHECKS_BUG_ON(cmnd->prelim_compl_flags != 0);

				iscsi_cmnd_get_length(&cmnd->pdu);

				if (cmnd->pdu.ahssize == 0) {
					if ((conn->hdigest_type & DIGEST_NONE) == 0)
						conn->read_state = RX_INIT_HDIGEST;
					else
						conn->read_state = RX_CMD_START;
				} else {
					iscsi_conn_prepare_read_ahs(conn, cmnd);
					conn->read_state = RX_AHS;
				}
			}
			break;

		case RX_CMD_START:
			res = cmnd_rx_start(cmnd);
			if (res == 0) {
				if (cmnd->pdu.datasize == 0)
					conn->read_state = RX_END;
				else
					conn->read_state = RX_DATA;
			} else if (res > 0) {
				conn->read_state = RX_CMD_CONTINUE;
			} else {
				sBUG_ON(!conn->closing);
			}
			break;

		case RX_CMD_CONTINUE:
			if (cmnd->scst_state == ISCSI_CMD_STATE_RX_CMD) {
				TRACE_DBG("cmnd %p is still in RX_CMD state",
					  cmnd);
				res = 1;
				break;
			}
			res = cmnd_rx_continue(cmnd);
			if (unlikely(res != 0)) {
				sBUG_ON(!conn->closing);
			} else {
				if (cmnd->pdu.datasize == 0)
					conn->read_state = RX_END;
				else
					conn->read_state = RX_DATA;
			}
			break;

		case RX_DATA:
			res = do_recv(conn);
			if (res == 0) {
				int psz = ((cmnd->pdu.datasize + 3) & -4) -
					cmnd->pdu.datasize;

				if (psz != 0) {
					TRACE_DBG("padding %d bytes", psz);
					iscsi_conn_init_read(conn, &conn->rpadding, psz);
					conn->read_state = RX_PADDING;
				} else if ((conn->ddigest_type & DIGEST_NONE) != 0) {
					conn->read_state = RX_END;
				} else {
					conn->read_state = RX_INIT_DDIGEST;
				}
			}
			break;

		case RX_END:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
			bytes_left = conn->read_msg.msg_iter.count;
#else
			bytes_left = conn->read_size;
#endif
			if (unlikely(bytes_left != 0)) {
				PRINT_CRIT_ERROR("conn read_size !=0 on RX_END (conn %p, op %x, read_size %d)",
						 conn, cmnd_opcode(cmnd),
						 bytes_left);
				sBUG();
			}
			conn->read_cmnd = NULL;
			conn->read_state = RX_INIT_BHS;

			cmnd_rx_end(cmnd);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
			EXTRACHECKS_BUG_ON(conn->read_msg.msg_iter.count != 0);
#else
			EXTRACHECKS_BUG_ON(conn->read_size != 0);
#endif

			/*
			 * To maintain fairness. Res must be 0 here anyway, the
			 * assignment is only to remove compiler warning about
			 * uninitialized variable.
			 */
			res = 0;
			goto out;

		case RX_INIT_HDIGEST:
			iscsi_conn_init_read(conn, &cmnd->hdigest, sizeof(u32));
			conn->read_state = RX_CHECK_HDIGEST;
			fallthrough;

		case RX_CHECK_HDIGEST:
			res = do_recv(conn);
			if (res == 0) {
				res = digest_rx_header(cmnd);
				if (unlikely(res != 0)) {
					PRINT_ERROR("rx header digest for initiator %s (conn %p) failed (%d)",
						    conn->session->initiator_name,
						    conn, res);
					mark_conn_closed(conn);
				} else {
					conn->read_state = RX_CMD_START;
				}
			}
			break;

		case RX_INIT_DDIGEST:
			iscsi_conn_init_read(conn, &cmnd->ddigest, sizeof(u32));
			conn->read_state = RX_CHECK_DDIGEST;
			fallthrough;

		case RX_CHECK_DDIGEST:
			res = iscsi_rx_check_ddigest(conn);
			break;

		case RX_AHS:
			res = do_recv(conn);
			if (res == 0) {
				if ((conn->hdigest_type & DIGEST_NONE) == 0)
					conn->read_state = RX_INIT_HDIGEST;
				else
					conn->read_state = RX_CMD_START;
			}
			break;

		case RX_PADDING:
			res = do_recv(conn);
			if (res == 0) {
				if ((conn->ddigest_type & DIGEST_NONE) == 0)
					conn->read_state = RX_INIT_DDIGEST;
				else
					conn->read_state = RX_END;
			}
			break;

		default:
			PRINT_CRIT_ERROR("%d %x", conn->read_state,
					 cmnd_opcode(cmnd));
			res = -1; /* to keep compiler happy */
			sBUG();
		}
	} while (res == 0);

	if (unlikely(conn->closing)) {
		start_close_conn(conn);
		*closed = 1;
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

/*
 * Called under rd_lock and BHs disabled, but will drop it inside,
 * then reacquire.
 */
static void scst_do_job_rd(struct iscsi_thread_pool *p)
	__acquires(&rd_lock)
	__releases(&rd_lock)
{
	TRACE_ENTRY();

	/*
	 * We delete/add to tail connections to maintain fairness between them.
	 */

	while (!list_empty(&p->rd_list)) {
		int closed = 0, rc;
		struct iscsi_conn *conn = list_first_entry(&p->rd_list,
			typeof(*conn), rd_list_entry);

		list_del(&conn->rd_list_entry);

		sBUG_ON(conn->rd_state == ISCSI_CONN_RD_STATE_PROCESSING);
		conn->rd_data_ready = 0;
		conn->rd_state = ISCSI_CONN_RD_STATE_PROCESSING;
#ifdef CONFIG_SCST_EXTRACHECKS
		conn->rd_task = current;
#endif
		spin_unlock_bh(&p->rd_lock);

		rc = process_read_io(conn, &closed);

		spin_lock_bh(&p->rd_lock);

		if (unlikely(closed))
			continue;

		if (unlikely(conn->conn_tm_active)) {
			spin_unlock_bh(&p->rd_lock);
			iscsi_check_tm_data_wait_timeouts(conn, false);
			spin_lock_bh(&p->rd_lock);
		}

#ifdef CONFIG_SCST_EXTRACHECKS
		conn->rd_task = NULL;
#endif
		if (rc == 0 || conn->rd_data_ready) {
			list_add_tail(&conn->rd_list_entry, &p->rd_list);
			conn->rd_state = ISCSI_CONN_RD_STATE_IN_LIST;
		} else {
			conn->rd_state = ISCSI_CONN_RD_STATE_IDLE;
		}
	}

	TRACE_EXIT();
}

static inline int test_rd_list(struct iscsi_thread_pool *p)
{
	int res = !list_empty(&p->rd_list) ||
		  unlikely(kthread_should_stop());
	return res;
}

int istrd(void *arg)
{
	struct iscsi_thread_pool *p = arg;
	int rc;

	TRACE_ENTRY();

	PRINT_INFO("Read thread for pool %p started", p);

	current->flags |= PF_NOFREEZE;
	rc = set_cpus_allowed_ptr(current, &p->cpu_mask);
	if (rc != 0)
		PRINT_ERROR("Setting CPU affinity failed: %d", rc);

	spin_lock_bh(&p->rd_lock);
	while (!kthread_should_stop()) {
		scst_wait_event_interruptible_lock_bh(p->rd_waitQ, test_rd_list(p), p->rd_lock);
		scst_do_job_rd(p);
	}
	spin_unlock_bh(&p->rd_lock);

	/*
	 * If kthread_should_stop() is true, we are guaranteed to be
	 * on the module unload, so rd_list must be empty.
	 */
	sBUG_ON(!list_empty(&p->rd_list));

	PRINT_INFO("Read thread for pool %p finished", p);

	TRACE_EXIT();
	return 0;
}

void req_add_to_write_timeout_list(struct iscsi_cmnd *req)
{
	struct iscsi_conn *conn;
	bool set_conn_tm_active = false;

	TRACE_ENTRY();

	if (req->on_write_timeout_list)
		goto out;

	conn = req->conn;

	TRACE_DBG("Adding req %p to conn %p write_timeout_list",
		  req, conn);

	spin_lock_bh(&conn->write_list_lock);

	/* Recheck, since it can be changed behind us */
	if (unlikely(req->on_write_timeout_list)) {
		spin_unlock_bh(&conn->write_list_lock);
		goto out;
	}

	req->on_write_timeout_list = 1;
	req->write_start = jiffies;

	if (unlikely(cmnd_opcode(req) == ISCSI_OP_NOP_OUT)) {
		unsigned long req_tt = iscsi_get_timeout_time(req);
		struct iscsi_cmnd *r;
		bool inserted = false;

		list_for_each_entry(r, &conn->write_timeout_list, write_timeout_list_entry) {
			unsigned long tt = iscsi_get_timeout_time(r);

			if (time_after(tt, req_tt)) {
				TRACE_DBG("Add NOP IN req %p (tt %ld) before req %p (tt %ld)",
					  req, req_tt, r, tt);
				list_add_tail(&req->write_timeout_list_entry,
					      &r->write_timeout_list_entry);
				inserted = true;
				break;
			}

			TRACE_DBG("Skipping op %x req %p (tt %ld)",
				  cmnd_opcode(r), r, tt);
		}
		if (!inserted) {
			TRACE_DBG("Add NOP IN req %p in the tail", req);
			list_add_tail(&req->write_timeout_list_entry, &conn->write_timeout_list);
		}

		/* We suppose that nop_in_timeout must be <= data_rsp_timeout */
		req_tt += ISCSI_ADD_SCHED_TIME;
		if (timer_pending(&conn->rsp_timer) &&
		    time_after(conn->rsp_timer.expires, req_tt)) {
			TRACE_DBG("Timer adjusted for sooner expired NOP IN req %p",
				  req);
			mod_timer(&conn->rsp_timer, req_tt);
		}
	} else {
		list_add_tail(&req->write_timeout_list_entry, &conn->write_timeout_list);
	}

	if (!timer_pending(&conn->rsp_timer)) {
		unsigned long timeout_time;

		if (unlikely(conn->conn_tm_active ||
			     test_bit(ISCSI_CMD_ABORTED, &req->prelim_compl_flags))) {
			set_conn_tm_active = true;
			timeout_time = req->write_start +
					ISCSI_TM_DATA_WAIT_TIMEOUT;
		} else {
			timeout_time = iscsi_get_timeout_time(req);
		}

		timeout_time += ISCSI_ADD_SCHED_TIME;

		TRACE_DBG("Starting timer on %ld (con %p, write_start %ld)",
			  timeout_time, conn, req->write_start);

		conn->rsp_timer.expires = timeout_time;
		add_timer(&conn->rsp_timer);
	} else if (unlikely(test_bit(ISCSI_CMD_ABORTED, &req->prelim_compl_flags))) {
		unsigned long timeout_time = jiffies +
			ISCSI_TM_DATA_WAIT_TIMEOUT + ISCSI_ADD_SCHED_TIME;

		set_conn_tm_active = true;
		if (time_after(conn->rsp_timer.expires, timeout_time)) {
			TRACE_MGMT_DBG("Mod timer on %ld (conn %p)",
				       timeout_time, conn);
			mod_timer(&conn->rsp_timer, timeout_time);
		}
	}

	spin_unlock_bh(&conn->write_list_lock);

	/*
	 * conn_tm_active can be already cleared by
	 * iscsi_check_tm_data_wait_timeouts(). write_list_lock is an inner
	 * lock for rd_lock.
	 */
	if (unlikely(set_conn_tm_active)) {
		spin_lock_bh(&conn->conn_thr_pool->rd_lock);
		TRACE_MGMT_DBG("Setting conn_tm_active for conn %p", conn);
		conn->conn_tm_active = 1;
		spin_unlock_bh(&conn->conn_thr_pool->rd_lock);
	}

out:
	TRACE_EXIT();
}

static int write_data(struct iscsi_conn *conn)
{
	struct socket *sock;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
	ssize_t (*sock_sendpage)(struct socket *sock, struct page *page,
				 int offset, size_t size, int flags);
#else
	struct msghdr msg = {};
	struct bio_vec bvec;
#endif
	struct iscsi_cmnd *write_cmnd, *parent_req;
	struct iscsi_cmnd *ref_cmd;
	struct page *page;
	struct scatterlist *sg;
	size_t saved_size, size, sg_size;
	size_t sendsize, length;
	int offset, idx, flags, res = 0;
	bool ref_cmd_to_parent;

	TRACE_ENTRY();

	write_cmnd = conn->write_cmnd;
	parent_req = write_cmnd->parent_req;

	iscsi_extracheck_is_wr_thread(conn);

	if (!write_cmnd->own_sg) {
		ref_cmd = parent_req;
		ref_cmd_to_parent = true;
	} else {
		ref_cmd = write_cmnd;
		ref_cmd_to_parent = false;
	}

	req_add_to_write_timeout_list(parent_req);

	size = conn->write_size;
	saved_size = size;

	if (conn->write_iop) {
		struct file *file = conn->file;
		struct kvec *iop = conn->write_iop;
		int count = conn->write_iop_used;
		loff_t off;

		sBUG_ON(count > ARRAY_SIZE(conn->write_iov));

		while (true) {
			off = 0;

			res = scst_writev(file, iop, count, &off);
			TRACE_WRITE("sid %#Lx, cid %u, res %d, iov_len %zd",
				    (unsigned long long)conn->session->sid,
				    conn->cid, res, iop->iov_len);

			if (unlikely(res <= 0)) {
				if (res == -EINTR)
					continue;

				if (res == -EAGAIN) {
					conn->write_iop = iop;
					conn->write_iop_used = count;
					goto out_iov;
				}

				goto out_err;
			}

			size -= res;

			while ((typeof(res))iop->iov_len <= res && res) {
				res -= iop->iov_len;
				iop++;
				count--;
			}

			if (count == 0) {
				conn->write_iop = NULL;
				conn->write_iop_used = 0;
				if (size)
					break;
				goto out_iov;
			}
			sBUG_ON(iop >
				conn->write_iov + ARRAY_SIZE(conn->write_iov));
			iop->iov_base += res;
			iop->iov_len -= res;
		}
	}

	sg = write_cmnd->sg;
	if (unlikely(!sg)) {
		PRINT_INFO("WARNING: Data missed (cmd %p)!", write_cmnd);
		res = 0;
		goto out;
	}

	sock = conn->sock;
	flags = MSG_DONTWAIT;

	if (sg != write_cmnd->rsp_sg &&
	    (!parent_req->scst_cmd || parent_req->scst_state == ISCSI_CMD_STATE_AEN ||
	     !scst_cmd_get_dh_data_buff_alloced(parent_req->scst_cmd)))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
		flags |= MSG_SPLICE_PAGES;
#else
		sock_sendpage = sock->ops->sendpage;
	else
		sock_sendpage = sock_no_sendpage;
#endif

	sg_size = size;

	if (sg != write_cmnd->rsp_sg) {
		/*
		 * Data scatterlist. It is assumed that only the first element
		 * has a non-zero offset and that all elements except the
		 * first and the last have a length that is equal to
		 * PAGE_SIZE.
		 */
		offset = conn->write_offset + sg[0].offset;
		idx = offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
		length = min_t(size_t, size, PAGE_SIZE - offset);
		TRACE_WRITE("write_offset %d, sg_size %lu, idx %d, offset %d, length %lu",
			    conn->write_offset, sg_size, idx, offset, length);
	} else {
		/*
		 * Response scatterlist. No assumptions are made about the
		 * offset nor about the length of scatterlist elements.
		 */
		idx = 0;
		offset = conn->write_offset;
		while (offset >= sg[idx].length) {
			offset -= sg[idx].length;
			idx++;
		}
		length = sg[idx].length - offset;
		offset += sg[idx].offset;
		TRACE_WRITE("rsp_sg: write_offset %d, sg_size %lu, idx %d, offset %d, length %lu",
			    conn->write_offset, sg_size, idx, offset, length);
	}
	page = sg_page(&sg[idx]);

	while (true) {
		sendsize = min_t(size_t, size, length);

		if (sendsize == size)
			flags &= ~MSG_MORE;
		else
			flags |= MSG_MORE;

		while (sendsize) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
			res = sock_sendpage(sock, page, offset, sendsize, flags);
#else
			memset(&msg, 0, sizeof(struct msghdr));
			msg.msg_flags = flags;

			bvec_set_page(&bvec, page, sendsize, offset);
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, sendsize);
			res = sock_sendmsg(sock, &msg);
#endif
			TRACE_WRITE("sid %#Lx cid %u: res %d (page[%p] index %lu, offset %u, sendsize %lu, size %lu, cmd %p)",
				    (unsigned long long)conn->session->sid, conn->cid,
				    res, page, page->index, offset, sendsize, size,
				    write_cmnd);

			if (unlikely(res <= 0)) {
				if (res == -EINTR)
					continue;

				if (res == -EAGAIN) {
					conn->write_offset += sg_size - size;
					goto out_iov;
				}

				goto out_err;
			}

			offset += res;
			sendsize -= res;
			size -= res;
		}

		if (size == 0)
			goto out_iov;

		idx++;
		EXTRACHECKS_BUG_ON(idx >= ref_cmd->sg_cnt);
		page = sg_page(&sg[idx]);
		length = sg[idx].length;
		offset = sg[idx].offset;
	}

out_iov:
	conn->write_size = size;

	if (res != -EAGAIN || saved_size != size)
		res = saved_size - size;

out:
	TRACE_EXIT_RES(res);
	return res;

out_err:
#ifndef CONFIG_SCST_DEBUG
	if (!conn->closing) {
#else
	{
#endif
		PRINT_ERROR("error %d at sid:cid %#Lx:%u, cmnd %p", res,
			    (unsigned long long)conn->session->sid,
			    conn->cid, conn->write_cmnd);
	}

	if (ref_cmd_to_parent) {
		if (ref_cmd->scst_state == ISCSI_CMD_STATE_AEN) {
			if (ref_cmd->scst_aen)
				scst_set_aen_delivery_status(ref_cmd->scst_aen,
							     SCST_AEN_RES_FAILED);
		} else {
			if (ref_cmd->scst_cmd)
				scst_set_delivery_status(ref_cmd->scst_cmd,
							 SCST_CMD_DELIVERY_FAILED);
		}
	}

	goto out;
}

static void exit_tx(struct iscsi_conn *conn, int res)
{
	iscsi_extracheck_is_wr_thread(conn);

	switch (res) {
	case -EAGAIN:
	case -ERESTARTSYS:
		break;
	default:
#ifndef CONFIG_SCST_DEBUG
		if (!conn->closing) {
#else
		{
#endif
			PRINT_ERROR("Sending data failed: initiator %s (conn %p), write_size %d, write_state %d, res %d",
				    conn->session->initiator_name, conn,
				    conn->write_size,
				    conn->write_state, res);
		}
		conn->write_state = TX_END;
		conn->write_size = 0;
		mark_conn_closed(conn);
		break;
	}
}

static int tx_ddigest(struct iscsi_cmnd *cmnd, int state)
{
	int res, rest = cmnd->conn->write_size;
	struct msghdr msg = {.msg_flags = MSG_NOSIGNAL | MSG_DONTWAIT};
	struct kvec iov;

	iscsi_extracheck_is_wr_thread(cmnd->conn);

	TRACE_DBG("Sending data digest %x (cmd %p)", cmnd->ddigest, cmnd);

	iov.iov_base = (char *)(&cmnd->ddigest) + (sizeof(u32) - rest);
	iov.iov_len = rest;

	res = kernel_sendmsg(cmnd->conn->sock, &msg, &iov, 1, rest);
	if (res > 0) {
		cmnd->conn->write_size -= res;
		if (!cmnd->conn->write_size)
			cmnd->conn->write_state = state;
	} else {
		exit_tx(cmnd->conn, res);
	}

	return res;
}

static void init_tx_hdigest(struct iscsi_cmnd *cmnd)
{
	struct iscsi_conn *conn = cmnd->conn;
	struct kvec *iop;

	iscsi_extracheck_is_wr_thread(conn);

	digest_tx_header(cmnd);

	sBUG_ON(conn->write_iop_used >= ARRAY_SIZE(conn->write_iov));

	iop = &conn->write_iop[conn->write_iop_used];
	conn->write_iop_used++;
	iop->iov_base = &cmnd->hdigest;
	iop->iov_len = sizeof(u32);
	conn->write_size += sizeof(u32);
}

static int tx_padding(struct iscsi_cmnd *cmnd, int state)
{
	int res, rest = cmnd->conn->write_size;
	struct msghdr msg = {.msg_flags = MSG_NOSIGNAL | MSG_DONTWAIT};
	struct kvec iov;
	static const uint32_t padding;

	BUG_ON(rest < 1);
	BUG_ON(rest >= sizeof(uint32_t));
	iscsi_extracheck_is_wr_thread(cmnd->conn);

	TRACE_DBG("Sending %d padding bytes (cmd %p)", rest, cmnd);

	iov.iov_base = (char *)&padding;
	iov.iov_len = rest;

	res = kernel_sendmsg(cmnd->conn->sock, &msg, &iov, 1, rest);
	if (res > 0) {
		cmnd->conn->write_size -= res;
		if (!cmnd->conn->write_size)
			cmnd->conn->write_state = state;
	} else {
		exit_tx(cmnd->conn, res);
	}

	return res;
}

static int iscsi_do_send(struct iscsi_conn *conn, int state)
{
	int res;

	iscsi_extracheck_is_wr_thread(conn);

	res = write_data(conn);
	if (res > 0) {
		if (!conn->write_size)
			conn->write_state = state;
	} else {
		exit_tx(conn, res);
	}

	return res;
}

/*
 * No locks, conn is wr processing.
 *
 * IMPORTANT! Connection conn must be protected by additional conn_get()
 * upon entrance in this function, because otherwise it could be destroyed
 * inside as a result of cmnd release.
 */
int iscsi_send(struct iscsi_conn *conn)
{
	struct iscsi_cmnd *cmnd = conn->write_cmnd;
	int ddigest, res = 0;

	TRACE_ENTRY();

	TRACE_DBG("conn %p, write_cmnd %p", conn, cmnd);

	iscsi_extracheck_is_wr_thread(conn);

	ddigest = conn->ddigest_type != DIGEST_NONE ? 1 : 0;

	switch (conn->write_state) {
	case TX_INIT:
		sBUG_ON(cmnd);
		conn->write_cmnd = iscsi_get_send_cmnd(conn);
		if (!conn->write_cmnd)
			goto out;

		cmnd = conn->write_cmnd;
		cmnd_tx_start(cmnd);
		if (!(conn->hdigest_type & DIGEST_NONE))
			init_tx_hdigest(cmnd);
		conn->write_state = TX_BHS_DATA;
		fallthrough;
	case TX_BHS_DATA:
		res = iscsi_do_send(conn, cmnd->pdu.datasize ? TX_INIT_PADDING : TX_END);
		if (res <= 0 || conn->write_state != TX_INIT_PADDING)
			break;
		fallthrough;
	case TX_INIT_PADDING:
		cmnd->conn->write_size = ((cmnd->pdu.datasize + 3) & -4) - cmnd->pdu.datasize;
		if (cmnd->conn->write_size != 0)
			conn->write_state = TX_PADDING;
		else if (ddigest)
			conn->write_state = TX_INIT_DDIGEST;
		else
			conn->write_state = TX_END;
		break;
	case TX_PADDING:
		res = tx_padding(cmnd, ddigest ? TX_INIT_DDIGEST : TX_END);
		if (res <= 0 || conn->write_state != TX_INIT_DDIGEST)
			break;
		fallthrough;
	case TX_INIT_DDIGEST:
		cmnd->conn->write_size = sizeof(u32);
		conn->write_state = TX_DDIGEST;
		fallthrough;
	case TX_DDIGEST:
		res = tx_ddigest(cmnd, TX_END);
		break;
	default:
		PRINT_CRIT_ERROR("%d %d %x",
				 res, conn->write_state, cmnd_opcode(cmnd));
		sBUG();
	}

	if (conn->write_state != TX_END)
		goto out;

	if (unlikely(conn->write_size)) {
		PRINT_CRIT_ERROR("%d %x %u",
				 res, cmnd_opcode(cmnd), conn->write_size);
		sBUG();
	}
	cmnd_tx_end(cmnd);

	rsp_cmnd_release(cmnd);

	conn->write_cmnd = NULL;
	conn->write_state = TX_INIT;

out:
	TRACE_EXIT_RES(res);
	return res;
}

/*
 * Called under wr_lock and BHs disabled, but will drop it inside,
 * then reacquire.
 */
static void scst_do_job_wr(struct iscsi_thread_pool *p)
	__acquires(&wr_lock)
	__releases(&wr_lock)
{
	TRACE_ENTRY();

	/*
	 * We delete/add to tail connections to maintain fairness between them.
	 */

	while (!list_empty(&p->wr_list)) {
		int rc;
		struct iscsi_conn *conn =
			list_first_entry(&p->wr_list, typeof(*conn), wr_list_entry);

		TRACE_DBG("conn %p, wr_state %x, wr_space_ready %d, write ready %d",
			  conn, conn->wr_state,
			  conn->wr_space_ready, test_write_ready(conn));

		list_del(&conn->wr_list_entry);

		sBUG_ON(conn->wr_state == ISCSI_CONN_WR_STATE_PROCESSING);

		conn->wr_state = ISCSI_CONN_WR_STATE_PROCESSING;
		conn->wr_space_ready = 0;
#ifdef CONFIG_SCST_EXTRACHECKS
		conn->wr_task = current;
#endif
		spin_unlock_bh(&p->wr_lock);

		conn_get(conn);

		rc = iscsi_send(conn);

		spin_lock_bh(&p->wr_lock);
#ifdef CONFIG_SCST_EXTRACHECKS
		conn->wr_task = NULL;
#endif
		if (rc == -EAGAIN && !conn->wr_space_ready) {
			TRACE_DBG("EAGAIN, setting WR_STATE_SPACE_WAIT (conn %p)",
				  conn);
			conn->wr_state = ISCSI_CONN_WR_STATE_SPACE_WAIT;
		} else if (test_write_ready(conn)) {
			list_add_tail(&conn->wr_list_entry, &p->wr_list);
			conn->wr_state = ISCSI_CONN_WR_STATE_IN_LIST;
		} else {
			conn->wr_state = ISCSI_CONN_WR_STATE_IDLE;
		}

		conn_put(conn);
	}

	TRACE_EXIT();
}

static inline int test_wr_list(struct iscsi_thread_pool *p)
{
	int res = !list_empty(&p->wr_list) || unlikely(kthread_should_stop());

	return res;
}

int istwr(void *arg)
{
	struct iscsi_thread_pool *p = arg;
	int rc;

	TRACE_ENTRY();

	PRINT_INFO("Write thread for pool %p started", p);

	current->flags |= PF_NOFREEZE;
	rc = set_cpus_allowed_ptr(current, &p->cpu_mask);
	if (rc != 0)
		PRINT_ERROR("Setting CPU affinity failed: %d", rc);

	spin_lock_bh(&p->wr_lock);
	while (!kthread_should_stop()) {
		scst_wait_event_interruptible_lock_bh(p->wr_waitQ, test_wr_list(p), p->wr_lock);
		scst_do_job_wr(p);
	}
	spin_unlock_bh(&p->wr_lock);

	/*
	 * If kthread_should_stop() is true, we are guaranteed to be
	 * on the module unload, so wr_list must be empty.
	 */
	sBUG_ON(!list_empty(&p->wr_list));

	PRINT_INFO("Write thread for pool %p finished", p);

	TRACE_EXIT();
	return 0;
}
