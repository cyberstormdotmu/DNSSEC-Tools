/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */
#ifndef VAL_CACHE_H
#define VAL_CACHE_H


int             stow_zone_info(struct rrset_rec *new_info, struct val_query_chain *matched_q);
int             stow_key_info(struct rrset_rec *new_info, struct val_query_chain *matched_q);
int             stow_ds_info(struct rrset_rec *new_info, struct val_query_chain *matched_q);
int             stow_answers(struct rrset_rec *new_info, struct val_query_chain *matched_q);
int             stow_negative_answers(struct rrset_rec *new_info, struct val_query_chain *matched_q);
int             stow_root_info(struct rrset_rec *root_info);
int             get_cached_rrset(struct val_query_chain *matched_q, struct domain_info **response);
int             free_validator_cache(void);
int             get_root_ns(struct name_server **ns);
int             store_ns_for_zone(u_int8_t * zonecut_n,
                                  struct name_server *resp_server);
int             get_nslist_from_cache(val_context_t *ctx,
                                      struct val_query_chain *matched_q,
                                      struct val_query_chain **queries,
                                      struct name_server **ref_ns_list,
                                      u_int8_t **zonecut_n);

#endif
