/*
* Portions Copyright 1994-2004 The PL-J Project. All rights reserved.
* Portions Copyright © 2016-Present Pivotal Software, Inc.
*/


/* Postgres Headers */
#include "postgres.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "storage/ipc.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/faultinjector.h"
#include "utils/memutils.h"
#include "utils/guc.h"
/* PLContainer Headers */
#include "common/comm_channel.h"
#include "common/messages/messages.h"
#include "containers.h"
#include "message_fns.h"
#include "plcontainer.h"
#include "plc_configuration.h"
#include "plc_typeio.h"
#include "sqlhandler.h"
#include "subtransaction_handler.h"

#ifdef PG_MODULE_MAGIC

PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(plcontainer_call_handler);

static Datum plcontainer_call_hook(PG_FUNCTION_ARGS);

static plcProcResult *plcontainer_get_result(FunctionCallInfo fcinfo,
                                             plcProcInfo *pinfo);

static Datum plcontainer_process_result(FunctionCallInfo fcinfo,
                                        plcProcInfo *pinfo,
                                        plcProcResult *presult);

static void plcontainer_process_exception(plcMsgError *msg);

static void plcontainer_process_sql(plcMsgSQL *msg, plcConn *conn, plcProcInfo *pinfo);

static void plcontainer_process_log(plcMsgLog *log);

static volatile bool DeleteBackendsWhenError;

/* this is saved and restored by plcontainer_call_handler */
MemoryContext pl_container_caller_context = NULL;

void _PG_init(void);

static void
plcontainer_cleanup(pg_attribute_unused() int code, pg_attribute_unused() Datum arg) {
	delete_containers();
}

/*
 * _PG_init() - library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void) {
	/* Be sure we do initialization only once (should be redundant now) */
	static bool inited = false;
	if (inited)
		return;

	on_proc_exit(plcontainer_cleanup, 0);
	explicit_subtransactions = NIL;
	inited = true;
}

Datum plcontainer_call_handler(PG_FUNCTION_ARGS) {
	Datum datumreturn = (Datum) 0;
	int ret;

	/* TODO: handle trigger requests as well */
	if (CALLED_AS_TRIGGER(fcinfo)) {
		plc_elog(ERROR, "PL/Container does not support triggers");
		return datumreturn;
	}

	/* pl_container_caller_context refer to the CurrentMemoryContext(e.g. ExprContext)
	 * since SPI_connect() will switch memory context to SPI_PROC, we need
	 * to switch back to the pl_container_caller_context at plcontainer_get_result*/
	pl_container_caller_context = CurrentMemoryContext;

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		plc_elog(ERROR, "[plcontainer] SPI connect error: %d (%s)", ret,
		     SPI_result_code_string(ret));

	/* We need to cover this in try-catch block to catch the even of user
	 * requesting the query termination. In this case we should forcefully
	 * kill the container and reset its information
	 */
	PG_TRY();
	{
		datumreturn = plcontainer_call_hook(fcinfo);
	}
	PG_CATCH();
	{
		/* If the reason is Cancel or Termination or Backend error. */
		if (InterruptPending || QueryCancelPending || QueryFinishPending ||
		    DeleteBackendsWhenError) {
			plc_elog(DEBUG1, "Terminating containers due to user request reason("
				"Flags for debugging: %d %d %d %d", InterruptPending,
			     QueryCancelPending, QueryFinishPending, DeleteBackendsWhenError);
			delete_containers();
			DeleteBackendsWhenError = false;
		}

		PG_RE_THROW();
	}
	PG_END_TRY();

	/**
	 *  TODO: SPI_finish() will switch back the memory context. Upstream code place it at earlier
	 *  part of code, we'd better find the right place for it in plcontainer.
	 */
	ret = SPI_finish();
	if (ret != SPI_OK_FINISH)
		plc_elog(ERROR, "[plcontainer] SPI finish error: %d (%s)", ret,
		     SPI_result_code_string(ret));

	return datumreturn;
}

