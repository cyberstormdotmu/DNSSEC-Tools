
/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 */
#include "validator-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef LIBVAL_NSEC3
#include <openssl/sha.h>
#endif

#include <arpa/nameser.h>
#ifdef HAVE_ARPA_NAMESER_COMPAT_H
#include <arpa/nameser_compat.h>
#else
#include "arpa/header.h"
#endif

#include <resolver.h>
#include <validator.h>
#include <resolv.h>
#include "val_resquery.h"
#include "val_support.h"
#include "val_cache.h"
#include "val_verify.h"
#include "val_policy.h"
#include "val_log.h"

/*
 * Identify if the type is present in the bitmap
 * The encoding of the bitmap is a sequence of <block#, len, bitmap> tuples
 */
static int
is_type_set(u_int8_t * field, int field_len, u_int16_t type)
{
    int             block, blen;

    /** The type will be present in the following block */
    int             t_block = type / 256;
    /** within the bitmap, the type will be present in the following byte */
    int             t_bm_offset = type / 8;

    int             cnt = 0;

    /*
     * need at least two bytes 
     */
    while (field_len > cnt + 2) {

        block = field[cnt];
        blen = field[cnt + 1];
        cnt += 2;

        if (block == t_block) {
            /*
             * see if we have space 
             */
            if ((blen >= t_bm_offset) && (field_len >= cnt + blen)) {
                /*
                 * see if the bit is set 
                 */
                if (field[cnt + t_bm_offset] & (1 << (7 - (type % 8))))
                    return 1;
            }
            return 0;
        }
        cnt += blen;
    }
    return 0;
}

#define CLONE_NAME_LEN(oldb, oldlen, newb, newlen) do {\
        if (oldlen) {                                                   \
            newb =	(u_int8_t *) MALLOC (oldlen * sizeof(u_int8_t)); \
            if (newb == NULL)                                           \
                return VAL_OUT_OF_MEMORY;                               \
            memcpy(newb, oldb, oldlen);                                 \
        }                                                               \
        else                                                            \
            newb = NULL;                                                \
        newlen = oldlen;                                                \
    } while (0)


#ifdef LIBVAL_NSEC3
#define CHECK_RANGE(range1, range1len, range2, range2len, hash, hashlen) \
            ((nsec3_order_cmp(range2, range2len, hash, hashlen) != 0) &&\
                ((nsec3_order_cmp(range2, range2len, range1, range1len) > 0)?\
                    ((nsec3_order_cmp(hash, hashlen, range1, range1len) > 0) && \
					(nsec3_order_cmp(hash, hashlen, range2, range2len) < 0)) :\
                    ((nsec3_order_cmp(hash, hashlen, range2, range2len) < 0)||\
                     (nsec3_order_cmp(hash, hashlen, range1, range1len) > 0))))
#endif

/*
 * Create a "result" list whose elements point to assertions and also have their
 * validated result 
 */

void
val_free_result_chain(struct val_result_chain *results)
{
    struct val_result_chain *prev;
    struct val_authentication_chain *trust;

    while (NULL != (prev = results)) {
        results = results->val_rc_next;

        /*
         * free the chain of trust 
         */
        while (NULL != (trust = prev->val_rc_trust)) {

            prev->val_rc_trust = trust->val_ac_trust;

            if (trust->val_ac_rrset != NULL) {
                if (trust->val_ac_rrset->val_msg_header)
                    FREE(trust->val_ac_rrset->val_msg_header);
                if (trust->val_ac_rrset->val_rrset_name_n)
                    FREE(trust->val_ac_rrset->val_rrset_name_n);
                if (trust->val_ac_rrset->val_rrset_data != NULL)
                    res_sq_free_rr_recs(&trust->val_ac_rrset->
                                        val_rrset_data);
                if (trust->val_ac_rrset->val_rrset_sig != NULL)
                    res_sq_free_rr_recs(&trust->val_ac_rrset->
                                        val_rrset_sig);

                FREE(trust->val_ac_rrset);
            }

            FREE(trust);
        }

        FREE(prev);
    }
}


/*
 * Add {domain_name, type, class} to the list of queries currently active
 * for validating a response. 
 *
 * Returns:
 * VAL_NO_ERROR                 Operation succeeded
 * VAL_BAD_ARGUMENT     Bad argument (e.g. NULL ptr)
 * VAL_OUT_OF_MEMORY    Could not allocate enough memory for operation
 */
int
add_to_query_chain(struct val_query_chain **queries, u_char * name_n,
                   const u_int16_t type_h, const u_int16_t class_h)
{
    struct val_query_chain *temp, *prev;

    /*
     * sanity checks 
     */
    if ((NULL == queries) || (NULL == name_n))
        return VAL_BAD_ARGUMENT;

    /*
     * Check if query already exists 
     */
    temp = *queries;
    prev = temp;
    while (temp) {
        if ((namecmp(temp->qc_name_n, name_n) == 0)
            && (temp->qc_type_h == type_h)
            && (temp->qc_class_h == class_h))
            break;
        prev = temp;
        temp = temp->qc_next;
    }

    /*
     * If query already exists, bring it to the front of the list 
     */
    if (temp != NULL) {
        if (prev != temp) {
            prev->qc_next = temp->qc_next;
            temp->qc_next = *queries;
            *queries = temp;
        }
        return VAL_NO_ERROR;
    }

    temp =
        (struct val_query_chain *) MALLOC(sizeof(struct val_query_chain));
    if (temp == NULL)
        return VAL_OUT_OF_MEMORY;

    memcpy(temp->qc_name_n, name_n, wire_name_length(name_n));
    temp->qc_type_h = type_h;
    temp->qc_class_h = class_h;
    temp->qc_state = Q_INIT;
    temp->qc_zonecut_n = NULL;
    temp->qc_as = NULL;
    temp->qc_glue_request = 0;
    temp->qc_ns_list = NULL;
    temp->qc_respondent_server = NULL;
    temp->qc_trans_id = -1;
    temp->qc_referral = NULL;
    temp->qc_next = *queries;
    *queries = temp;

    return VAL_NO_ERROR;
}

/*
 * Free up the query chain.
 */
void
free_query_chain(struct val_query_chain *queries)
{
    if (queries == NULL)
        return;

    if (queries->qc_next)
        free_query_chain(queries->qc_next);

    if (queries->qc_zonecut_n != NULL) {
        FREE(queries->qc_zonecut_n);
    }

    if (queries->qc_referral != NULL) {
        free_referral_members(queries->qc_referral);
        FREE(queries->qc_referral);
    }

    if (queries->qc_ns_list != NULL)
        free_name_servers(&(queries->qc_ns_list));

    if (queries->qc_respondent_server != NULL)
        free_name_server(&(queries->qc_respondent_server));

    FREE(queries);

}

static u_int16_t
is_trusted_zone(val_context_t * ctx, u_int8_t * name_n)
{
    struct zone_se_policy *zse_pol, *zse_cur;
    int             name_len;
    u_int8_t       *p, *q;
    char            name_p[NS_MAXDNAME];

    /*
     * sanity checks 
     */
    if (NULL == name_n)
        return VAL_BAD_ARGUMENT;

    name_len = wire_name_length(name_n);

    /*
     * Check if the zone is trusted 
     */
    zse_pol =
        RETRIEVE_POLICY(ctx, P_ZONE_SECURITY_EXPECTATION,
                        struct zone_se_policy *);
    if (zse_pol != NULL) {
        for (zse_cur = zse_pol;
             zse_cur && (wire_name_length(zse_cur->zone_n) > name_len);
             zse_cur = zse_cur->next);

        /*
         * for all zones which are shorter or as long, do a strstr 
         */
        // XXX We will probably need to use namecmp() instead so that
        // XXX casing and endien order are accounted for 
        /*
         * Because of the ordering, the longest match is found first 
         */
        for (; zse_cur; zse_cur = zse_cur->next) {
            int             root_zone = 0;
            if (!namecmp(zse_cur->zone_n, (const u_int8_t *) ""))
                root_zone = 1;
            else {
                /*
                 * Find the last occurrence of zse_cur->zone_n in name_n 
                 */
                p = name_n;
                q = (u_int8_t *) strstr((char *) p,
                                        (char *) zse_cur->zone_n);
                while (q != NULL) {
                    p = q;
                    q = (u_int8_t *) strstr((char *) q + 1,
                                            (char *) zse_cur->zone_n);
                }
            }

            if (root_zone
                || (!strcmp((char *) p, (char *) zse_cur->zone_n))) {
                if (-1 == ns_name_ntop(name_n, name_p, sizeof(name_p)))
                    snprintf(name_p, sizeof(name_p), "unknown/error");
                if (zse_cur->trusted == ZONE_SE_UNTRUSTED) {
                    val_log(ctx, LOG_DEBUG, "zone %s is not trusted",
                            name_p);
                    return VAL_A_UNTRUSTED_ZONE;
                } else if (zse_cur->trusted == ZONE_SE_DO_VAL) {
                    val_log(ctx, LOG_DEBUG, "%s requires DNSSEC", name_p);
                    return VAL_A_WAIT_FOR_TRUST;
                } else {
                    /** ZONE_SE_IGNORE */
                    val_log(ctx, LOG_DEBUG, "Ignoring DNSSEC for  %s",
                            name_p);
                    return VAL_A_TRUST_ZONE;
                }
            }
        }
    }
    if (-1 == ns_name_ntop(name_n, name_p, sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
    val_log(ctx, LOG_DEBUG, "%s requires DNSSEC", name_p);

    return VAL_A_WAIT_FOR_TRUST;
}

static u_int16_t
is_trusted_key(val_context_t * ctx, u_int8_t * zone_n, struct rr_rec *key)
{
    struct trust_anchor_policy *ta_pol, *ta_cur, *ta_tmphead;
    int             name_len;
    u_int8_t       *zp = zone_n;
    val_dnskey_rdata_t dnskey;
    struct rr_rec  *curkey;

    /*
     * This function should never be called with a NULL zone_n, but still... 
     */
    if (zone_n == NULL)
        return VAL_A_NO_TRUST_ANCHOR;

    name_len = wire_name_length(zp);
    ta_pol =
        RETRIEVE_POLICY(ctx, P_TRUST_ANCHOR, struct trust_anchor_policy *);
    if (ta_pol == NULL)
        return VAL_A_NO_TRUST_ANCHOR;

    /*
     * skip longer names 
     */
    for (ta_cur = ta_pol;
         ta_cur && (wire_name_length(ta_cur->zone_n) > name_len);
         ta_cur = ta_cur->next);

    /*
     * for the remaining nodes, if the length of the zones are 
     * the same, look for an exact match 
     */
    for (; ta_cur &&
         (wire_name_length(ta_cur->zone_n) == name_len);
         ta_cur = ta_cur->next) {

        if (!namecmp(ta_cur->zone_n, zp)) {

            for (curkey = key; curkey; curkey = curkey->rr_next) {
                val_parse_dnskey_rdata(curkey->rr_rdata,
                                       curkey->rr_rdata_length_h, &dnskey);
                if (!dnskey_compare(&dnskey, ta_cur->publickey)) {
                    char            name_p[NS_MAXDNAME];
                    if (-1 == ns_name_ntop(zp, name_p, sizeof(name_p)))
                        snprintf(name_p, sizeof(name_p), "unknown/error");
                    if (dnskey.public_key != NULL)
                        FREE(dnskey.public_key);
                    curkey->rr_status = VAL_A_VERIFIED_LINK;
                    val_log(ctx, LOG_DEBUG, "key %s is trusted", name_p);
                    return VAL_A_TRUST_KEY;
                }
                if (dnskey.public_key != NULL)
                    FREE(dnskey.public_key);
            }
        }
    }


    /*
     * for the remaining nodes, see if there is any hope 
     */
    ta_tmphead = ta_cur;
    while ((zp != NULL) && zp[0]) {
        /*
         * trim the top label from our candidate zone 
         */
        zp += (int) zp[0] + 1;
        for (ta_cur = ta_tmphead; ta_cur; ta_cur = ta_cur->next) {
            if (wire_name_length(zp) < wire_name_length(ta_cur->zone_n))
                /** next time look from this point */
                ta_tmphead = ta_cur->next;

            if (namecmp(ta_cur->zone_n, zp) == 0) {
                /** We have hope */
                return VAL_A_WAIT_FOR_TRUST;
            }
        }
    }

    val_log(ctx, LOG_DEBUG,
            "Cannot find a good trust anchor for the chain of trust above %s",
            zp);
    return VAL_A_NO_TRUST_ANCHOR;
}


static int
set_ans_kind(u_int8_t * qc_name_n,
             const u_int16_t q_type_h,
             const u_int16_t q_class_h,
             struct rrset_rec *the_set, u_int16_t * status)
{
    if ((NULL == the_set) || (NULL == status))
        return VAL_BAD_ARGUMENT;

    /*
     * Answer is a Referral if... 
     */
    /*
     * Referals won't make it this far, they are handled in digest_response 
     */

    if ((the_set->rrs.val_rrset_data == NULL)
        && (the_set->rrs.val_rrset_sig != NULL)) {
        the_set->rrs_ans_kind = SR_ANS_BARE_RRSIG;
        return VAL_NO_ERROR;
    }

    /*
     * Answer is a NACK_NSEC if... 
     */
    if (the_set->rrs.val_rrset_type_h == ns_t_nsec) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_nsec))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_NSEC;

        return VAL_NO_ERROR;
    }
