/*
Copyright 1994 The PL-J Project. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE PL-J PROJECT ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE PL-J PROJECT OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
   OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
   OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those of the authors and should not be
interpreted as representing official policies, either expressed or implied, of the PL-J Project.
*/


/**
 * file name:        plj-callmkr.c
 * description:        PL/pgJ call message creator routine. This file
 *                    was renamed from plpgj_call_maker.c
 *                    It is replaceable with pljava-way of declaring java
 *                    method calls. (read the readme!)
 * author:        Laszlo Hornyak
 */

/*
 * Portions Copyright © 2016-Present Pivotal Software, Inc.
 */

/* Greenplum headers */
#include "postgres.h"

#ifdef PLC_PG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "executor/spi.h"
#ifdef PLC_PG
#pragma GCC diagnostic pop
#endif

#include "access/transam.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"

/* message and function definitions */
#include "common/comm_utils.h"
#include "common/messages/messages.h"
#include "message_fns.h"
#include "function_cache.h"
#include "plc_typeio.h"

#ifdef PLC_PG
  #include "catalog/pg_type.h"
  #include "access/htup_details.h"
#endif

static bool plc_procedure_valid(plcProcedure *proc, HeapTuple procTup);

static bool plc_type_valid(plcTypeInfo *type);

static void fill_callreq_arguments(FunctionCallInfo fcinfo, plcProcedure *proc, plcMsgCallreq *req);

/*
 * Create a new PLyProcedure structure
 */
static plcProcedure * plcontainer_procedure_create(FunctionCallInfo fcinfo, HeapTuple procTup, Oid fn_oid);

plcProcedure *plcontainer_procedure_get(FunctionCallInfo fcinfo) {
	Oid fn_oid;
	HeapTuple procTup;
	plcProcedure * volatile proc = NULL;

	fn_oid = fcinfo->flinfo->fn_oid;
	procTup = SearchSysCache1(PROCOID, fn_oid);
	if (!HeapTupleIsValid(procTup)) {
		plc_elog(ERROR, "cannot find proc with oid %u", fn_oid);
	}

	/* TODO: plpython use hash bucket to store plcProcedure
	 * See PLy_procedure_get() for details.
	 */
	proc = function_cache_get(fn_oid);

	/*
	 * All the catalog operations are done only if the cached function
	 * information has changed in the catalog
	 */
	if (!plc_procedure_valid(proc, procTup)) {
		proc = plcontainer_procedure_create(fcinfo, procTup, fn_oid);
	} else {
		proc->hasChanged = 0;
	}
	ReleaseSysCache(procTup);
	return proc;
}

/*
 * Create a new PLyProcedure structure
 */
