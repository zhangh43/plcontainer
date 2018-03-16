#ifndef PLC_PYCALL_H
#define PLC_PYCALL_H

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#include <Python.h>

#if PY_MAJOR_VERSION >= 3
/*#define PyString_Check(x) 0
*/
#define plc_Py_SetProgramName(x)                \
		do {                                        \
			wchar_t progname[FILENAME_MAX + 1];     \
			mbstowcs(progname, x, strlen(x) + 1);   \
			Py_SetProgramName(progname);            \
		} while (0);
#else
#define plc_Py_SetProgramName(x) Py_SetProgramName(x)
#endif

#include "common/comm_connectivity.h"
#include "pyconversions.h"

#ifdef __GNUC__
#define UNUSED __attribute__ (( unused ))
#else
#define UNUSED
#endif

// Global connection object
extern plcConn *plcconn_global;

// Global execution termination flag
int plc_is_execution_terminated;
int plc_sending_data;

// Initialization of Python module
int python_init(void);

// Processing of the Greenplum function call
void handle_call(plcMsgCallreq *req, plcConn *conn);

#endif /* PLC_PYCALL_H */