#ifdef LIBVAL_NSEC3
    /*
     * Answer is a NACK_NSEC3 if... 
     */
    if (the_set->rrs.val_rrset_type_h == ns_t_nsec3) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_nsec3))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_NSEC3;

        return VAL_NO_ERROR;
    }
#endif

    /*
     * Answer is a NACK_SOA if... 
     */

    if (the_set->rrs.val_rrset_type_h == ns_t_soa) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_soa))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_NACK_SOA;

        return VAL_NO_ERROR;
    }

    /*
     * Answer is a CNAME if... 
     */

    if (the_set->rrs.val_rrset_type_h == ns_t_cname) {
        if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) == 0 &&
            (q_type_h == ns_t_any || q_type_h == ns_t_cname))
            /*
             * We asked for it 
             */
            the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        else
            the_set->rrs_ans_kind = SR_ANS_CNAME;

        return VAL_NO_ERROR;
    }

    /*
     * Answer is an ANSWER if... 
     */
    if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) == 0 &&
        (q_type_h == ns_t_any
         || q_type_h == the_set->rrs.val_rrset_type_h)) {
        /*
         * We asked for it 
         */
        the_set->rrs_ans_kind = SR_ANS_STRAIGHT;
        return VAL_NO_ERROR;
    }

    the_set->rrs_ans_kind = SR_ANS_UNSET;
    *status = VAL_A_DNS_ERROR_BASE + SR_WRONG_ANSWER;

    return VAL_GENERIC_ERROR;
}

#define TOP_OF_QNAMES   0
#define MID_OF_QNAMES   1
#define NOT_IN_QNAMES   2

static int
name_in_q_names(struct qname_chain *q_names_n, struct rrset_rec *the_set)
{
    struct qname_chain *temp_qc;

    if ((the_set == NULL) || (q_names_n == NULL))
        return NOT_IN_QNAMES;

    if (namecmp(the_set->rrs.val_rrset_name_n, q_names_n->qnc_name_n) == 0)
        return TOP_OF_QNAMES;

    temp_qc = q_names_n->qnc_next;

    while (temp_qc) {
        if (namecmp(the_set->rrs.val_rrset_name_n, temp_qc->qnc_name_n) ==
            0)
            return MID_OF_QNAMES;
        temp_qc = temp_qc->qnc_next;
    }

    return NOT_IN_QNAMES;
}

int
fails_to_answer_query(struct qname_chain *q_names_n,
                      const u_int16_t q_type_h,
                      const u_int16_t q_class_h,
                      struct rrset_rec *the_set, u_int16_t * status)
{
    int             name_present = name_in_q_names(q_names_n, the_set);
    int             type_match = the_set->rrs.val_rrset_type_h == q_type_h
        || q_type_h == ns_t_any;
    int             class_match =
        the_set->rrs.val_rrset_class_h == q_class_h
        || q_class_h == ns_c_any;
    int             data_present = the_set->rrs.val_rrset_data != NULL;

    if ((NULL == the_set) || (NULL == status))
        return TRUE;

    name_present = name_in_q_names(q_names_n, the_set);
    type_match = the_set->rrs.val_rrset_type_h == q_type_h
        || q_type_h == ns_t_any;
    class_match = the_set->rrs.val_rrset_class_h == q_class_h
        || q_class_h == ns_c_any;
    data_present = the_set->rrs.val_rrset_data != NULL;

    /*
     * Could be an empty answer or only RRSIGs 
     */
    if (!data_present)
        return FALSE;

    if (!class_match ||
        (!type_match && the_set->rrs_ans_kind == SR_ANS_STRAIGHT) ||
        (type_match && the_set->rrs_ans_kind != SR_ANS_STRAIGHT) ||
        (name_present != TOP_OF_QNAMES && type_match &&
         the_set->rrs_ans_kind == SR_ANS_STRAIGHT) ||
        (name_present != MID_OF_QNAMES && !type_match &&
         the_set->rrs_ans_kind == SR_ANS_CNAME) ||
        (name_present == MID_OF_QNAMES && !type_match &&
         (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC ||
#ifdef LIBVAL_NSEC3
          the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3 ||
#endif
          the_set->rrs_ans_kind == SR_ANS_NACK_SOA))
        ) {
        *status = VAL_A_DNS_ERROR_BASE + SR_WRONG_ANSWER;
        return TRUE;
    }

    return FALSE;
}


/*
 * Add a new assertion for the response data 
 *
 * Returns:
 * VAL_NO_ERROR                 Operation succeeded
 * VAL_OUT_OF_MEMORY    Could not allocate enough memory for operation
 * VAL_BAD_ARGUMENT     Bad argument (eg NULL ptr)
 */
static int
add_to_authentication_chain(struct val_authentication_chain **assertions,
                            struct rrset_rec *rrset)
{
    struct val_authentication_chain *new_as, *first_as, *prev_as;
    struct rrset_rec *next_rr;

    if (NULL == assertions)
        return VAL_BAD_ARGUMENT;

    first_as = NULL;
    prev_as = NULL;

    next_rr = rrset;
    while (next_rr) {

        new_as =
            (struct val_authentication_chain *)
            MALLOC(sizeof(struct val_authentication_chain));
        if (new_as == NULL)
            return VAL_OUT_OF_MEMORY;

        new_as->_as.ac_data = copy_rrset_rec(next_rr);

        new_as->val_ac_trust = NULL;
        new_as->_as.val_ac_rrset_next = NULL;
        new_as->_as.val_ac_next = NULL;
        new_as->_as.ac_pending_query = NULL;
        new_as->val_ac_status = VAL_A_INIT;
        if (first_as != NULL) {
            /*
             * keep the first assertion constant 
             */
            new_as->_as.val_ac_next = first_as->_as.val_ac_next;
            first_as->_as.val_ac_next = new_as;
            prev_as->_as.val_ac_rrset_next = new_as;

        } else {
            first_as = new_as;
            new_as->_as.val_ac_next = *assertions;
            *assertions = new_as;
        }
        prev_as = new_as;
        next_rr = next_rr->rrs_next;
    }

    return VAL_NO_ERROR;
}

/*
 * Free up the authentication chain.
 */
void
free_authentication_chain(struct val_authentication_chain *assertions)
{

    if (assertions == NULL)
        return;

    // xxx-audit: opportunity for disaster
    //            having a free function for a structure w/a union w/
    //            differing characteristics gives me the heebeejeebees.
    //            this blindly assumes that _as is the right union memeber.
    if (assertions->_as.val_ac_next)
        free_authentication_chain(assertions->_as.val_ac_next);

    if (assertions->_as.ac_data)
        res_sq_free_rrset_recs(&(assertions->_as.ac_data));

    FREE(assertions);
}

/*
 * For a given assertion identify its pending queries
 */
static int
build_pending_query(val_context_t * context,
                    struct val_query_chain **queries,
                    struct val_authentication_chain *as)
{
    u_int8_t       *signby_name_n;
    u_int16_t       tzonestatus;
    int             retval;

    if ((NULL == queries) || (NULL == as))
        return VAL_BAD_ARGUMENT;

    if (as->_as.ac_data == NULL) {
        as->val_ac_status = VAL_A_DATA_MISSING;
        return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs_ans_kind == SR_ANS_BARE_RRSIG) {
        as->val_ac_status = VAL_A_BARE_RRSIG;
        return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs.val_rrset_data == NULL) {
        as->val_ac_status = VAL_A_DATA_MISSING;
        return VAL_NO_ERROR;
    }

    /*
     * Check if this zone is locally trusted/untrusted 
     */
    tzonestatus =
        is_trusted_zone(context, as->_as.ac_data->rrs.val_rrset_name_n);
    if (tzonestatus != VAL_A_WAIT_FOR_TRUST) {
        as->val_ac_status = tzonestatus;
        return VAL_NO_ERROR;
    }

    if (as->_as.ac_data->rrs.val_rrset_sig == NULL) {
        as->val_ac_status = VAL_A_WAIT_FOR_RRSIG;
        /*
         * create a query and link it as the pending query for this assertion 
         */
        if (VAL_NO_ERROR != (retval = add_to_query_chain(queries,
                                                         as->_as.ac_data->
                                                         rrs.
                                                         val_rrset_name_n,
                                                         ns_t_rrsig,
                                                         as->_as.ac_data->
                                                         rrs.
                                                         val_rrset_class_h)))
            return retval;
        as->_as.ac_pending_query = *queries;    /* The first value in the list is the most recent element */
        return VAL_NO_ERROR;
    }

    /*
     * Identify the DNSKEY that created the RRSIG:
     */

    // xxx-audit: ptr deref w/out NULL check
    // if ((NULL == as->_as.ac_data->rrs.val_rrset_sig) ||
    //     (NULL == as->_as.ac_data->rrs.val_rrset_sig->rr_rdata))
    //     return VAL_???

    /*
     * First identify the signer name from the RRSIG 
     */
    signby_name_n = &as->_as.ac_data->rrs.val_rrset_sig->rr_rdata[SIGNBY];
    //XXX The signer name has to be within the zone

    /*
     * Then look for  {signby_name_n, DNSKEY/DS, type} 
     */
    if (as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey) {

        as->val_ac_status =
            is_trusted_key(context, signby_name_n,
                           as->_as.ac_data->rrs.val_rrset_data);
        if (as->val_ac_status != VAL_A_WAIT_FOR_TRUST)
            return VAL_NO_ERROR;

        /*
         * Create a query for missing data 
         */
        if (VAL_NO_ERROR !=
            (retval =
             add_to_query_chain(queries, signby_name_n, ns_t_ds,
                                as->_as.ac_data->rrs.val_rrset_class_h)))
            return retval;

    } else {
        /*
         * look for DNSKEY records 
         */
        if (VAL_NO_ERROR !=
            (retval =
             add_to_query_chain(queries, signby_name_n, ns_t_dnskey,
                                as->_as.ac_data->rrs.val_rrset_class_h)))
            return retval;
        as->val_ac_status = VAL_A_WAIT_FOR_TRUST;
    }

