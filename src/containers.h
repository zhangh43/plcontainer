/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2016-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */

#ifndef PLC_CONTAINERS_H
#define PLC_CONTAINERS_H

#include <regex.h>

#include "common/comm_connectivity.h"
#include "plc_configuration.h"

#define CONTAINER_CONNECT_TIMEOUT_MS 10000
#define CONTAINER_ID_MAX_LENGTH 128
/* given source code of the function, extract the container name */
char *parse_container_meta(const char *source);

/* return the port of a started container, -1 if the container isn't started */
plcConn *get_container_conn(const char *id);

/* start a new docker container using the given configuration */
plcConn *start_backend(runtimeConfEntry *conf);

/* Function deletes all the containers */
void delete_containers(void);

#endif /* PLC_CONTAINERS_H */
