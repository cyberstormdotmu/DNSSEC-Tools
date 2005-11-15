
/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */ 
#ifndef VAL_X_QUERY_H
#define VAL_X_QUERY_H

int val_x_query(val_context_t *ctx,
            const char *domain_name,
            const u_int16_t class,
            const u_int16_t type,
            const u_int8_t flags,
            struct response_t *resp,
            int *resp_count);

int val_query ( const char *domain_name, int class, int type,
        unsigned char *answer, int anslen, int flags,
        int *dnssec_status );

#endif /* VAL_X_QUERY_H */