    as->_as.ac_pending_query = *queries;        /* The first value in the list is the most recent element */
    return VAL_NO_ERROR;
}


/*
 * Read the response that came in and create assertions from it. Set the state
 * of the assertion based on what data is available and whether validation
 * can proceed.
 * 
 * Returns:
 * VAL_NO_ERROR                 Operation completed successfully
 * SR_CALL_ERROR        If the name could not be converted from host to network format
 *
 */
static int
assimilate_answers(val_context_t * context,
                   struct val_query_chain **queries,
                   struct domain_info *response,
                   struct val_query_chain *matched_q,
                   struct val_authentication_chain **assertions,
                   u_int8_t flags)
{
    int             retval;
    struct val_authentication_chain *as = NULL;
    u_int16_t       type_h;
    u_int16_t       class_h;
    u_int8_t        kind = SR_ANS_UNSET;

    if (matched_q == NULL)
        return VAL_NO_ERROR;

    if ((NULL == queries) || (NULL == response) || (NULL == assertions))
        return VAL_BAD_ARGUMENT;        // xxx-audit: or SR_??

    type_h = response->di_requested_type_h;
    class_h = response->di_requested_class_h;

    if (matched_q->qc_as != NULL) {
        /*
         * We already had an assertion for this query 
         */
        // XXX What about FLOOD_ATTACKS ?
        return VAL_NO_ERROR;
    }

    if (response->di_rrset == NULL) {
        matched_q->qc_state = Q_ERROR_BASE + SR_NO_ANSWER;
        return VAL_NO_ERROR;
    }

    /*
     * Create an assertion for the response data 
     */
    if (VAL_NO_ERROR !=
        (retval =
         add_to_authentication_chain(assertions, response->di_rrset)))
        return retval;

    as = *assertions;           /* The first value in the list is the most recent element */

    /*
     * Link the original query to the above assertion 
     */
    matched_q->qc_as = as;

    /*
     * Identify the state for each of the assertions obtained 
     */
    for (; as; as = as->_as.val_ac_rrset_next) {

        /*
         * Cover error conditions first 
         * SOA checks will appear during sanity checks later on 
         */
        // xxx-audit: ptr deref w/out NULL check (response->di_qnames)
        if ((NULL == response->di_qnames) ||    // xxx-check: is continue ok in this case??
            (set_ans_kind(response->di_qnames->qnc_name_n, type_h, class_h,
                          as->_as.ac_data,
                          &as->val_ac_status) != VAL_NO_ERROR)
            || fails_to_answer_query(response->di_qnames, type_h, class_h,
                                     as->_as.ac_data,
                                     &as->val_ac_status)) {

            continue;
        }

        if (kind == SR_ANS_UNSET)
            kind = as->_as.ac_data->rrs_ans_kind;
        else {
            switch (kind) {
                /*
                 * STRAIGHT and CNAME are OK 
                 */
            case SR_ANS_STRAIGHT:
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_STRAIGHT) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_CNAME)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

            case SR_ANS_CNAME:
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_STRAIGHT) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_CNAME) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_SOA) &&
#ifdef LIBVAL_NSEC3
                    /*
                     * check if there is a mix of NSEC and NSEC3 later in the proof 
                     */
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC3) &&
#endif
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * Only bare RRSIGs together 
                 */
            case SR_ANS_BARE_RRSIG:
                if (as->_as.ac_data->rrs_ans_kind != SR_ANS_BARE_RRSIG) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * NACK_NXT and NACK_SOA are OK 
                 */
            case SR_ANS_NACK_NSEC:
#ifdef LIBVAL_NSEC3
            case SR_ANS_NACK_NSEC3:
#endif
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC) &&
#ifdef LIBVAL_NSEC3
                    /*
                     * check if there is a mix of NSEC and NSEC3 later in the proof 
                     */
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC3) &&
#endif
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_SOA) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_CNAME)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

            case SR_ANS_NACK_SOA:
                if ((as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC) &&
#ifdef LIBVAL_NSEC3
                    /*
                     * check if there is a mix of NSEC and NSEC3 later in the proof 
                     */
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_NSEC3) &&
#endif
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_NACK_SOA) &&
                    (as->_as.ac_data->rrs_ans_kind != SR_ANS_CNAME)) {
                    matched_q->qc_state =
                        Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
                }
                break;

                /*
                 * Never Reached 
                 */
            default:
                matched_q->qc_state =
                    Q_ERROR_BASE + SR_CONFLICTING_ANSWERS;
            }
        }

        if (flags & F_DONT_VALIDATE)
            as->val_ac_status = VAL_A_DONT_VALIDATE;
        else if (!matched_q->qc_glue_request) {
            if (VAL_NO_ERROR !=
                (retval = build_pending_query(context, queries, as)))
                return retval;
        }

    }
    return VAL_NO_ERROR;
}


static void
prove_nsec_wildcard_check(val_context_t * ctx,
                          u_int16_t qc_type_h,
                          struct rrset_rec *wcard_proof,
                          u_int8_t * closest_encounter,
                          val_status_t * status)
{
    /*
     * Check the wild card proof 
     */
    /*
     * prefix "*" to the closest encounter, and check if that 
     * * name falls within the range given in wcard_proof
     */
    u_int8_t       *nxtname;
    u_char          domain_name_n[NS_MAXCDNAME];
    if ((NULL == wcard_proof) || (NULL == wcard_proof->rrs.val_rrset_data))
        nxtname = NULL;
    else
        nxtname = wcard_proof->rrs.val_rrset_data->rr_rdata;

    if (NS_MAXCDNAME < wire_name_length(closest_encounter) + 2) {
        val_log(ctx, LOG_DEBUG,
                "NSEC Error: label length with wildcard exceeds bounds");
        *status = VAL_R_BOGUS_PROOF;
        return;
    }

    domain_name_n[0] = 0x01;
    domain_name_n[1] = 0x2a;    /* for the '*' character */
    memcpy(&domain_name_n[2], closest_encounter,
           wire_name_length(closest_encounter));
    /*
     *  either we should be able to prove that wild card does not exist, 
     *  or that type is not
     *  present at that wild card 
     */
    if ((nxtname != NULL) &&
        (!namecmp(domain_name_n, wcard_proof->rrs.val_rrset_name_n))) {

        int             nsec_bit_field;
        nsec_bit_field =
            wire_name_length(wcard_proof->rrs.val_rrset_data->rr_rdata);
        if (is_type_set
            ((&
              (wcard_proof->rrs.val_rrset_data->rr_rdata[nsec_bit_field])),
             wcard_proof->rrs.val_rrset_data->rr_rdata_length_h -
             nsec_bit_field, qc_type_h)) {
            val_log(ctx, LOG_DEBUG, "NSEC error: type exists at wildcard");
            *status = VAL_R_BOGUS_PROOF;
        }
    } else if ((nxtname == NULL) ||
               (namecmp(domain_name_n, wcard_proof->rrs.val_rrset_name_n) <
                0) || (namecmp(nxtname, domain_name_n) < 0)) {
        val_log(ctx, LOG_DEBUG,
                "NSEC error: Incorrect span for wildcard proof");
        *status = VAL_R_BOGUS_PROOF;
    }
}

static void
nsec_proof_chk(val_context_t * ctx,
               struct rrset_rec *the_set, u_int8_t * qc_name_n,
               u_int16_t qc_type_h, u_int8_t * soa_name_n, int *span_chk,
               int *wcard_chk, struct rrset_rec **wcard_proof,
               u_int8_t ** closest_encounter, val_status_t * status)
{

    int             nsec_bit_field;

    if (!namecmp(the_set->rrs.val_rrset_name_n, qc_name_n)) {
        struct rr_rec  *sig;
        int             wcard;

        /*
         * NSEC owner = query name & q_type not in list 
         */
        nsec_bit_field =
            wire_name_length(the_set->rrs.val_rrset_data->rr_rdata);
        if (is_type_set
            ((&(the_set->rrs.val_rrset_data->rr_rdata[nsec_bit_field])),
             the_set->rrs.val_rrset_data->rr_rdata_length_h -
             nsec_bit_field, qc_type_h)) {
            val_log(ctx, LOG_DEBUG,
                    "NSEC error: Type exists at NSEC record");
            *status = VAL_R_BOGUS_PROOF;
            return;
        }

        *span_chk = 1;
        *status = VAL_NONEXISTENT_TYPE;

        /*
         * if the label count in the RRSIG equals the labels
         * in the nsec owner name, wildcard absence is also proved
         * Be sure to check the label count in an RRSIG that was 
         * verified
         */
        for (sig = the_set->rrs.val_rrset_sig; sig; sig = sig->rr_next) {
            if ((sig->rr_status == VAL_A_RRSIG_VERIFIED) &&
                (VAL_NO_ERROR ==
                 check_label_count(the_set, sig, &wcard))) {
                if (wcard == 0)
                    *wcard_chk = 1;
                return;
            }
        }
    } else if (namecmp(the_set->rrs.val_rrset_name_n, qc_name_n) > 0) {
        /*
         * query name comes after the NSEC owner 
         */
        val_log(ctx, LOG_DEBUG, "NSEC error: Incorrect span");
        *status = VAL_R_BOGUS_PROOF;
        return;
    }

    /*
     * else 
     */

    /*
     * Find the next name 
     */
    u_int8_t       *nxtname = the_set->rrs.val_rrset_data ?
        the_set->rrs.val_rrset_data->rr_rdata : NULL;

    if (namecmp(qc_name_n, nxtname) > 0) {
        /*
         * check if the next name wraps around 
         */
        if (namecmp(nxtname, soa_name_n) != 0) {
            /*
             * if no, check if this is the proof for no wild-card present 
             * i.e the proof must tell us that "*" does not exist 
             */
            *wcard_proof = the_set;
            return;
        }
    }

    *span_chk = 1;
    /*
     * The same NSEC may prove wildcard absence also 
     */
    if (*wcard_proof == NULL)
        *wcard_proof = the_set;

    /*
     * The closest encounter is the longest label match between 
     * * this NSEC's owner name and the query name
     */
    int             maxoffset = wire_name_length(qc_name_n);
    int             offset = qc_name_n[0] + 1;
    while (offset < maxoffset) {
        u_int8_t       *cur_name_n = &qc_name_n[offset];
        int             cmp;
        if ((cmp =
             namecmp(cur_name_n, the_set->rrs.val_rrset_name_n)) == 0) {
            *closest_encounter = cur_name_n;
            break;
        } else if (cmp < 0) {
            /*
             * strip off one label from the NSEC owner name 
             */
            *closest_encounter = the_set->rrs.val_rrset_name_n ?
                &the_set->rrs.val_rrset_name_n[the_set->rrs.
                                               val_rrset_name_n[0] +
                                               1] : NULL;
            break;
        }
        offset += cur_name_n[0] + 1;
    }

    return;
}


