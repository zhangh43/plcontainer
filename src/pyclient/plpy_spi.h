/*------------------------------------------------------------------------------
 *
 *
 * Copyright (c) 2016, Pivotal.
 *
 *------------------------------------------------------------------------------
 */
#ifndef PLC_PYSPI_H
#define PLC_PYSPI_H

#include <Python.h>

PyTypeObject PLy_PlanType;
PyTypeObject PLy_SubtransactionType;

PyObject *PLy_spi_execute(PyObject *self, PyObject *pyquery);

PyObject *PLy_spi_prepare(PyObject *self, PyObject *args);

PyObject *PLy_subtransaction(PyObject *, PyObject *);

void Ply_spi_exception_init(PyObject *plpy);

void PLy_init_plpy(PyObject *PyMainModule);

#endif /* PLC_PYSPI_H */
