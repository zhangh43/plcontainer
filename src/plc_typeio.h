/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016, Pivotal.
 *
 *------------------------------------------------------------------------------
 */


#ifndef PLC_TYPEIO_H
#define PLC_TYPEIO_H

#include "postgres.h"
#include "funcapi.h"

#include "common/messages/messages.h"
#include "plcontainer.h"

typedef struct plcTypeInfo plcTypeInfo;

typedef char *(*plcDatumOutput)(Datum, plcTypeInfo *);

typedef Datum (*plcDatumInput)(char *, plcTypeInfo *);


struct PLyDatumToOb;
typedef char *(*PLyDatumToObFunc) (struct PLyDatumToOb *, Datum);

typedef struct plcDatumToOb
{
	PLyDatumToObFunc func; /* plcTypeInput use plcDatumOutput*/
	FmgrInfo	typfunc;		/* The type's output function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	plcDatatype type;
	struct plcDatumToOb *elm;
} plcDatumToOb;

typedef struct plcTupleToOb
{
	plcDatumToOb *atts;
	int			natts;
} plcTupleToOb;

typedef union plcTypeInput
{
	plcDatumToOb d;
	plcTupleToOb r;
} plcTypeInput;


struct PLyObToDatum;
typedef Datum (*PLyObToDatumFunc) (struct PLyObToDatum *, int32 typmod,
											   char *, bool inarray);

typedef struct plcObToDatum
{
	PLyObToDatumFunc func;  /* plcTypeOutput use plcDatumInput*/
	FmgrInfo	typfunc;		/* The type's input function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	plcDatatype type;
	struct plcObToDatum *elm;
} plcObToDatum;

typedef struct plcObToTuple
{
	plcObToDatum *atts;
	int			natts;
} plcObToTuple;

typedef union plcTypeOutput
{
	plcObToDatum d;
	plcObToTuple r;
} plcTypeOutput;



struct plcTypeInfo {

	plcTypeInput in;   /* from Datum/Tuple */
	plcTypeOutput out; /* to Datum/Tuple */
	/*
	 * is_rowtype can be: -1 = not known yet (initial state); 0 = scalar
	 * datatype; 1 = rowtype; 2 = rowtype, but I/O functions not set up yet
	 */
	int is_rowtype;
	/* used to check if the type has been modified */
	Oid typ_relid;
	TransactionId typrel_xmin;
	ItemPointerData typrel_tid;

	/* PL/Container-specific information */
	plcDatatype type;
	int nSubTypes;
	plcTypeInfo *subTypes;

	/* Custom input and output functions used for most common data types that
	 * allow binary data transfer */
	plcDatumOutput outfunc;
	plcDatumInput infunc;

	/* GPDB in- and out- functions to transform custom types to text and back */
	RegProcedure output, input;

	/* Information used for type input/output operations */
	Oid typeOid;
	int32 typmod;
	bool typbyval;
	int16 typlen;
	char typalign;


	Oid typelem;



	/* UDT-specific information */
	bool is_record;
	bool attisdropped;
	char *typeName;
};

typedef struct plcPgArrayPosition {
	plcTypeInfo *type;
	bits8 *bitmap;
	int bitmask;
} plcPgArrayPosition;

void plc_input_datum_func(plcTypeInfo *, Oid, HeapTuple);
void plc_input_datum_func2(plcDatumToOb *, Oid, HeapTuple);

void plc_output_datum_func(plcTypeInfo *arg, HeapTuple typeTup);
void plc_output_datum_func2(plcObToDatum *arg, HeapTuple typeTup);

void plc_output_datum_func(plcTypeInfo *arg, HeapTuple typeTup);

void fill_type_info(FunctionCallInfo fcinfo, Oid typeOid, plcTypeInfo *type);

void copy_type_info(plcType *type, plcTypeInfo *ptype);

void free_type_info(plcTypeInfo *type);

char *fill_type_value(Datum funcArg, plcTypeInfo *argType);

plcDatatype plc_get_datatype_from_oid(Oid oid);

#endif /* PLC_TYPEIO_H */