static plcProcedure *
plcontainer_procedure_create(FunctionCallInfo fcinfo, HeapTuple procTup, Oid fn_oid) {

	plcProcedure * volatile proc = NULL;
	int lenOfArgnames;
	Datum *argnames = NULL;
	bool *argnulls = NULL;
	Datum argnamesArray;
	Datum srcdatum, namedatum;
	HeapTuple textHeapTup = NULL;
	Form_pg_type typeTup;
	char procName[NAMEDATALEN + 256];
	Form_pg_proc procStruct;
	bool isnull;
	int rv;

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);
	rv = snprintf(procName, sizeof(procName), "__plpython_procedure_%s_%u",
			NameStr(procStruct->proname), fn_oid);
	if (rv < 0 || (unsigned int) rv >= sizeof(procName))
		elog(ERROR, "procedure name would overrun buffer");

	/*
	 * Here we are using plc_top_alloc as the function structure should be
	 * available across the function handler call
	 *
	 * Note: we free the procedure from within function_put_cache below
	 */
	proc = PLy_malloc(sizeof(plcProcedure));
	if (proc == NULL) {
		plc_elog(FATAL, "Cannot allocate memory for plcProcInfo structure");
	}

	proc->proname = PLy_strdup(NameStr(procStruct->proname));
	proc->pyname = PLy_strdup(procName);
	proc->funcOid = fn_oid;
	proc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	proc->fn_tid = procTup->t_self;
	/* Remember if function is STABLE/IMMUTABLE */
	proc->fn_readonly = (procStruct->provolatile != PROVOLATILE_VOLATILE);

	proc->retset = fcinfo->flinfo->fn_retset;

	proc->hasChanged = 1;
	proc->nargs = 0;
	proc->src = NULL;
	proc->argnames = NULL;

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);//TODO delete
	fill_type_info(fcinfo, procStruct->prorettype, &proc->result);

	HeapTuple rvTypeTup;
	Form_pg_type rvTypeStruct;

	rvTypeTup = SearchSysCache1(TYPEOID,
			ObjectIdGetDatum(procStruct->prorettype));
	if (!HeapTupleIsValid(rvTypeTup))
		elog(ERROR, "cache lookup failed for type %u", procStruct->prorettype);
	rvTypeStruct = (Form_pg_type) GETSTRUCT(rvTypeTup);

	/* Disallow pseudotype result, except for void or record */
	if (rvTypeStruct->typtype == TYPTYPE_PSEUDO) {
		if (procStruct->prorettype == TRIGGEROID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
							"trigger functions can only be called as triggers")));
		else if (procStruct->prorettype != VOIDOID
				&& procStruct->prorettype != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
							"PL/Python functions cannot return type %s",
							format_type_be(procStruct->prorettype))));
	}

	if (rvTypeStruct->typtype == TYPTYPE_COMPOSITE
			|| procStruct->prorettype == RECORDOID) {
		/*
		 * Tuple: set up later, during first call to
		 * PLy_function_handler
		 */
		proc->result.out.d.typoid = procStruct->prorettype;
		proc->result.out.d.typmod = -1;
		proc->result.is_rowtype = 2;
	} else {
		/* do the real work */
		plc_output_datum_func(&proc->result, rvTypeTup);
	}

	ReleaseSysCache(rvTypeTup);

	/*
	 * Now get information required for input conversion of the
	 * procedure's arguments.  Note that we ignore output arguments here
	 * --- since we don't support returning record, and that was already
	 * checked above, there's no need to worry about multiple output
	 * arguments.
	 */
	if (procStruct->pronargs) {
		Oid *types;
		char **names, *modes;
		int i, pos, total;

		// This is required to avoid the cycle from being removed by optimizer
		/*int volatile j;

		proc->args = PLy_malloc(proc->nargs * sizeof(plcTypeInfo));
		for (j = 0; j < proc->nargs; j++) {
			fill_type_info(fcinfo, procStruct->proargtypes.values[j],
					&proc->args[j]);
		}*/

		/* extract argument type info from the pg_proc tuple */
		total = get_func_arg_info(procTup, &types, &names, &modes);

		/* count number of in+inout args into proc->nargs */
		if (modes == NULL)
			proc->nargs = total;
		else {
			/* proc->nargs was initialized to 0 above */
			for (i = 0; i < total; i++) {
				if (modes[i] != PROARGMODE_OUT && modes[i] != PROARGMODE_TABLE)
					(proc->nargs)++;
			}
		}

		proc->argnames = (char **) PLy_malloc0(sizeof(char *) * proc->nargs);

		for (i = pos = 0; i < total; i++) {
			HeapTuple argTypeTup;
			Form_pg_type argTypeStruct;

			if (modes
					&& (modes[i] == PROARGMODE_OUT
							|| modes[i] == PROARGMODE_TABLE))
				continue; /* skip OUT arguments */

			Assert(types[i] == procStruct->proargtypes.values[pos]);

			argTypeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(types[i]));
			if (!HeapTupleIsValid(argTypeTup))
				elog(ERROR, "cache lookup failed for type %u", types[i]);
			argTypeStruct = (Form_pg_type) GETSTRUCT(argTypeTup);

			/* check argument type is OK, set up I/O function info */
			switch (argTypeStruct->typtype) {
			case TYPTYPE_PSEUDO:
				/* Disallow pseudotype argument */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
								"PL/Python functions cannot accept type %s",
								format_type_be(types[i]))));
				break;
			case TYPTYPE_COMPOSITE:
				/* we'll set IO funcs at first call */
				proc->args[pos].is_rowtype = 2;
				break;
			default:
				plc_input_datum_func(&(proc->args[pos]), types[i], argTypeTup);
				break;
			}

			/* get argument name */
			proc->argnames[pos] = names ? PLy_strdup(names[i]) : NULL;

			ReleaseSysCache(argTypeTup);

			pos++;
		}
	} else {
		proc->args = NULL;
		proc->argnames = NULL;
	}

	/* Get the text and name of the function */
	srcdatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		plc_elog(ERROR, "null prosrc");
	proc->src = plc_top_strdup(
			DatumGetCString(DirectFunctionCall1(textout, srcdatum)));
	namedatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_proname,
			&isnull);
	if (isnull)
		plc_elog(ERROR, "null proname");
	proc->name = plc_top_strdup(
			DatumGetCString(DirectFunctionCall1(nameout, namedatum)));

	/* Cache the function for later use */
	function_cache_put(proc);

	return proc;
}