#ifdef LIBVAL_NSEC3
u_int8_t       *
compute_nsec3_hash(val_context_t * ctx, u_int8_t * qc_name_n,
                   u_int8_t * soa_name_n, u_int8_t alg, u_int16_t iter,
                   u_int8_t saltlen, u_int8_t * salt,
                   u_int8_t * b32_hashlen, u_int8_t ** b32_hash)
{
    SHA_CTX         c;
    int             i;
    int             name_len;
    struct nsec3_max_iter_policy *pol, *cur;
    u_int8_t       *p, *q;
    char            name_p[NS_MAXDNAME];
    u_int8_t        hashlen;
    u_int8_t       *hash;

    if (alg != ALG_NSEC3_HASH_SHA1)
        return NULL;

    pol = NULL;

    if (soa_name_n != NULL) {
        name_len = wire_name_length(soa_name_n);
        pol =
            RETRIEVE_POLICY(ctx, P_NSEC3_MAX_ITER,
                            struct nsec3_max_iter_policy *);
    }

    if (pol != NULL) {
        /*
         * go past longer names 
         */
        for (cur = pol;
             cur && (wire_name_length(cur->zone_n) > name_len);
             cur = cur->next);

        /*
         * for all zones which are shorter or as long, do a strstr 
         */
        // XXX We will probably need to use namecmp() instead so that
        // XXX casing and endien order are accounted for 
        /*
         * Because of the ordering, the longest match is found first 
         */
        for (; cur; cur = cur->next) {
            int             root_zone = 0;
            if (!namecmp(cur->zone_n, (const u_int8_t *) ""))
                root_zone = 1;
            else {
                /*
                 * Find the last occurrence of cur->zone_n in soa_name_n 
                 */
                p = soa_name_n;
                q = (u_int8_t *) strstr((char *) p, (char *) cur->zone_n);
                while (q != NULL) {
                    p = q;
                    q = (u_int8_t *) strstr((char *) q + 1,
                                            (char *) cur->zone_n);
                }
            }

            if (root_zone || (!strcmp((char *) p, (char *) cur->zone_n))) {
                if (-1 == ns_name_ntop(soa_name_n, name_p, sizeof(name_p)))
                    snprintf(name_p, sizeof(name_p), "unknown/error");

                if (cur->iter < iter)
                    return NULL;
                break;
            }
        }
    }

    hashlen = SHA_DIGEST_LENGTH;
    hash = (u_int8_t *) MALLOC(SHA_DIGEST_LENGTH * sizeof(u_int8_t));
    if (hash == NULL)
        return NULL;

    memset(hash, 0, SHA_DIGEST_LENGTH);

    /*
     * IH(salt, x, 0) = H( x || salt) 
     */
    SHA1_Init(&c);
    SHA1_Update(&c, qc_name_n, wire_name_length(qc_name_n));
    SHA1_Update(&c, salt, saltlen);
    SHA1_Final(hash, &c);

    /*
     * IH(salt, x, k) = H(IH(salt, x, k-1) || salt) 
     */
    for (i = 0; i < iter; i++) {
        SHA1_Init(&c);
        SHA1_Update(&c, hash, hashlen);
        SHA1_Update(&c, salt, saltlen);
        SHA1_Final(hash, &c);
    }

    base32hex_encode(hash, hashlen, b32_hash, b32_hashlen);
    FREE(hash);
    return *b32_hash;
}

static void
nsec3_proof_chk(val_context_t * ctx, struct val_result_chain *results,
                u_int8_t * qc_name_n, u_int16_t qc_type_h,
                u_int8_t * soa_name_n, val_status_t * status)
{

    struct val_result_chain *res;
    u_int8_t        hashlen;
    u_int8_t        nsec3_hashlen;
    val_nsec3_rdata_t nd;
    u_int8_t       *cp = NULL;
    u_int8_t       *cpe = NULL;
    u_int8_t       *ncn = NULL;
    u_char          wc_n[NS_MAXCDNAME];
    u_int8_t       *hash = NULL;
    u_int8_t       *nsec3_hash = NULL;
    int             optout = 0;

    cp = qc_name_n;

    while ((namecmp(cp, soa_name_n) >= 0) && !cpe) {

        /*
         * we have all the data we're looking for 
         */
        if (ncn && ((ncn == cpe) || (cpe == (ncn + ncn[0] + 1)))) {
            break;
        }

        for (res = results;
             res && res->val_rc_trust && res->val_rc_trust->_as.ac_data;
             res = res->val_rc_next) {

            struct rrset_rec *the_set = res->val_rc_trust->_as.ac_data;

            if (the_set->rrs_ans_kind != SR_ANS_NACK_NSEC3)
                continue;

            nsec3_hashlen = the_set->rrs.val_rrset_name_n[0];
            nsec3_hash =
                (nsec3_hashlen ==
                 0) ? NULL : the_set->rrs.val_rrset_name_n + 1;

            if (NULL ==
                val_parse_nsec3_rdata(the_set->rrs.val_rrset_data->
                                      rr_rdata,
                                      the_set->rrs.val_rrset_data->
                                      rr_rdata_length_h, &nd)) {
                val_log(ctx, LOG_DEBUG, "Cannot parse NSEC3 rdata");
                *status = VAL_R_BOGUS_PROOF;
                return;
            }

            /*
             * hash name according to nsec3 parameters 
             */
            if (NULL ==
                compute_nsec3_hash(ctx, cp, soa_name_n, nd.alg,
                                   nd.iterations, nd.saltlen, nd.salt,
                                   &hashlen, &hash)) {
                val_log(ctx, LOG_DEBUG,
                        "Cannot compute NSEC3 hash with given params");
                *status = VAL_R_BOGUS_PROOF;
                FREE(nd.nexthash);
                return;
            }

            /*
             * Check if there is an exact match 
             */
            if ((nsec3_hashlen == hashlen)
                && !memcmp(hash, nsec3_hash, hashlen)) {
                struct rr_rec  *sig;
                int             wcard;
                int             nsec3_bm_len =
                    the_set->rrs.val_rrset_data->rr_rdata_length_h -
                    nd.bit_field;

                /*
                 * This is the closest provable encounter 
                 */
                cpe = cp;
#if 0
                /*
                 * NS can only be set if the SOA bit is set 
                 */
                /*
                 * XXX The NSEC3 that proves that a DS record for a delegation is absent
                 * * XXX is an exact match for that delegation owner name. The NS bit will be
                 * * XXX set but the SOA will not. So there is some confusion here.
                 */
                if ((is_type_set
                     ((&
                       (the_set->rrs.val_rrset_data->
                        rr_rdata[nd.bit_field])), nsec3_bm_len, ns_t_ns))
                    &&
                    (!is_type_set
                     ((&
                       (the_set->rrs.val_rrset_data->
                        rr_rdata[nd.bit_field])), nsec3_bm_len,
                      ns_t_soa))) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: NS can only be set if the SOA bit is set");
                    *status = VAL_R_BOGUS_PROOF;
                    FREE(nd.nexthash);
                    FREE(hash);
                    return;
                }
#endif
                /*
                 * hashes match 
                 */
                if (cp == qc_name_n) {
                    /*
                     * this is the query name 
                     * make sure that type is missing 
                     */

                    if (is_type_set
                        ((&
                          (the_set->rrs.val_rrset_data->
                           rr_rdata[nd.bit_field])), nsec3_bm_len,
                         qc_type_h)) {
                        val_log(ctx, LOG_DEBUG,
                                "NSEC3 error: hashes equal but type is present");
                        *status = VAL_R_BOGUS_PROOF;
                        FREE(nd.nexthash);
                        FREE(hash);
                        return;
                    }

                    ncn = cp;
                    *status = VAL_NONEXISTENT_TYPE;

                    /*
                     * if the label count in the RRSIG equals the labels
                     * in the nsec owner name, wildcard absence is also proved
                     * Be sure to check the label count in an RRSIG that was 
                     * verified
                     */
                    for (sig = the_set->rrs.val_rrset_sig; sig;
                         sig = sig->rr_next) {
                        if ((sig->rr_status == VAL_A_RRSIG_VERIFIED)
                            && (VAL_NO_ERROR ==
                                check_label_count(the_set, sig, &wcard))) {
                            if (wcard == 0) {
                                /*
                                 * proof complete 
                                 */
                                FREE(nd.nexthash);
                                FREE(hash);
                                return;
                            }
                            /*
                             * still need to do wildcard check 
                             */
                            break;
                        }
                    }
                }
            }

            /*
             * Check if NSEC3 covers the hash 
             */
            if (CHECK_RANGE
                (nsec3_hash, nsec3_hashlen, nd.nexthash, nd.nexthashlen,
                 hash, hashlen)) {
                ncn = cp;
                if (nd.optout) {
                    optout = 1;
                } else {
                    optout = 0;
                }
            }

            FREE(nd.nexthash);
            FREE(hash);
        }

        /*
         * strip leading label 
         */
        cp += cp[0] + 1;
    }

    if (!ncn || !cpe) {
        if (!ncn)
            val_log(ctx, LOG_DEBUG, "NSEC3 error: NCN was not found");
        if (!cpe)
            val_log(ctx, LOG_DEBUG, "NSEC3 error: CPE was not found");
        *status = VAL_R_INCOMPLETE_PROOF;
        return;
    }

    /*
     * if ncn is not one label greater than cpe then we have a problem 
     */
    if ((ncn != cpe) && (cpe != (ncn + ncn[0] + 1))) {
        val_log(ctx, LOG_DEBUG,
                "NSEC3 error: NCN is not one label greater than CPE");
        *status = VAL_R_BOGUS_PROOF;
        return;
    }

    if (NS_MAXCDNAME < wire_name_length(cpe) + 2) {
        val_log(ctx, LOG_DEBUG,
                "NSEC3 Error: label length with wildcard exceeds bounds");
        *status = VAL_R_BOGUS_PROOF;
        return;
    }
    /*
     * Check for wildcard 
     */
    /*
     * Create a the name *.cpe 
     */
    memset(wc_n, 0, sizeof(wc_n));
    wc_n[0] = 0x01;
    wc_n[1] = 0x2a;             /* for the '*' character */
    memcpy(&wc_n[2], cpe, wire_name_length(cpe));

    for (res = results;
         res && res->val_rc_trust && res->val_rc_trust->_as.ac_data;
         res = res->val_rc_next) {
        struct rrset_rec *the_set = res->val_rc_trust->_as.ac_data;
        if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3) {

            nsec3_hashlen = the_set->rrs.val_rrset_name_n[0];
            nsec3_hash =
                (nsec3_hashlen ==
                 0) ? NULL : the_set->rrs.val_rrset_name_n + 1;

            if (NULL ==
                val_parse_nsec3_rdata(the_set->rrs.val_rrset_data->
                                      rr_rdata,
                                      the_set->rrs.val_rrset_data->
                                      rr_rdata_length_h, &nd)) {
                val_log(ctx, LOG_DEBUG,
                        "NSEC3 error: Cannot parse NSEC3 rdata");
                *status = VAL_R_BOGUS_PROOF;
                return;
            }

            /*
             * hash name according to nsec3 parameters 
             */
            if (NULL ==
                compute_nsec3_hash(ctx, wc_n, soa_name_n, nd.alg,
                                   nd.iterations, nd.saltlen, nd.salt,
                                   &hashlen, &hash)) {
                val_log(ctx, LOG_DEBUG,
                        "NSEC3 error: Cannot compute hash with given params");
                FREE(nd.nexthash);
                *status = VAL_R_BOGUS_PROOF;
                return;
            }
            if (!nsec3_order_cmp(nsec3_hash, nsec3_hashlen, hash, hashlen)) {
                /*
                 * if type is set, that's a problem 
                 */
                if (is_type_set
                    ((&
                      (the_set->rrs.val_rrset_data->
                       rr_rdata[nd.bit_field])),
                     the_set->rrs.val_rrset_data->rr_rdata_length_h -
                     nd.bit_field, qc_type_h)) {
                    val_log(ctx, LOG_DEBUG,
                            "NSEC3 error: wildcard proof does not prove non-existence");
                    *status = VAL_R_BOGUS_PROOF;
                } else
                    *status = VAL_NONEXISTENT_TYPE;
                FREE(nd.nexthash);
                FREE(hash);
                return;
            } else
                if (CHECK_RANGE
                    (nsec3_hash, nsec3_hashlen, nd.nexthash,
                     nd.nexthashlen, hash, hashlen)) {
                /*
                 * proved 
                 */
                FREE(nd.nexthash);
                FREE(hash);
                if (optout) {
                    *status = VAL_NONEXISTENT_NAME_OPTOUT;
                } else {
                    *status = VAL_NONEXISTENT_NAME;
                }
                return;
            }

            FREE(nd.nexthash);
            FREE(hash);
        }
    }

    val_log(ctx, LOG_DEBUG, "NSEC3 error: wildcard proof does not exist");
    /*
     * Could not find a proof covering the wildcard 
     */
    *status = VAL_R_BOGUS_PROOF;
}
#endif

