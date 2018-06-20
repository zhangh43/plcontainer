/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016, Pivotal.
 *
 *------------------------------------------------------------------------------
 */


/* Greenplum headers */
#include "postgres.h"
#include "fmgr.h"
#include "access/transam.h"
#include "access/tupmacs.h"

#ifdef PLC_PG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "executor/spi.h"
#ifdef PLC_PG
#pragma GCC diagnostic pop
#endif

#include "parser/parse_type.h"
#include "utils/fmgroids.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"

#include "plc_typeio.h"
#include "common/comm_utils.h"
#include "message_fns.h"

#ifdef PLC_PG
  #include "catalog/pg_type.h"
  #include "access/htup_details.h"
#endif

static void fill_type_info_inner(FunctionCallInfo fcinfo, Oid typeOid, plcTypeInfo *type,
                                 bool isArrayElement, bool isUDTElement);

static char *plc_datum_as_int1(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_int2(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_int4(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_int8(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_float4(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_float8(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_float8_numeric(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_text(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_bytea(PLyDatumToOb *arg, Datum input);

static char *plc_datum_as_array(PLyDatumToOb *arg, Datum input);

static void plc_backend_array_free(plcIterator *iter);

static rawdata *plc_backend_array_next(plcIterator *self);


static Datum plc_datum_from_int1(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_int2(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_int4(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_int8(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_float4(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_float8(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_float8_numeric(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_text(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_text_ptr(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_bytea(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_bytea_ptr(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_array(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

static Datum plc_datum_from_composite(struct PLyObToDatum *arg, int32 typmod, char *input, bool inarray);

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of plpython functions/queries is allocated permanently
 * (mostly via malloc()) and never released until backend exit.  Subsidiary
 * data structures such as fmgr info records therefore must live forever
 * as well.  A better implementation would store all this stuff in a per-
 * function memory context that could be reclaimed at need.  In the meantime,
 * fmgr_info_cxt must be called specifying TopMemoryContext so that whatever
 * it might allocate, and whatever the eventual function might allocate using
 * fn_mcxt, will live forever too.
 */
static void
perm_fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, TopMemoryContext);
}


static void
fill_type_info_inner(FunctionCallInfo fcinfo, Oid typeOid, plcTypeInfo *type, bool isArrayElement, bool isUDTElement) {
	HeapTuple typeTup;
	Form_pg_type typeStruct;
	char dummy_delim;
	Oid typioparam;

	typeTup = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeOid), 0, 0, 0);
	if (!HeapTupleIsValid(typeTup))
		plc_elog(ERROR, "cache lookup failed for type %u", typeOid);

	typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	ReleaseSysCache(typeTup);

	type->typeOid = typeOid;
	type->output = typeStruct->typoutput;
	type->input = typeStruct->typinput;
	get_type_io_data(typeOid, IOFunc_input,
	                 &type->typlen, &type->typbyval, &type->typalign,
	                 &dummy_delim,
	                 &typioparam, &type->input);
	type->typmod = typeStruct->typtypmod;
	type->nSubTypes = 0;
	type->subTypes = NULL;
	type->typelem = typeStruct->typelem;

	type->is_rowtype = false;
	type->is_record = false;
	type->attisdropped = false;
	type->typ_relid = InvalidOid;
	type->typrel_xmin = InvalidTransactionId;
	ItemPointerSetInvalid(&type->typrel_tid);
	type->typeName = NULL;

	switch (typeOid) {
		case BOOLOID:
			type->type = PLC_DATA_INT1;
			type->outfunc = plc_datum_as_int1;
			type->infunc = plc_datum_from_int1;
			break;
		case INT2OID:
			type->type = PLC_DATA_INT2;
			type->outfunc = plc_datum_as_int2;
			type->infunc = plc_datum_from_int2;
			break;
		case INT4OID:
			type->type = PLC_DATA_INT4;
			type->outfunc = plc_datum_as_int4;
			type->infunc = plc_datum_from_int4;
			break;
		case INT8OID:
			type->type = PLC_DATA_INT8;
			type->outfunc = plc_datum_as_int8;
			type->infunc = plc_datum_from_int8;
			break;
		case FLOAT4OID:
			type->type = PLC_DATA_FLOAT4;
			type->outfunc = plc_datum_as_float4;
			type->infunc = plc_datum_from_float4;
			break;
		case FLOAT8OID:
			type->type = PLC_DATA_FLOAT8;
			type->outfunc = plc_datum_as_float8;
			type->infunc = plc_datum_from_float8;
			break;
		case NUMERICOID:
			type->type = PLC_DATA_FLOAT8;
			type->outfunc = plc_datum_as_float8_numeric;
			type->infunc = plc_datum_from_float8_numeric;
			break;
		case BYTEAOID:
			type->type = PLC_DATA_BYTEA;
			type->outfunc = plc_datum_as_bytea;
			if (!isArrayElement) {
				type->infunc = plc_datum_from_bytea;
			} else {
				type->infunc = plc_datum_from_bytea_ptr;
			}
			break;
			/* All the other types are passed through in-out functions to translate
			 * them to text before sending and after receiving */
		default:
			type->type = PLC_DATA_TEXT;
			type->outfunc = plc_datum_as_text;
			if (!isArrayElement) {
				type->infunc = plc_datum_from_text;
			} else {
				type->infunc = plc_datum_from_text_ptr;
			}
			break;
	}

	/* Processing arrays here */
	if (!isArrayElement && typeStruct->typelem != 0 && typeStruct->typlen == -1 && typeStruct->typoutput == F_ARRAY_OUT) {
		type->type = PLC_DATA_ARRAY;
		type->outfunc = plc_datum_as_array;
		type->infunc = plc_datum_from_array;
		type->nSubTypes = 1;
		type->subTypes = (plcTypeInfo *) PLy_malloc(sizeof(plcTypeInfo));
		fill_type_info_inner(fcinfo, typeStruct->typelem, &type->subTypes[0], true, isUDTElement);
	}
}

void fill_type_info(FunctionCallInfo fcinfo, Oid typeOid, plcTypeInfo *type) {
	fill_type_info_inner(fcinfo, typeOid, type, false, false);
}

void copy_type_info(plcType *type, plcTypeInfo *ptype) {
	type->type = ptype->type;
	type->nSubTypes = 0;
	if (ptype->typeName != NULL) {
		type->typeName = pstrdup(ptype->typeName);
	} else {
		type->typeName = NULL;
	}
	if (ptype->nSubTypes > 0) {
		int i, j;

		for (i = 0; i < ptype->nSubTypes; i++) {
			if (!ptype->subTypes[i].attisdropped) {
				type->nSubTypes += 1;
			}
		}

		type->subTypes = (plcType *) pmalloc(type->nSubTypes * sizeof(plcType));
		for (i = 0, j = 0; i < ptype->nSubTypes; i++) {
			if (!ptype->subTypes[i].attisdropped) {
				copy_type_info(&type->subTypes[j], &ptype->subTypes[i]);
				j += 1;
			}
		}
	} else {
		type->subTypes = NULL;
	}
}

void free_type_info(plcTypeInfo *type) {
	int i = 0;

	if (type->typeName != NULL) {
		pfree(type->typeName);
	}

	for (i = 0; i < type->nSubTypes; i++) {
		free_type_info(&type->subTypes[i]);
	}

	if (type->nSubTypes > 0) {
		pfree(type->subTypes);
	}
}

static char *plc_datum_as_int1(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(1);
	*((char *) out) = DatumGetBool(d);
	return out;
}

static char *plc_datum_as_int2(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(2);
	*((int16 *) out) = DatumGetInt16(d);
	return out;
}

static char *plc_datum_as_int4(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(4);
	*((int32 *) out) = DatumGetInt32(d);
	return out;
}

static char *plc_datum_as_int8(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(8);
	*((int64 *) out) = DatumGetInt64(d);
	return out;
}

static char *plc_datum_as_float4(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(4);
	*((float4 *) out) = DatumGetFloat4(d);
	return out;
}

static char *plc_datum_as_float8(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(8);
	*((float8 *) out) = DatumGetFloat8(d);
	return out;
}

static char *plc_datum_as_float8_numeric(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	char *out = (char *) pmalloc(8);
	/* Numeric is casted to float8 which causes precision lost */
	Datum fdatum = DirectFunctionCall1(numeric_float8, d);
	*((float8 *) out) = DatumGetFloat8(fdatum);
	return out;
}

static char *plc_datum_as_text(PLyDatumToOb *arg, Datum d) {
	return OutputFunctionCall(&arg->typfunc, d);
}

static char *plc_datum_as_bytea(pg_attribute_unused() PLyDatumToOb *arg, Datum d) {
	text *txt = DatumGetByteaP(d);
	int len = VARSIZE(txt) - VARHDRSZ;
	char *out = (char *) pmalloc(len + 4);
	*((int *) out) = len;
	memcpy(out + 4, VARDATA(txt), len);
	return out;
}

static char *plc_datum_as_array(PLyDatumToOb *arg, Datum d) {
	ArrayType *array = DatumGetArrayTypeP(d);
	plcIterator *iter;
	plcArrayMeta *meta;
	plcPgArrayPosition *pos;
	int i;

	iter = (plcIterator *) palloc(sizeof(plcIterator));
	meta = (plcArrayMeta *) palloc(sizeof(plcArrayMeta));
	pos = (plcPgArrayPosition *) palloc(sizeof(plcPgArrayPosition));
	iter->meta = meta;
	iter->position = (char *) pos;
/*
	meta->type = type->subTypes[0].type;
	meta->ndims = ARR_NDIM(array);
	meta->dims = (int *) palloc(meta->ndims * sizeof(int));
	pos->type = type;
	pos->bitmap = ARR_NULLBITMAP(array);
	pos->bitmask = 1;
	meta->size = meta->ndims > 0 ? 1 : 0;
	for (i = 0; i < meta->ndims; i++) {
		meta->dims[i] = ARR_DIMS(array)[i];
		meta->size *= ARR_DIMS(array)[i];
	}	
	iter->data = ARR_DATA_PTR(array);
	iter->next = plc_backend_array_next;
	iter->cleanup = plc_backend_array_free;
*/
	return (char *) iter;
}

static void plc_backend_array_free(plcIterator *iter) {
	plcArrayMeta *meta;
	meta = (plcArrayMeta *) iter->meta;
	if (meta->ndims > 0) {
		pfree(meta->dims);
	}
	pfree(iter->meta);
	pfree(iter->position);
	return;
}

static rawdata *plc_backend_array_next(plcIterator *self) {
	plcTypeInfo *subtyp;
	rawdata *res;
	plcPgArrayPosition *pos;
	Datum itemvalue;

	res = palloc(sizeof(rawdata));
	pos = (plcPgArrayPosition *) self->position;
	subtyp = &pos->type->subTypes[0];

	/* Get source element, checking for NULL */
	if (pos->bitmap && (*(pos->bitmap) & pos->bitmask) == 0) {
		res->isnull = 1;
		res->value = NULL;
	} else {
		res->isnull = 0;
		itemvalue = fetch_att(self->data, subtyp->typbyval, subtyp->typlen);
		res->value = subtyp->outfunc(itemvalue, subtyp);

		self->data = att_addlength_pointer(self->data, subtyp->typlen, self->data);
		self->data = (char *) att_align_nominal(self->data, subtyp->typalign);
	}

	/* advance bitmap pointer if any */
	if (pos->bitmap) {
		pos->bitmask <<= 1;
		if (pos->bitmask == 0x100 /* (1<<8) */) {
			pos->bitmap++;
			pos->bitmask = 1;
		}
	}

	return res;
}

static Datum plc_datum_from_int1(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return BoolGetDatum(*((bool *) input));
}

static Datum plc_datum_from_int2(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return Int16GetDatum(*((int16 *) input));
}

static Datum plc_datum_from_int4(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return Int32GetDatum(*((int32 *) input));
}

static Datum plc_datum_from_int8(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return Int64GetDatum(*((int64 *) input));
}

static Datum plc_datum_from_float4(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return Float4GetDatum(*((float4 *) input));
}

static Datum plc_datum_from_float8(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return Float8GetDatum(*((float8 *) input));
}

static Datum plc_datum_from_float8_numeric(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	Datum fdatum = Float8GetDatum(*((float8 *) input));
	return DirectFunctionCall1(float8_numeric, fdatum);
}

static Datum plc_datum_from_text(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return OidFunctionCall3(type->input,
	                        CStringGetDatum(input),
	                        type->typelem,
	                        type->typmod);
}

static Datum plc_datum_from_text_ptr(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return OidFunctionCall3(type->input,
	                        CStringGetDatum(*((char **) input)),
	                        type->typelem,
	                        type->typmod);
}

static Datum plc_datum_from_bytea(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	int size = *((int *) input);
	bytea *result = palloc(size + VARHDRSZ);

	SET_VARSIZE(result, size + VARHDRSZ);
	memcpy(VARDATA(result), input + 4, size);
	return PointerGetDatum(result);
}

static Datum plc_datum_from_bytea_ptr(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	return plc_datum_from_bytea(*((char **) input), type);
}

static Datum plc_datum_from_array(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	Datum dvalue;
	Datum *elems;
	ArrayType *array = NULL;
	int *lbs = NULL;
	int i;
	plcArray *arr;
	char *ptr;
	int len;
	plcTypeInfo *subType;

	arr = (plcArray *) input;
	subType = &type->subTypes[0];
	lbs = (int *) palloc(arr->meta->ndims * sizeof(int));
	for (i = 0; i < arr->meta->ndims; i++)
		lbs[i] = 1;

	elems = palloc(arr->meta->size * sizeof(Datum));
	ptr = arr->data;
	len = plc_get_type_length(subType->type);
	for (i = 0; i < arr->meta->size; i++) {
		if (arr->nulls[i] == 0) {
			elems[i] = subType->infunc(ptr, subType);
		} else {
			elems[i] = (Datum) 0;
		}
		ptr += len;
	}

	array = construct_md_array(elems,
	                           arr->nulls,
	                           arr->meta->ndims,
	                           arr->meta->dims,
	                           lbs,
	                           subType->typeOid,
	                           subType->typlen,
	                           subType->typbyval,
	                           subType->typalign);

	dvalue = PointerGetDatum(array);

	pfree(lbs);
	pfree(elems);

	return dvalue;
}

static Datum plc_datum_from_composite(pg_attribute_unused() struct PLyObToDatum *arg, pg_attribute_unused() int32 typmod, char *input, pg_attribute_unused() bool inarray) {
	HeapTuple	tuple = NULL;
	Datum		rv;
	PLyTypeInfo info;
	TupleDesc	desc;

	if (typmod != -1)
		elog(ERROR, "received unnamed record type as input");

	/* Create a dummy PLyTypeInfo */
	MemSet(&info, 0, sizeof(PLyTypeInfo));
	PLy_typeinfo_init(&info);
	/* Mark it as needing output routines lookup */
	info.is_rowtype = 2;

	desc = lookup_rowtype_tupdesc(arg->typoid, arg->typmod);

	/*
	 * This will set up the dummy PLyTypeInfo's output conversion routines,
	 * since we left is_rowtype as 2. A future optimisation could be caching
	 * that info instead of looking it up every time a tuple is returned from
	 * the function.
	 */
	tuple = PLyObject_ToTuple(&info, desc, plrv, false);

	PLy_typeinfo_dealloc(&info);

	if (tuple != NULL)
		rv = HeapTupleGetDatum(tuple);
	else
		rv = (Datum) 0;

	return rv;
}

static Datum plc_datum_from_udt_ptr(char *input, plcTypeInfo *type) {
	return plc_datum_from_udt(*((char **) input), type);
}

plcDatatype plc_get_datatype_from_oid(Oid oid) {
	plcDatatype dt;

	switch (oid) {
		case BOOLOID:
			dt = PLC_DATA_INT1;
			break;
		case INT2OID:
			dt = PLC_DATA_INT2;
			break;
		case INT4OID:
			dt = PLC_DATA_INT4;
			break;
		case INT8OID:
			dt = PLC_DATA_INT8;
			break;
		case FLOAT4OID:
			dt = PLC_DATA_FLOAT4;
			break;
		case FLOAT8OID:
			dt = PLC_DATA_FLOAT8;
			break;
		case NUMERICOID:
			dt = PLC_DATA_FLOAT8;
			break;
		case BYTEAOID:
			dt = PLC_DATA_BYTEA;
			break;
		default:
			dt = PLC_DATA_TEXT;
			break;
	}

	return dt;
}

void
plc_input_datum_func(plcTypeInfo *arg, Oid typeOid, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for Tuple");
	arg->is_rowtype = 0;
	PLy_input_datum_func2(&(arg->in.d), typeOid, typeTup);
}

void
plc_input_datum_func2(plcDatumToOb *arg, Oid typeOid, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	Oid			element_type = get_element_type(typeOid);

	/* Get the type's conversion information */
	perm_fmgr_info(typeStruct->typoutput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typmod = -1;
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;
	arg->typlen = typeStruct->typlen;
	arg->typalign = typeStruct->typalign;

	/* Determine which kind of Python object we will convert to */
	switch (getBaseType(element_type ? element_type : typeOid))
	{
		case BOOLOID:
			arg->type = PLC_DATA_INT1;
			arg->func = plc_datum_as_int1;
			break;
		case FLOAT4OID:
			arg->type = PLC_DATA_FLOAT4;
			arg->func = plc_datum_as_float4;
			break;
		case FLOAT8OID:
			arg->type = PLC_DATA_FLOAT8;
			arg->func = plc_datum_as_float8;
			break;
		case NUMERICOID:
			arg->type = PLC_DATA_FLOAT8;
			arg->func = plc_datum_as_float8_numeric;
			break;
		case INT2OID:
			arg->type = PLC_DATA_INT2;
			arg->func = plc_datum_as_int2;
			break;
		case INT4OID:
			arg->type = PLC_DATA_INT4;
			arg->func = plc_datum_as_int4;
			break;
		case INT8OID:
			arg->type = PLC_DATA_INT8;
			arg->func = plc_datum_as_int8;
			break;
		case BYTEAOID:
			arg->type = PLC_DATA_BYTEA;
			arg->func = plc_datum_as_bytea;
			break;
		default:
			arg->type = PLC_DATA_TEXT;
			arg->func = plc_datum_as_text;
			break;
	}

	if (element_type)
	{
		char		dummy_delim;
		Oid			funcid;

		arg->elm = PLy_malloc0(sizeof(*arg->elm));
		arg->elm->func = arg->func;
		arg->func = plcList_FromArray;
		arg->elm->typoid = element_type;
		arg->elm->typmod = -1;
		get_type_io_data(element_type, IOFunc_output,
						 &arg->elm->typlen, &arg->elm->typbyval, &arg->elm->typalign, &dummy_delim,
						 &arg->elm->typioparam, &funcid);
		perm_fmgr_info(funcid, &arg->elm->typfunc);
	}
}

char *
plcList_FromArray(plcDatumToOb *arg, Datum d)
{
	ArrayType  *array = DatumGetArrayTypeP(d);
	plcDatumToOb *elm = arg->elm;
	int			ndim;
	int		   *dims;
	char	   *dataptr;
	bits8	   *bitmap;
	int			bitmask;

	if (ARR_NDIM(array) == 0)
		return PyList_New(0);

	/* Array dimensions and left bounds */
	ndim = ARR_NDIM(array);
	dims = ARR_DIMS(array);
	Assert(ndim < MAXDIM);

	/*
	 * We iterate the SQL array in the physical order it's stored in the
	 * datum. For example, for a 3-dimensional array the order of iteration would
	 * be the following: [0,0,0] elements through [0,0,k], then [0,1,0] through
	 * [0,1,k] till [0,m,k], then [1,0,0] through [1,0,k] till [1,m,k], and so on.
	 *
	 * In Python, there are no multi-dimensional lists as such, but they are
	 * represented as a list of lists. So a 3-d array of [n,m,k] elements is a
	 * list of n m-element arrays, each element of which is k-element array.
	 * PLyList_FromArray_recurse() builds the Python list for a single
	 * dimension, and recurses for the next inner dimension.
	 */
	dataptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	return plcList_FromArray_recurse(elm, dims, ndim, 0,
									 &dataptr, &bitmap, &bitmask);
}

char *
plcList_FromArray_recurse(plcDatumToOb *elm, int *dims, int ndim, int dim,
						  char **dataptr_p, bits8 **bitmap_p, int *bitmask_p)
{
	int			i;
	PyObject   *list;

	list = PyList_New(dims[dim]);

	if (dim < ndim - 1)
	{
		/* Outer dimension. Recurse for each inner slice. */
		for (i = 0; i < dims[dim]; i++)
		{
			PyObject   *sublist;

			sublist = PLyList_FromArray_recurse(elm, dims, ndim, dim + 1,
											 dataptr_p, bitmap_p, bitmask_p);
			PyList_SET_ITEM(list, i, sublist);
		}
	}
	else
	{
		/*
		 * Innermost dimension. Fill the list with the values from the array
		 * for this slice.
		 */
		char	   *dataptr = *dataptr_p;
		bits8	   *bitmap = *bitmap_p;
		int			bitmask = *bitmask_p;

		for (i = 0; i < dims[dim]; i++)
		{
			/* checking for NULL */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				Py_INCREF(Py_None);
				PyList_SET_ITEM(list, i, Py_None);
			}
			else
			{
				Datum		itemvalue;

				itemvalue = fetch_att(dataptr, elm->typbyval, elm->typlen);
				PyList_SET_ITEM(list, i, elm->func(elm, itemvalue));
				dataptr = att_addlength_pointer(dataptr, elm->typlen, dataptr);
				dataptr = (char *) att_align_nominal(dataptr, elm->typalign);
			}

			/* advance bitmap pointer if any */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100 /* (1<<8) */ )
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		*dataptr_p = dataptr;
		*bitmap_p = bitmap;
		*bitmask_p = bitmask;
	}

	return list;
}



void
plc_output_datum_func(plcTypeInfo *arg, HeapTuple typeTup)
{
	if (arg->is_rowtype > 0)
		elog(ERROR, "PLyTypeInfo struct is initialized for a Tuple");
	arg->is_rowtype = 0;
	plc_output_datum_func2(&(arg->out.d), typeTup);
}


void
plc_output_datum_func2(plcObToDatum *arg, HeapTuple typeTup)
{
	Form_pg_type typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
	Oid			element_type;

	perm_fmgr_info(typeStruct->typinput, &arg->typfunc);
	arg->typoid = HeapTupleGetOid(typeTup);
	arg->typmod = -1;
	arg->typioparam = getTypeIOParam(typeTup);
	arg->typbyval = typeStruct->typbyval;

	element_type = get_element_type(arg->typoid);

	/*
	 * Select a conversion function to convert Python objects to PostgreSQL
	 * datums.	Most data types can go through the generic function.
	 */
	/* Composite types need their own input routine, though */
	//if (typeStruct->typtype == TYPTYPE_COMPOSITE)
	//{
	//	arg->func = plc_datum_from_composite;
	//}
	//else
		switch (getBaseType(element_type ? element_type : arg->typoid))
		{
			case BOOLOID:
				arg->func = plc_datum_from_int1;
				break;
			case BYTEAOID:
				arg->func = plc_datum_from_bytea;
				break;
			default:
				arg->func = plc_datum_from_text;
				break;
		}


	/* Composite types need their own input routine, though */
	//if (typeStruct->typtype == TYPTYPE_COMPOSITE)
	//{
	//	arg->func = PLyObject_ToComposite;
	//}

	/*if (element_type)
	{
		char		dummy_delim;
		Oid			funcid;

		if (type_is_rowtype(element_type))
			arg->func = PLyObject_ToComposite;

		arg->elm = PLy_malloc0(sizeof(*arg->elm));
		arg->elm->func = arg->func;
		arg->func = PLySequence_ToArray;

		arg->elm->typoid = element_type;
		arg->elm->typmod = -1;
		get_type_io_data(element_type, IOFunc_input,
						 &arg->elm->typlen, &arg->elm->typbyval, &arg->elm->typalign, &dummy_delim,
						 &arg->elm->typioparam, &funcid);
		perm_fmgr_info(funcid, &arg->elm->typfunc);
	}*/
}



