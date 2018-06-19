/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */


#ifndef PLC_SQLHANDLER_H
#define PLC_SQLHANDLER_H

#include "common/messages/messages.h"
#include "message_fns.h"

typedef struct plcPlan {
	Oid *argOids;
	SPIPlanPtr plan;
	int nargs;
} plcPlan;

plcMessage *handle_sql_message(plcMsgSQL *msg, plcConn *conn, plcProcedure *pinfo);

#endif /* PLC_SQLHANDLER_H */