static void
prove_nonexistence(val_context_t * ctx, struct val_query_chain *top_q,
                   struct val_result_chain *results)
{
    struct val_result_chain *res;
    int             wcard_chk = 0;
    int             span_chk = 0;
    int             provably_unsecure = 0;
    val_status_t    status = VAL_R_DONT_KNOW;
    u_int8_t       *soa_name_n = NULL;
    u_int8_t       *closest_encounter = NULL;
    struct rrset_rec *wcard_proof = NULL;

    if (NULL == top_q)
        return;

    val_log(ctx, LOG_DEBUG, "proving non-existence for {%s, %d, %d}",
            top_q->qc_name_n, top_q->qc_class_h, top_q->qc_type_h);

    /*
     * Check if this is the whole proof and nothing but the proof
     * At this point these records should already be in the TRUSTED state.
     */

    /*
     * inspect the SOA record first 
     */
    // XXX Can we assume that the SOA record is always present?
    for (res = results;
         res && res->val_rc_trust && res->val_rc_trust->_as.ac_data;
         res = res->val_rc_next) {
        struct rrset_rec *the_set = res->val_rc_trust->_as.ac_data;
        if (the_set->rrs_ans_kind == SR_ANS_NACK_SOA) {
            soa_name_n = the_set->rrs.val_rrset_name_n;
            if (res->val_rc_status == VAL_PROVABLY_UNSECURE)
                provably_unsecure = 1;
            break;
        }
    }
    if (soa_name_n == NULL)
        status = VAL_R_INCOMPLETE_PROOF;
    else if (provably_unsecure) {
        /*
         * use the error code as status 
         */
        if (top_q->qc_as &&
            top_q->qc_as->val_ac_rrset &&
            top_q->qc_as->val_ac_rrset &&
            top_q->qc_as->val_ac_rrset->val_msg_header) {

            HEADER         *hp =
                (HEADER *) top_q->qc_as->val_ac_rrset->val_msg_header;
            if (hp->rcode == ns_r_noerror) {
                status = VAL_NONEXISTENT_TYPE;
            } else if (hp->rcode == ns_r_nxdomain) {
                status = VAL_NONEXISTENT_NAME;
            } else
                status = VAL_ERROR;
        } else
            status = VAL_ERROR;
    } else {
        int             nsec = 0;
#ifdef LIBVAL_NSEC3
        int             nsec3 = 0;
#endif
        /*
         * for every NSEC or NSEC3 proof 
         */
        for (res = results;
             res && res->val_rc_trust && res->val_rc_trust->_as.ac_data;
             res = res->val_rc_next) {
            struct rrset_rec *the_set = res->val_rc_trust->_as.ac_data;

            if (NULL == the_set->rrs.val_rrset_data) {
                status = VAL_R_BOGUS_PROOF;
                break;
            }

            if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC) {

#ifdef LIBVAL_NSEC3
                nsec = 1;
#endif
                nsec_proof_chk(ctx, the_set, top_q->qc_name_n,
                               top_q->qc_type_h, soa_name_n, &span_chk,
                               &wcard_chk, &wcard_proof,
                               &closest_encounter, &status);
                if (status != VAL_R_DONT_KNOW)
                    break;
            }
#ifdef LIBVAL_NSEC3
            else if (the_set->rrs_ans_kind == SR_ANS_NACK_NSEC3) {
                nsec3 = 1;
            }
#endif
        }


#ifdef LIBVAL_NSEC3
        /*
         * Check if we received NSEC and NSEC3 proofs 
         */
        if ((nsec && nsec3) || (!nsec && !nsec3))
            status = VAL_R_BOGUS_PROOF;
        else
#endif
        if (nsec) {
            if (!span_chk)
                status = VAL_R_INCOMPLETE_PROOF;
            else if (!wcard_chk) {
                if (!closest_encounter)
                    status = VAL_R_INCOMPLETE_PROOF;
                else
                    prove_nsec_wildcard_check(ctx, top_q->qc_type_h,
                                              wcard_proof,
                                              closest_encounter, &status);
            }
        }
#ifdef LIBVAL_NSEC3
        else if (nsec3) {
            /*
             * only nsec3 records 
             */
            nsec3_proof_chk(ctx, results, top_q->qc_name_n,
                            top_q->qc_type_h, soa_name_n, &status);
        }
#endif

        /*
         * passed all tests 
         */
        if (status == VAL_R_DONT_KNOW)
            status = VAL_NONEXISTENT_NAME;
    }

    /*
     * set the error condition in all elements of the proof 
     */
    for (res = results; res; res = res->val_rc_next)
        res->val_rc_status = status;
}


static int
verify_provably_unsecure(val_context_t * context,
                         struct val_query_chain *top_q,
                         struct val_authentication_chain *as)
{
    struct val_result_chain *results = NULL;
    char            name_p[NS_MAXDNAME];
    char            name_p_orig[NS_MAXDNAME];

    u_int8_t       *curzone_n = NULL;
    u_int8_t       *zonecut_n = NULL;

    struct rrset_rec *rrset;
    int             error = 1;

    if ((NULL == as) || (NULL == as->_as.ac_data))
        return 0;

    rrset = as->_as.ac_data;

    /*
     * save original zone name 
     */
    if (-1 == ns_name_ntop(rrset->rrs.val_rrset_name_n, name_p_orig,
                           sizeof(name_p_orig)))
        snprintf(name_p_orig, sizeof(name_p_orig), "unknown/error");