void free_proc_info(plcProcedure *proc) {
	int i;
	for (i = 0; i < proc->nargs; i++) {
		if (proc->argnames[i] != NULL) {
			pfree(proc->argnames[i]);
		}
		free_type_info(&proc->args[i]);
	}
	if (proc->nargs > 0) {
		pfree(proc->argnames);
		pfree(proc->args);
	}
	pfree(proc);
}

plcMsgCallreq *plcontainer_generate_call_request(FunctionCallInfo fcinfo, plcProcedure *proc) {
	plcMsgCallreq *req;

	req = pmalloc(sizeof(plcMsgCallreq));
	req->msgtype = MT_CALLREQ;
	req->proc.name = proc->name;
	req->proc.src = proc->src;
	req->logLevel = log_min_messages;
	req->objectid = proc->funcOid;
	req->hasChanged = proc->hasChanged;
	copy_type_info(&req->retType, &proc->result);

	fill_callreq_arguments(fcinfo, proc, req);

	return req;
}

static bool plc_type_valid(plcTypeInfo *type) {
	bool valid = true;
	int i;

	for (i = 0; i < type->nSubTypes && valid; i++) {
		valid = plc_type_valid(&type->subTypes[i]);
	}

	/* We exclude record from testing here, as it would change only if the function
	 * changes itself, which would be caugth by checking function creation xid */
	if (valid && type->is_rowtype && !type->is_record) {
		HeapTuple relTup;

		Assert(OidIsValid(type->typ_relid));
		Assert(TransactionIdIsValid(type->typrel_xmin));
		Assert(ItemPointerIsValid(&type->typrel_tid));

		// Get the pg_class tuple for the argument type
		relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(type->typ_relid));
		if (!HeapTupleIsValid(relTup))
			plc_elog(ERROR, "PL/Container cache lookup failed for relation %u", type->typ_relid);

		// If commit transaction ID has changed or relation was moved within table
		// our type information is no longer valid
		if (type->typrel_xmin != HeapTupleHeaderGetXmin(relTup->t_data) ||
		    !ItemPointerEquals(&type->typrel_tid, &relTup->t_self)) {
			valid = false;
		}

		ReleaseSysCache(relTup);
	}

	return valid;
}

/*
 * Decide whether a cached PLyProcedure struct is still valid
 */
static bool plc_procedure_valid(plcProcedure *proc, HeapTuple procTup) {
	bool valid = false;

	if (proc != NULL) {
		/* If the pg_proc tuple has changed, it's not valid */
		if (proc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
		    ItemPointerEquals(&proc->fn_tid, &procTup->t_self)) {
			int i;

			valid = true;

			// If there are composite input arguments, they might have changed
			for (i = 0; i < proc->nargs && valid; i++) {
				valid = plc_type_valid(&proc->args[i]);
			}

			// Also check for composite output type
			if (valid) {
				valid = plc_type_valid(&proc->result);
			}
		}
	}
	return valid;
}

static void fill_callreq_arguments(FunctionCallInfo fcinfo, plcProcedure *proc, plcMsgCallreq *req) {
	int i;

	req->nargs = proc->nargs;
	req->retset = proc->retset;
	req->args = pmalloc(sizeof(*req->args) * proc->nargs);

	for (i = 0; i < proc->nargs; i++) {
		req->args[i].name = proc->argnames[i];
		copy_type_info(&req->args[i].type, &proc->args[i]);

		if (fcinfo->argnull[i]) {
			req->args[i].data.isnull = 1;
			req->args[i].data.value = NULL;
		} else {
			req->args[i].data.isnull = 0;
			req->args[i].data.value = proc->args[i].in.d.func(&(proc->args[i].in.d),fcinfo->arg[i]);
		}
	}
}
