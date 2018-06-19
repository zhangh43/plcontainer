/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016, Pivotal.
 *
 *------------------------------------------------------------------------------
 */

#ifndef PLC_FUNCTION_CACHE_H
#define PLC_FUNCTION_CACHE_H

#include "message_fns.h"

#define PLC_FUNCTION_CACHE_SIZE 5

void function_cache_up(int index);

plcProcedure *function_cache_get(Oid funcOid);

void function_cache_put(plcProcedure *func);

#endif /* PLC_FUNCTION_CACHE_H */