    while (error) {

        if (results != NULL) {
            val_free_result_chain(results);
            results = NULL;
        }

        /*
         * break out of possible loop 
         * got an soa from the same zone while querying for a DS 
         */

        if ((top_q->qc_type_h == ns_t_ds) &&
            !namecmp(top_q->qc_name_n, rrset->rrs.val_rrset_name_n) &&
            (as->val_ac_status == VAL_A_RRSIG_MISSING) &&
            (rrset->rrs_ans_kind == SR_ANS_NACK_SOA)) {

            if (-1 ==
                ns_name_ntop(top_q->qc_name_n, name_p, sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");
            val_log(context, LOG_DEBUG,
                    "Cannot show that zone %s is provably unsecure. \n",
                    name_p);
            return 0;
        }

        val_log(context, LOG_DEBUG, "Finding next zone cut \n");
        if ((VAL_NO_ERROR !=
             find_next_zonecut(rrset, curzone_n, &zonecut_n))
            || (zonecut_n == NULL)) {

            if (curzone_n == NULL)
                val_log(context, LOG_DEBUG, "SOA not returned");
            else if (-1 == ns_name_ntop(curzone_n, name_p, sizeof(name_p))) {
                snprintf(name_p, sizeof(name_p), "unknown/error");
                val_log(context, LOG_DEBUG, "Cannot find zone cut for %s",
                        name_p);
            }

            return 0;
        }

        if (-1 == ns_name_ntop(zonecut_n, name_p, sizeof(name_p)))
            snprintf(name_p, sizeof(name_p), "unknown/error");

        val_log(context, LOG_DEBUG,
                "About to check if %s is provably unsecure. \n", name_p);

        if ((VAL_NO_ERROR != val_resolve_and_check(context, zonecut_n,
                                                   ns_c_in, ns_t_ds, 0,
                                                   &results))
            || (results == NULL)) {

            val_log(context, LOG_DEBUG,
                    "Zone %s is not provably unsecure. \n", name_p);
            return 0;
        }

        /*
         * check new results 
         */
        if ((results->val_rc_trust == NULL) ||
            (results->val_rc_trust->val_ac_rrset == NULL)) {

            /*
             * query wasn't answered 
             */

            error = 1;
            rrset = NULL;
        } else
            error = 0;

        if (curzone_n) {
            FREE(curzone_n);
        }
        curzone_n = zonecut_n;
        zonecut_n = NULL;
    }

    /*
     * free the saved name 
     */
    if (curzone_n)
        FREE(curzone_n);

    // xxx-check: sanity check
    //     I just find it odd that we had to go and get some results to
    //     determine if the zone is provably unsecure or not, but we
    //     don't save that information anywhere for the caller/user to
    //     inspect. Maybe this function is only called from other high
    //     level routines?

    if (results->val_rc_status == VAL_SUCCESS) {
        val_log(context, LOG_DEBUG, "Zone %s is not provably unsecure. \n",
                name_p_orig);
        val_free_result_chain(results);
        return 0;
    }

    if (results->val_rc_status == VAL_NONEXISTENT_TYPE) {
        val_log(context, LOG_DEBUG, "Zone %s is provably unsecure",
                name_p_orig);
        val_free_result_chain(results);
        as->val_ac_status = VAL_A_PROVABLY_UNSECURE;
        return 1;
    }
#ifdef LIBVAL_NSEC3
    if (results->val_rc_status == VAL_NONEXISTENT_NAME_OPTOUT) {
        val_log(context, LOG_DEBUG, "Zone %s is optout provably unsecure",
                name_p_orig);
        val_free_result_chain(results);
        as->val_ac_status = VAL_A_PROVABLY_UNSECURE;
        return 1;
    }
#endif

    val_log(context, LOG_DEBUG, "Zone %s is not provably unsecure. \n",
            name_p_orig);
    val_free_result_chain(results);
    return 0;
}

/*
 * Verify an assertion if possible. Complete assertions are those for which 
 * you have data, rrsigs and key information. 
 * Returns:
 * VAL_NO_ERROR                 Operation completed successfully
 * Other return values from add_to_query_chain()
 */
static int
try_verify_assertion(val_context_t * context, struct val_query_chain *pc,
                     struct val_query_chain **queries,
                     struct val_authentication_chain *next_as)
{
    struct val_authentication_chain *pending_as;
    int             retval;
    struct rrset_rec *pending_rrset;

    /*
     * Sanity check 
     */
    if (next_as == NULL)
        return VAL_NO_ERROR;

    if (!pc)
        /*
         * If there is no pending query, we've already 
         * reached some end-state.
         */
        return VAL_NO_ERROR;

    if (NULL == queries)
        return VAL_BAD_ARGUMENT;

    if (pc->qc_state > Q_ERROR_BASE) {
        if (next_as->val_ac_status == VAL_A_WAIT_FOR_RRSIG)
            next_as->val_ac_status = VAL_A_RRSIG_MISSING;
        else if (next_as->val_ac_status == VAL_A_WAIT_FOR_TRUST) {
            /*
             * We're either waiting for DNSKEY or DS 
             */
            if (pc->qc_type_h == ns_t_ds)
                next_as->val_ac_status = VAL_A_DS_MISSING;
            else if (pc->qc_type_h == ns_t_dnskey)
                next_as->val_ac_status = VAL_A_DNSKEY_MISSING;
        }
    }

    if (pc->qc_state == Q_ANSWERED) {

        if (next_as->val_ac_status == VAL_A_WAIT_FOR_RRSIG) {

            for (pending_as = pc->qc_as; pending_as;
                 pending_as = pending_as->_as.val_ac_rrset_next) {
                /*
                 * We were waiting for the RRSIG 
                 */
                // xxx-audit: ptr deref w/out NULL check.
                //     the memcpy a few lines down dereferences
                //     pending_rrset w/out a NULL check. probably
                //     easier to handler here than there...
                pending_rrset = pending_as->_as.ac_data;

                /*
                 * Check if what we got was an RRSIG 
                 */
                if (pending_as->val_ac_status == VAL_A_BARE_RRSIG) {
                    /*
                     * Find the RRSIG that matches the type 
                     * Check if type is in the RRSIG 
                     */
                    u_int16_t       rrsig_type_n;
                    memcpy(&rrsig_type_n,
                           pending_rrset->rrs.val_rrset_sig->rr_rdata,
                           sizeof(u_int16_t));
                    // xxx-audit: ptr deref w/out NULL check (next_as->_as.ac_data)
                    if (next_as->_as.ac_data->rrs.val_rrset_type_h ==
                        ntohs(rrsig_type_n)) {
                        /*
                         * store the RRSIG in the assertion 
                         */
                        next_as->_as.ac_data->rrs.val_rrset_sig =
                            copy_rr_rec(pending_rrset->rrs.
                                        val_rrset_type_h,
                                        pending_rrset->rrs.val_rrset_sig,
                                        0);
                        next_as->val_ac_status = VAL_A_WAIT_FOR_TRUST;
                        /*
                         * create a pending query for the trust portion 
                         */
                        if (VAL_NO_ERROR !=
                            (retval =
                             build_pending_query(context, queries,
                                                 next_as)))
                            return retval;
                        break;
                    }
                }
            }
            if (pending_as == NULL) {
                /*
                 * Could not find any RRSIG matching query type
                 */
                next_as->val_ac_status = VAL_A_RRSIG_MISSING;
            }
        } else if (next_as->val_ac_status == VAL_A_WAIT_FOR_TRUST) {
            pending_as = pc->qc_as;
            next_as->val_ac_trust = pending_as;
            next_as->_as.ac_pending_query = NULL;

            // xxx-audit: ptr deref w/out NULL check (pending_as->_as.ac_data)
            if ((pending_as->_as.ac_data->rrs_ans_kind == SR_ANS_NACK_NSEC)
#ifdef LIBVAL_NSEC3
                || (pending_as->_as.ac_data->rrs_ans_kind ==
                    SR_ANS_NACK_NSEC3)
#endif
                || (pending_as->_as.ac_data->rrs_ans_kind ==
                    SR_ANS_NACK_SOA)) {

                /*
                 * proof of non-existence should follow 
                 */
                next_as->val_ac_status = VAL_A_NEGATIVE_PROOF;
            } else {
                /*
                 * XXX what if this is an SR_ANS_CNAME? Can DS or DNSKEY return a CNAME? 
                 */
                /*
                 * if the pending assertion contains a straight answer, 
                 * trust is useful for verification 
                 */
                next_as->val_ac_status = VAL_A_CAN_VERIFY;
            }
        }
    }

    if (next_as->val_ac_status == VAL_A_CAN_VERIFY) {
        val_log(context, LOG_DEBUG, "verifying next assertion");
        verify_next_assertion(context, next_as);
    }

    return VAL_NO_ERROR;
}



/*
 * Try and verify each assertion. Update results as and when they are available.
 * Do not try and validate assertions that have already been validated.
 */
static int
verify_and_validate(val_context_t * context,
                    struct val_query_chain **queries,
                    struct val_query_chain *top_q, u_int8_t flags,
                    struct val_result_chain **results, int *done)
{
    struct val_authentication_chain *next_as;
    int             retval;
    struct val_authentication_chain *as_more;
    struct val_result_chain *res;
    struct val_authentication_chain *top_as;

    if ((top_q == NULL) || (NULL == queries) || (NULL == results)
        || (NULL == done))
        return VAL_BAD_ARGUMENT;

    if ((top_as = top_q->qc_as) == NULL)
        /*
         * nothing to do 
         */
        return VAL_NO_ERROR;

    *done = 1;

    /*
     * Look at every answer that was returned 
     */
    for (as_more = top_as; as_more;
         as_more = as_more->_as.val_ac_rrset_next) {
        int             thisdone = 1;

        /*
         * If this assertion is already in the results list with a completed status
         * no need for repeating the validation process
         */
        for (res = *results; res; res = res->val_rc_next)
            if (res->val_rc_trust == as_more)
                break;
        if (res) {
            if (!CHECK_MASKED_STATUS(res->val_rc_status, VAL_R_DONT_KNOW))
                /*
                 * we've already dealt with this one 
                 */
                continue;
        } else {
            /*
             * Add this result to the list 
             */
            res =
                (struct val_result_chain *)
                MALLOC(sizeof(struct val_result_chain));
            if (res == NULL)
                return VAL_OUT_OF_MEMORY;
            res->val_rc_trust = as_more;
            res->val_rc_status = VAL_R_DONT_KNOW;
            res->val_rc_next = *results;
            *results = res;
        }

        /*
         * as_more is the next answer that we obtained; next_as is the 
         * next assertion in the chain of trust
         */
        for (next_as = as_more; next_as; next_as = next_as->val_ac_trust) {

            if (next_as->val_ac_status <= VAL_A_INIT) {

                struct val_query_chain *pc;
                pc = next_as->_as.ac_pending_query;
                // xxx-audit: ptr deref w/out NULL check (pc)
                if (pc->qc_state == Q_WAIT_FOR_GLUE) {
                    merge_glue_in_referral(pc, queries);
                }
                if (pc->qc_state > Q_ERROR_BASE)
                    next_as->val_ac_status =
                        VAL_A_DNS_ERROR_BASE + pc->qc_state - Q_ERROR_BASE;

                if (!(flags & F_DONT_VALIDATE)) {
                    /*
                     * Go up the chain of trust 
                     */
                    if (VAL_NO_ERROR !=
                        (retval =
                         try_verify_assertion(context, pc, queries,
                                              next_as)))
                        return retval;
                }
            }

            /*
             * break out of infinite loop -- trying to verify the proof of non-existence
             * for a DS record; but the DNSKEY that signs the proof is also in the 
             * chain of trust (not-validated)
             */
            if ((next_as->_as.ac_data != NULL) &&
                (next_as->_as.ac_data->rrs.val_rrset_type_h == ns_t_dnskey)
                && (next_as->val_ac_trust)
                && (next_as == next_as->val_ac_trust->val_ac_trust)) {
                res->val_rc_status = VAL_R_INDETERMINATE_DS;
                break;
            }


            /*
             * Check initial states 
             */
            if (next_as->val_ac_status <= VAL_A_INIT) {
                /*
                 * still need more data to validate this assertion 
                 */
                *done = 0;
                thisdone = 0;
            } else if (next_as->val_ac_status == VAL_A_DONT_VALIDATE) {
                break;
            } else if ((next_as->val_ac_status == VAL_A_TRUST_KEY) ||
                       (next_as->val_ac_status == VAL_A_TRUST_ZONE) ||
                       (next_as->val_ac_status ==
                        VAL_A_PROVABLY_UNSECURE)) {
                SET_RESULT_TRUSTED(res->val_rc_status);
                break;
            } else if (next_as->val_ac_status == VAL_A_NEGATIVE_PROOF) {
                /*
                 * This means that the trust point has a proof of non-existence 
                 */

                if (next_as->val_ac_trust == NULL) {
                    res->val_rc_status = VAL_R_INDETERMINATE_PROOF;
                    break;
                }

                /*
                 * We may have asked the child zone for the DS;
                 * This can only happen if the current member in
                 * the chain of trust is the DNSKEY record
                 */
                if (next_as->val_ac_rrset->val_rrset_type_h == ns_t_dnskey) {

                    int             asked_the_parent = 0;
                    struct val_authentication_chain *as;

                    /*
                     * Check if the name in the soa record is the same as the
                     * owner name of the DS record
                     */
                    for (as = next_as->val_ac_trust; as;
                         as = as->_as.val_ac_rrset_next) {
                        if ((as->val_ac_rrset != NULL)
                            && (as->val_ac_rrset->val_rrset_type_h ==
                                ns_t_soa)) {
                            if (namecmp
                                (as->val_ac_rrset->val_rrset_name_n,
                                 next_as->val_ac_rrset->val_rrset_name_n))
                                asked_the_parent = 1;
                            break;
                        }
                    }
                    if (asked_the_parent) {
                        if (verify_provably_unsecure(context, top_q, as)) {
                            res->val_rc_status = VAL_R_PROVABLY_UNSECURE;
                            SET_RESULT_TRUSTED(res->val_rc_status);
                        }
                        break;
                    }

                    /*
                     * We could only be asking the child if our default name server is 
                     * the child, so ty again starting from root; state will be WAIT_FOR_TRUST 
                     */
                    /*
                     * XXX There is an opportunity for an infinite loop here: 
                     * XXX If some nameserver actually sends a referral for the DS record
                     * XXX to the child (faulty/malicious NS) we'll keep recursing from root
                     * XXX Need to detect this case
                     */
#if 0
                    struct name_server *root_ns = NULL;
                    get_root_ns(&root_ns);
                    if (root_ns == NULL) {
                        /*
                         * No root hints configured 
                         */
                        res->val_rc_status = VAL_R_INDETERMINATE_PROOF;
                        // xxx-check: log message?
                        break;
                    } else {
                        /*
                         * send query to root 
                         */
                        next_as->val_ac_status = VAL_A_WAIT_FOR_TRUST;
                        if (VAL_NO_ERROR !=
                            (retval =
                             build_pending_query(context, queries,
                                                 next_as)))
                            return retval;
                        (*queries)->qc_ns_list = root_ns;
                        *done = 0;
                        thisdone = 0;
                    }
#else
                    res->val_rc_status = VAL_R_INDETERMINATE_PROOF;
                    break;
#endif
                } else {
                    if (verify_provably_unsecure(context, top_q, next_as)) {
                        res->val_rc_status = VAL_R_PROVABLY_UNSECURE;
                        SET_RESULT_TRUSTED(res->val_rc_status);
                    }
                    break;
                }
            }

            /*
             * Check error conditions 
             */
            else if (next_as->val_ac_status <= VAL_A_LAST_ERROR) {
                if (verify_provably_unsecure(context, top_q, next_as)) {
                    res->val_rc_status = VAL_R_PROVABLY_UNSECURE;
                    SET_RESULT_TRUSTED(res->val_rc_status);
                } else
                    res->val_rc_status = VAL_ERROR;
                break;
            } else if (next_as->val_ac_status <= VAL_A_LAST_BAD) {

                res->val_rc_status = VAL_ERROR;
                break;
            } else if (next_as->val_ac_status <= VAL_A_LAST_FAILURE) {
                /*
                 * double failures are errors 
                 */
                if (CHECK_MASKED_STATUS
                    (res->val_rc_status, VAL_R_BOGUS_UNPROVABLE)) {
                    if (verify_provably_unsecure(context, top_q, next_as)) {
                        res->val_rc_status = VAL_R_PROVABLY_UNSECURE;
                        SET_RESULT_TRUSTED(res->val_rc_status);
                    } else
                        res->val_rc_status = VAL_ERROR;
                    break;
                } else {
                    SET_MASKED_STATUS(res->val_rc_status,
                                      VAL_R_BOGUS_UNPROVABLE);
                    continue;
                }
            } else
                if (CHECK_MASKED_STATUS
                    (res->val_rc_status, VAL_R_VERIFIED_CHAIN)
                    || (res->val_rc_status == VAL_R_DONT_KNOW)) {

                /*
                 * Success condition 
                 */
                if ((next_as->val_ac_status == VAL_A_VERIFIED) ||
                    (next_as->val_ac_status == VAL_A_VERIFIED_LINK)) {
                    SET_MASKED_STATUS(res->val_rc_status,
                                      VAL_R_VERIFIED_CHAIN);
                    continue;
                } else if ((next_as->val_ac_status == VAL_A_LOCAL_ANSWER)
                           || (next_as->val_ac_status == VAL_A_TRUST_KEY)
                           || (next_as->val_ac_status == VAL_A_TRUST_ZONE)) {
                    res->val_rc_status = VAL_LOCAL_ANSWER;
                    break;
                } else if (next_as->val_ac_status == VAL_A_BARE_RRSIG) {
                    res->val_rc_status = VAL_BARE_RRSIG;
                    break;
                }
                /*
                 * unknown result 
                 */
                else if (next_as->val_ac_status == VAL_A_NO_TRUST_ANCHOR) {
                    /*
                     * verified but no trust 
                     */
                    res->val_rc_status = VAL_R_VERIFIED_CHAIN;
                    break;
                }
            }
        }
        if (!thisdone)
            /*
             * more work required 
             */
            SET_MASKED_STATUS(res->val_rc_status, VAL_R_DONT_KNOW);
    }

    return VAL_NO_ERROR;
}


// XXX Needs blocking/non-blocking logic so that the validator can operate in
// XXX the stealth mode
static int
ask_cache(val_context_t * context, u_int8_t flags,
          struct val_query_chain *end_q, struct val_query_chain **queries,
          struct val_authentication_chain **assertions, int *data_received)
{
    struct val_query_chain *next_q, *top_q;
    struct rrset_rec *next_answer;
    int             retval;
    char            name_p[NS_MAXDNAME];

    if ((queries == NULL) || (assertions == NULL)
        || (data_received == NULL))
        return VAL_BAD_ARGUMENT;

    top_q = *queries;

    for (next_q = *queries; next_q && next_q != end_q;
         next_q = next_q->qc_next) {
        if (next_q->qc_state == Q_INIT) {

            if (-1 ==
                ns_name_ntop(next_q->qc_name_n, name_p, sizeof(name_p)))
                snprintf(name_p, sizeof(name_p), "unknown/error");
            val_log(context, LOG_DEBUG,
                    "ask_cache(): looking for {%s %s(%d) %s(%d)}", name_p,
                    p_class(next_q->qc_class_h), next_q->qc_class_h,
                    p_type(next_q->qc_type_h), next_q->qc_type_h);
            if (VAL_NO_ERROR !=
                (retval =
                 get_cached_rrset(next_q->qc_name_n, next_q->qc_class_h,
                                  next_q->qc_type_h, &next_answer)))
                return retval;

            if (next_answer) {
                struct domain_info *response;

                val_log(context, LOG_DEBUG,
                        "ask_cache(): found data for {%s %d %d}", name_p,
                        next_q->qc_class_h, next_q->qc_type_h);
                *data_received = 1;

                next_q->qc_state = Q_ANSWERED;
                /*
                 * Construct a dummy response 
                 */
                response =
                    (struct domain_info *)
                    MALLOC(sizeof(struct domain_info));
                if (response == NULL) {
                    FREE(next_answer);
                    return VAL_OUT_OF_MEMORY;
                }

                response->di_rrset = next_answer;
                response->di_qnames =
                    (struct qname_chain *)
                    MALLOC(sizeof(struct qname_chain));
                if (response->di_qnames == NULL) {
                    FREE(response->di_rrset);
                    FREE(response);
                    return VAL_OUT_OF_MEMORY;
                }
                memcpy(response->di_qnames->qnc_name_n, next_q->qc_name_n,
                       wire_name_length(next_q->qc_name_n));
                response->di_qnames->qnc_next = NULL;

                if (ns_name_ntop(next_q->qc_name_n, name_p, sizeof(name_p))
                    == -1) {
                    next_q->qc_state = Q_ERROR_BASE + SR_CALL_ERROR;
                    FREE(response->di_qnames);
                    FREE(response->di_rrset);
                    FREE(response);
                    continue;
                }
                response->di_requested_name_h = name_p;
                response->di_requested_type_h = next_q->qc_type_h;
                response->di_requested_class_h = next_q->qc_class_h;
                response->di_res_error = SR_UNSET;

                retval = assimilate_answers(context, queries,
                                            response, next_q, assertions,
                                            flags);
                FREE(response->di_rrset);
                FREE(response->di_qnames);
                FREE(response);
                if (VAL_NO_ERROR != retval) {
                    return retval;
                }

                break;
            }
        }
    }

    if (top_q != *queries)
        /*
         * more queries have been added, do this again 
         */
        return ask_cache(context, flags, top_q, queries, assertions,
                         data_received);


    return VAL_NO_ERROR;
}

static int
ask_resolver(val_context_t * context, u_int8_t flags,
             struct val_query_chain **queries, int block,
             struct val_authentication_chain **assertions,
             int *data_received)
{
    struct val_query_chain *next_q;
    struct domain_info *response;
    int             retval;
    int             need_data = 0;
    char            name_p[NS_MAXDNAME];
    int             answered = 0;

    if ((queries == NULL) || (assertions == NULL)
        || (data_received == NULL))
        return VAL_BAD_ARGUMENT;

    response = NULL;

    while (!answered) {

        for (next_q = *queries; next_q; next_q = next_q->qc_next) {
            if (next_q->qc_state == Q_INIT) {
                struct name_server *ns;
                u_int8_t       *test_n;

                need_data = 1;
                next_q = next_q;        // xxx-audit: huh? assignemnt to self??
                if (-1 ==
                    ns_name_ntop(next_q->qc_name_n, name_p,
                                 sizeof(name_p)))
                    snprintf(name_p, sizeof(name_p), "unknown/error");
                val_log(context, LOG_DEBUG,
                        "ask_resolver(): sending query for {%s %d %d}",
                        name_p, next_q->qc_class_h, next_q->qc_type_h);

                if (next_q->qc_ns_list == NULL) {

                    /*
                     * See if we can get an answer from a closer NS (from cache) 
                     */
                    struct name_server *ref_ns_list;
                    int             ret_val;
                    ret_val =
                        get_matching_nslist(next_q, queries, &ref_ns_list);
                    if ((ret_val == VAL_NO_ERROR) && (ref_ns_list != NULL)) {
                        next_q->qc_ns_list = ref_ns_list;
                    } else if (context->nslist != NULL) {
                        clone_ns_list(&(next_q->qc_ns_list),
                                      context->nslist);
                        for (ns = next_q->qc_ns_list; ns; ns = ns->ns_next)
                            ns->ns_options |= RES_RECURSE;
                    } else {
                        /*
                         * work downward from root 
                         */
                        struct name_server *root_ns = NULL;
                        get_root_ns(&root_ns);
                        if (root_ns == NULL) {
                            /*
                             * No root hints; should not happen here 
                             */
                            return VAL_INTERNAL_ERROR;
                            // xxx-check: log message?
                        }
                        next_q->qc_ns_list = root_ns;
                    }
                }

                /*
                 * Only set the CD and EDS0 options if we feel the server 
                 * is capable of handling DNSSEC
                 */
                // xxx-note: the above comment isn't quite correct.
                //     this code actually checks if the zone should be
                //     trusted, not if the server can handle DNSSEC.
                if (next_q->qc_zonecut_n != NULL)
                    test_n = next_q->qc_zonecut_n;
                else
                    test_n = next_q->qc_name_n;

                if (!(flags & F_DONT_VALIDATE) &&
                    (is_trusted_zone(context, test_n) ==
                     VAL_A_WAIT_FOR_TRUST)) {

                    val_log(context, LOG_DEBUG,
                            "Setting D0 bit and using EDNS0");

                    for (ns = next_q->qc_ns_list; ns; ns = ns->ns_next)
                        ns->ns_options |= RES_USE_DNSSEC;
                } else {
                    val_log(context, LOG_DEBUG,
                            "Not setting D0 bit nor using EDNS0");
                }

                if ((retval =
                     val_resquery_send(context, next_q)) != VAL_NO_ERROR)
                    return retval;

                next_q->qc_state = Q_SENT;
            } else if (next_q->qc_state < Q_ANSWERED)
                need_data = 1;
        }

        /*
         * wait until we get at least one complete answer 
         */
        if ((block) && need_data) {

            for (next_q = *queries; next_q; next_q = next_q->qc_next) {
                if (next_q->qc_state < Q_ANSWERED) {
                    if ((retval =
                         val_resquery_rcv(context, next_q, &response,
                                          queries)) != VAL_NO_ERROR)
                        return retval;

                    if ((next_q->qc_state == Q_ANSWERED)
                        && (response != NULL)) {
                        if (-1 ==
                            ns_name_ntop(next_q->qc_name_n, name_p,
                                         sizeof(name_p)))
                            snprintf(name_p, sizeof(name_p),
                                     "unknown/error");
                        val_log(context, LOG_DEBUG,
                                "ask_resolver(): found data for {%s %d %d}",
                                name_p, next_q->qc_class_h,
                                next_q->qc_type_h);
                        if (VAL_NO_ERROR !=
                            (retval =
                             assimilate_answers(context, queries, response,
                                                next_q, assertions,
                                                flags))) {
                            free_domain_info_ptrs(response);
                            FREE(response);
                            return retval;
                        }

                        /*
                         * Save new responses in the cache 
                         */
                        if (VAL_NO_ERROR !=
                            (retval = stow_answer(response->di_rrset))) {
                            free_domain_info_ptrs(response);
                            FREE(response);
                            return retval;
                        }

                        response->di_rrset = NULL;
                        free_domain_info_ptrs(response);
                        FREE(response);
                        answered = 1;
                        break;
                    }
                    if (response != NULL) {
                        free_domain_info_ptrs(response);
                        FREE(response);
                    }
                    if ((next_q->qc_state == Q_WAIT_FOR_GLUE) ||
                        (next_q->qc_referral != NULL)) {
                        answered = 1;
                        /*
                         * Check if we fetched this same glue before and it was answered 
                         */
                        if (next_q->qc_referral->glueptr &&
                            (next_q->qc_referral->glueptr->qc_state ==
                             Q_ANSWERED)) {
                            merge_glue_in_referral(next_q, queries);
                            *data_received = 1;
                        }
                        break;
                    }
                    if (next_q->qc_state >= Q_ANSWERED) {
                        answered = 1;
                        *data_received = 1;
                        break;
                    }
                }
            }
        } else
            break;
    }

    return VAL_NO_ERROR;
}

int
clone_result_assertions(struct val_result_chain *results)
{
    struct val_result_chain *res;

    for (res = results; res && res->val_rc_trust; res = res->val_rc_next) {

        struct val_authentication_chain *n_ac, *o_ac, *head_ac, *prev_ac;

        head_ac = NULL;
        prev_ac = NULL;

        for (o_ac = res->val_rc_trust; o_ac; o_ac = o_ac->val_ac_trust) {

            n_ac = (struct val_authentication_chain *)
                MALLOC(sizeof(struct val_authentication_chain));
            if (n_ac == NULL)
                return VAL_OUT_OF_MEMORY;
            memset(n_ac, 0, sizeof(struct val_authentication_chain));
            n_ac->val_ac_status = o_ac->val_ac_status;
            n_ac->val_ac_trust = NULL;

            if (o_ac->val_ac_rrset != NULL) {
                int             len;

                n_ac->val_ac_rrset =
                    (struct val_rrset *) MALLOC(sizeof(struct val_rrset));
                if (n_ac->val_ac_rrset == NULL)
                    return VAL_OUT_OF_MEMORY;
                memset(n_ac->val_ac_rrset, 0, sizeof(struct val_rrset));

                // xxx- bug 1537734: potential memory leak
                //     not just in this loop iteration, but previous as well.
                //     iterate over head_ac & preforms frees?
                CLONE_NAME_LEN(o_ac->val_ac_rrset->val_msg_header,
                               o_ac->val_ac_rrset->val_msg_headerlen,
                               n_ac->val_ac_rrset->val_msg_header,
                               n_ac->val_ac_rrset->val_msg_headerlen);

                len =
                    wire_name_length(o_ac->val_ac_rrset->val_rrset_name_n);
                n_ac->val_ac_rrset->val_rrset_name_n =
                    (u_int8_t *) MALLOC(len * sizeof(u_int8_t));
                // xxx-audit: memory leak, no release of prior allocs before return
                //     not just in this loop iteration, but previous as well.
                //     iterate over head_ac & preforms frees?
                if (n_ac->val_ac_rrset->val_rrset_name_n == NULL)
                    return VAL_OUT_OF_MEMORY;
                memcpy(n_ac->val_ac_rrset->val_rrset_name_n,
                       o_ac->val_ac_rrset->val_rrset_name_n, len);

                n_ac->val_ac_rrset->val_rrset_class_h =
                    o_ac->val_ac_rrset->val_rrset_class_h;
                n_ac->val_ac_rrset->val_rrset_type_h =
                    o_ac->val_ac_rrset->val_rrset_type_h;
                n_ac->val_ac_rrset->val_rrset_ttl_h =
                    o_ac->val_ac_rrset->val_rrset_ttl_h;
                n_ac->val_ac_rrset->val_rrset_section =
                    o_ac->val_ac_rrset->val_rrset_section;
                n_ac->val_ac_rrset->val_rrset_data =
                    copy_rr_rec(n_ac->val_ac_rrset->val_rrset_type_h,
                                o_ac->val_ac_rrset->val_rrset_data, 0);
                n_ac->val_ac_rrset->val_rrset_sig =
                    copy_rr_rec(n_ac->val_ac_rrset->val_rrset_type_h,
                                o_ac->val_ac_rrset->val_rrset_sig, 0);
            }

            if (head_ac == NULL) {
                head_ac = n_ac;
                prev_ac = head_ac;
            } else {
                prev_ac->val_ac_trust = n_ac;
                prev_ac = n_ac;
            }
        }
        // xxx-audit: huh? huge memory leak?
        //     so we just clones the entire val_rc_trust, and now
        //     replace original w/clone. who frees original?
        res->val_rc_trust = head_ac;
    }

    return VAL_NO_ERROR;
}

void
fix_validation_results(val_context_t * context,
                       struct val_result_chain *results,
                       struct val_query_chain *top_q)
{
    struct val_result_chain *res;
    int             partially_wrong = 0;
    int             negative_proof = 0;

    for (res = results; res; res = res->val_rc_next) {

        /*
         * Fix validation results 
         */
        /*
         * Some error most likely, reflected in the val_query_chain 
         */
        if (res->val_rc_trust == NULL)
            res->val_rc_status = VAL_ERROR;

        /*
         *  Special case of provably unsecure: algorithms used
         *  for signing the DNSKEY record are not understood
         */
        if (res->val_rc_status == VAL_R_BOGUS_PROVABLE) {
            /*
             * implies that the trust flag is set 
             */
            struct val_authentication_chain *as;
            for (as = res->val_rc_trust; as; as = as->val_ac_trust) {
                if ((as->val_ac_rrset) &&
                    (as->val_ac_rrset->val_rrset_type_h == ns_t_dnskey)) {
                    if (as->val_ac_status == VAL_A_UNKNOWN_ALGO) {
                        res->val_rc_status = VAL_R_PROVABLY_UNSECURE;
                        SET_RESULT_TRUSTED(res->val_rc_status);
                        break;
                    }
                }
            }
        }

        if (res->val_rc_status == (VAL_R_DONT_KNOW | VAL_R_TRUST_FLAG))
            res->val_rc_status = VAL_SUCCESS;

        val_log(context, LOG_DEBUG, "validate result set to %s[%d]",
                p_val_error(res->val_rc_status), res->val_rc_status);

        if ((res->val_rc_status != VAL_SUCCESS) &&
            (res->val_rc_status != VAL_PROVABLY_UNSECURE))
            partially_wrong = 1;

        if ((res->val_rc_trust) && (res->val_rc_trust->_as.ac_data != NULL)
            &&
            ((res->val_rc_trust->_as.ac_data->rrs_ans_kind ==
              SR_ANS_NACK_NSEC) ||
#ifdef LIBVAL_NSEC3
             (res->val_rc_trust->_as.ac_data->rrs_ans_kind ==
              SR_ANS_NACK_NSEC3) ||
#endif
             (res->val_rc_trust->_as.ac_data->rrs_ans_kind ==
              SR_ANS_NACK_SOA)))
            negative_proof = 1;
    }

    if (negative_proof) {
        int             asked_the_child = 0;
        struct val_authentication_chain *as;

        if ((top_q != NULL) && (top_q->qc_type_h == ns_t_ds)) {
            /*
             * If we've asked for a DS and the soa has the same 
             * name, we've actually asked the child zone
             */
            for (res = results; res; res = res->val_rc_next) {
                as = res->val_rc_trust;
                if (as->val_ac_rrset->val_rrset_type_h == ns_t_soa) {
                    if (!namecmp(as->val_ac_rrset->val_rrset_name_n,
                                 top_q->qc_name_n))
                        asked_the_child = 1;
                    break;
                }
            }
        }

        if ((asked_the_child) || (partially_wrong)) {

            /*
             * mark all answers as bogus - 
             * all answers are related in the proof 
             */
            val_log(context, LOG_DEBUG, "Bogus Proof");
            for (res = results; res; res = res->val_rc_next)
                res->val_rc_status = VAL_R_BOGUS_PROOF;
        } else
            prove_nonexistence(context, top_q, results);
    }
}


/*
 * Look inside the cache, ask the resolver for missing data.
 * Then try and validate what ever is possible.
 * Return when we are ready with some useful answer (error condition is 
 * a useful answer)
 */
int
val_resolve_and_check(val_context_t * ctx,
                      u_char * domain_name_n,
                      const u_int16_t q_class,
                      const u_int16_t type,
                      const u_int8_t flags,
                      struct val_result_chain **results)
{

    int             retval;
    struct val_query_chain *top_q;
    char            block = 1;  /* block until at least some data is returned */
    char            name_p[NS_MAXDNAME];

    int             done = 0;
    int             data_received = 0;

    val_context_t  *context = NULL;

    if ((results == NULL) || (domain_name_n == NULL))
        return VAL_BAD_ARGUMENT;

    *results = NULL;

    /*
     * Create a default context if one does not exist 
     */
    if (ctx == NULL) {
        if (VAL_NO_ERROR != (retval = val_create_context(NULL, &context)))
            return retval;
    } else
        context = (val_context_t *) ctx;

    if (-1 == ns_name_ntop(domain_name_n, name_p, sizeof(name_p)))
        snprintf(name_p, sizeof(name_p), "unknown/error");
    val_log(context, LOG_DEBUG,
            "val_resolve_and_check(): looking for {%s %d %d}", name_p,
            q_class, type);

    if (VAL_NO_ERROR !=
        (retval =
         add_to_query_chain(&(context->q_list), domain_name_n, type,
                            q_class)))
        goto err;

    top_q = context->q_list;

    while (!done) {

        struct val_query_chain *last_q;

        /*
         * keep track of the last entry added to the query chain 
         */
        last_q = context->q_list;

        /*
         * Data might already be present in the cache 
         */
        /*
         * XXX by-pass this functionality through flags if needed 
         */
        if (VAL_NO_ERROR !=
            (retval =
             ask_cache(context, flags, NULL, &(context->q_list),
                       &(context->a_list), &data_received)))
            goto err;
        if (data_received)
            block = 0;

        /*
         * Send un-sent queries 
         */
        if (VAL_NO_ERROR !=
            (retval =
             ask_resolver(context, flags, &(context->q_list), block,
                          &(context->a_list), &data_received)))
            goto err;

        /*
         * check if more queries have been added 
         */
        if (last_q != context->q_list) {
            /*
             * There are new queries to send out -- do this first; 
             * we may also find this data in the cache 
             */
            block = 0;
            continue;
        }

        /*
         * Henceforth we will need some data before we can continue 
         */
        block = 1;

        if (top_q->qc_state == Q_WAIT_FOR_GLUE)
            merge_glue_in_referral(top_q, &(context->q_list));

        if ((!data_received) && (top_q->qc_state < Q_ANSWERED))
            continue;

        /*
         * No point going ahead if our original query had error conditions 
         */
        if (top_q->qc_state > Q_ERROR_BASE) {
            /*
             * the original query had some error 
             */
            *results =
                (struct val_result_chain *)
                MALLOC(sizeof(struct val_result_chain));
            if ((*results) == NULL) {
                retval = VAL_OUT_OF_MEMORY;
                goto err;
            }
            (*results)->val_rc_trust = top_q->qc_as;
            (*results)->val_rc_status =
                VAL_DNS_ERROR_BASE + top_q->qc_state - Q_ERROR_BASE;
            (*results)->val_rc_next = NULL;

            break;
        }

        /*
         * Answer will be digested 
         */
        data_received = 0;

        /*
         * We have sufficient data to at least perform some validation --
         * validate what ever is possible. 
         */
        if (VAL_NO_ERROR !=
            (retval =
             verify_and_validate(context, &(context->q_list), top_q, flags,
                                 results, &done)))
            goto err;
    }

    if (results) {

        if (!(flags & F_DONT_VALIDATE))
            fix_validation_results(context, *results, top_q);

        /*
         * Clone the required assertion list elements, so that 
         * context can be free'd up if necessary
         */
        if (VAL_NO_ERROR != (retval = clone_result_assertions(*results)))
            return retval;

    }

    retval = VAL_NO_ERROR;

  err:
    if ((ctx == NULL) && context) {
        val_free_context(context);
    }

    return retval;
}

/*
 * Function: val_isauthentic
 *
 * Purpose:   Tells whether the given validation status code represents an
 *            authentic response from the validator
 *
 * Parameter: val_status -- a validation status code returned by the validator
 *
 * Returns:   1 if the validation status represents an authentic response
 *            0 if the validation status does not represent an authentic response
 *
 * See also: val_istrusted()
 */
int
val_isauthentic(val_status_t val_status)
{
    switch (val_status) {
    case VAL_SUCCESS:
    case VAL_NONEXISTENT_NAME:
    case VAL_NONEXISTENT_TYPE:
        return 1;

    default:
        return 0;
    }
}

/*
 * Function: val_istrusted
 *
 * Purpose:   Tells whether the given validation status code represents an
 *            answer that can be trusted.  An answer can be trusted if it
 *            has been obtained locally (for example from /etc/hosts) or if
 *            it was an authentic response from the validator.
 *
 * Parameter: val_status -- a validation status code returned by the validator
 *
 * Returns:   1 if the validation status represents a trusted response
 *            0 if the validation status does not represent a trusted response
 *
 * See also: val_isauthentic()
 */
int
val_istrusted(val_status_t val_status)
{
    if ((val_status == VAL_LOCAL_ANSWER) || val_isauthentic(val_status)) {
        return 1;
    } else {
        return 0;
    }
}
