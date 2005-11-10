
/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */ 
#ifndef VAL_PRINT_H
#define VAL_PRINT_H

#include "val_parse.h"

void dump_rrset(struct rrset_rec *rrset);
void dump_dinfo(struct domain_info *dinfo);
void val_print_base64(unsigned char * message, int message_len);
void val_print_rrsig_rdata (const char *prefix, val_rrsig_rdata_t *rdata);
void val_print_dnskey_rdata (const char *prefix, val_dnskey_rdata_t *rdata);
char *print_ns_string(struct name_server **server);
void val_print_assertion_chain(u_char *name_n, u_int16_t class_h, u_int16_t type_h, 
				struct query_chain *queries, struct val_result *results);

#endif