static Datum plcontainer_call_hook(PG_FUNCTION_ARGS) {
	Datum result = (Datum) 0;
	plcProcInfo *pinfo;
	bool bFirstTimeCall = true;
	FuncCallContext *volatile funcctx = NULL;
	MemoryContext oldcontext = NULL;
	plcProcResult *presult = NULL;

	/* By default we return NULL */
	fcinfo->isnull = true;

	/* Get procedure info from cache or compose it based on catalog */
	pinfo = get_proc_info(fcinfo);

	/* If we have a set-retuning function */
	if (fcinfo->flinfo->fn_retset) {
		/* First Call setup */
		if (SRF_IS_FIRSTCALL()) {
			funcctx = SRF_FIRSTCALL_INIT();
		} else {
			bFirstTimeCall = false;
		}

		/* Every call setup */
		funcctx = SRF_PERCALL_SETUP();
		Assert(funcctx != NULL);

		/* SRF initializes special context shared between function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	} else {
		oldcontext = MemoryContextSwitchTo(pl_container_caller_context);
	}

	/* First time call for SRF or just a call of scalar function */
	if (bFirstTimeCall) {
		presult = plcontainer_get_result(fcinfo, pinfo);
		if (fcinfo->flinfo->fn_retset) {
			funcctx->user_fctx = (void *) presult;
		}
	} else {
		presult = (plcProcResult *) funcctx->user_fctx;
	}

	/* If we processed all the rows or the function returned 0 rows we can return immediately */
	if (presult->resrow >= presult->resmsg->rows) {
		free_result(presult->resmsg, false);
		pfree(presult);
		MemoryContextSwitchTo(oldcontext);
		SRF_RETURN_DONE(funcctx);
	}

	/* Process the result message from client */
	result = plcontainer_process_result(fcinfo, pinfo, presult);

	presult->resrow += 1;
	MemoryContextSwitchTo(oldcontext);

	if (fcinfo->flinfo->fn_retset) {
		SRF_RETURN_NEXT(funcctx, result);
	} else {
		free_result(presult->resmsg, false);
		pfree(presult);
	}
	SIMPLE_FAULT_NAME_INJECTOR("plcontainer_before_udf_finish");

	return result;
}

static plcProcResult *plcontainer_get_result(FunctionCallInfo fcinfo,
                                             plcProcInfo *pinfo) {
	char *runtime_id;
	plcConn *conn;
	int message_type;
	plcMsgCallreq *req = NULL;
	plcProcResult *result;
	int volatile save_subxact_level = list_length(explicit_subtransactions);

	PG_TRY();
	{
		result = NULL;
		req = plcontainer_create_call(fcinfo, pinfo);
		runtime_id = parse_container_meta(req->proc.src);
		conn = get_container_conn(runtime_id);
		if (conn == NULL) {
			runtimeConfEntry *runtime_conf_entry = NULL;
			runtime_conf_entry = plc_get_runtime_configuration(runtime_id);
			if (runtime_conf_entry == NULL) {
				plc_elog(ERROR, "Runtime '%s' is not defined in configuration "
							"and cannot be used", runtime_id);
			} else {
				/* TODO: We could only remove this backend when error occurs. */
				DeleteBackendsWhenError = true;
				conn = start_backend(runtime_conf_entry);
				DeleteBackendsWhenError = false;
			}
		}
		pfree(runtime_id);

		DeleteBackendsWhenError = true;
		if (conn != NULL) {
			int res;

			res = plcontainer_channel_send(conn, (plcMessage *) req);
			SIMPLE_FAULT_NAME_INJECTOR("plcontainer_after_send_request");

			if (res < 0) {
				plc_elog(ERROR, "Error sending data to the client: %d. "
							"Maybe retry later.", res);
				return NULL;
			}
			free_callreq(req, true, true);

			while (1) {
				plcMessage *answer;

				res = plcontainer_channel_receive(conn, &answer, MT_ALL_BITS);
				SIMPLE_FAULT_NAME_INJECTOR("plcontainer_after_recv_request");
				if (res < 0) {
					plc_elog(ERROR, "Error receiving data from the client: %d. "
								"Maybe retry later.", res);
					break;
				}

				message_type = answer->msgtype;
				switch (message_type) {
					case MT_RESULT:
						result = (plcProcResult *) pmalloc(sizeof(plcProcResult));
						result->resmsg = (plcMsgResult *) answer;
						result->resrow = 0;
						break;
					case MT_EXCEPTION:
						/* For exception, no need to delete containers. */
						DeleteBackendsWhenError = false;
						plcontainer_process_exception((plcMsgError *) answer);
						break;
					case MT_SQL:
						plcontainer_process_sql((plcMsgSQL *) answer, conn, pinfo);
						break;
					case MT_LOG:
						plcontainer_process_log((plcMsgLog *) answer);
						break;
					case MT_SUBTRANSACTION:
						plcontainer_process_subtransaction(
								(plcMsgSubtransaction *) answer, conn);
						break;
					default:
						plc_elog(ERROR, "Received unhandled message with type id %d "
								"from client", message_type);
						break;
				}

				if (message_type != MT_SQL && message_type != MT_LOG
				    && message_type != MT_SUBTRANSACTION)
					break;
			}
		} else {
			/* If conn == NULL, it should have longjump-ed earlier. */
			plc_elog(ERROR, "Could not create or connect to container.");
		}
		/*
		 * Since plpy will only let you close subtransactions that you
		 * started, you cannot *unnest* subtransactions, only *nest* them
		 * without closing.
		 */
		Assert(list_length(explicit_subtransactions) >= save_subxact_level);
	}
	PG_CATCH();
	{
		plcontainer_abort_open_subtransactions(save_subxact_level);
		PG_RE_THROW();
	}
	PG_END_TRY();

	plcontainer_abort_open_subtransactions(save_subxact_level);

	DeleteBackendsWhenError = false;
	return result;
}

/*
 * Processing client results message
 */
static Datum plcontainer_process_result(FunctionCallInfo fcinfo,
                                        plcProcInfo *pinfo,
                                        plcProcResult *presult) {
	Datum result = (Datum) 0;
	plcMsgResult *resmsg = presult->resmsg;

	if (resmsg->cols > 1) {
		plc_elog(ERROR, "Functions returning multiple columns are not supported yet");
		return result;
	}

	if (resmsg->rows == 0) {
		return result;
	}

	if (presult->resrow >= resmsg->rows) {
		ereport(ERROR,
		        (errcode(ERRCODE_CARDINALITY_VIOLATION),
			        errmsg("Trying to access result row %d of the %d-rows result set",
			               presult->resrow, resmsg->rows)));
		return result;
	}

	if (resmsg->data[presult->resrow][0].isnull == 0) {
		fcinfo->isnull = false;
		result = pinfo->rettype.infunc(resmsg->data[presult->resrow][0].value, &pinfo->rettype);
	}

	return result;
}

/*
 * Processing client log message
 */
static void plcontainer_process_log(plcMsgLog *log) {
	ereport(log->level,
	        (errcode(ERRCODE_NO_DATA),
		        errmsg("%s", log->message)));
	if (log->message != NULL)
		pfree(log->message);
}

/*
 * Processing client SQL query message
 */
static void plcontainer_process_sql(plcMsgSQL *msg, plcConn *conn, plcProcInfo *pinfo) {
	plcMessage *res;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	int retval;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	res = handle_sql_message(msg, conn, pinfo);
	if (res != NULL) {
		retval = plcontainer_channel_send(conn, res);
		if (retval < 0) {
			plc_elog(ERROR, "Error sending data to the client: %d. "
				"Maybe retry later.", retval);
			return;
		}
		switch (res->msgtype) {
			case MT_RESULT:
				free_result((plcMsgResult *) res, true);
				break;
			case MT_CALLREQ:
				free_callreq((plcMsgCallreq *) res, true, true);
				break;
			case MT_RAW:
				free_rawmsg((plcMsgRaw *) res);
				break;
			default:
				ereport(ERROR,
				        (errcode(ERRCODE_RAISE_EXCEPTION),
					        errmsg("Returning message type '%c' from SPI call is not implemented", res->msgtype)));
		}
	}

	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	/*
	 * AtEOSubXact_SPI() should not have popped any SPI context, but just
	 * in case it did, make sure we remain connected.
	 */
	SPI_restore_connection();
}

/*
 * Processing client exception message
 */
static void plcontainer_process_exception(plcMsgError *msg) {
	if (msg->stacktrace != NULL) {
		ereport(ERROR,
		        (errcode(ERRCODE_RAISE_EXCEPTION),
			        errmsg("PL/Container client exception occurred: \n %s \n %s", msg->message, msg->stacktrace)));
	} else {
		ereport(ERROR,
		        (errcode(ERRCODE_RAISE_EXCEPTION),
			        errmsg("PL/Container client exception occurred: \n %s", msg->message)));
	}
	free_error(msg);
}
