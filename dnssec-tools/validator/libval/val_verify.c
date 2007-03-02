/*
 * Copyright 2005 SPARTA, Inc.  All rights reserved.
 * See the COPYING file distributed with this software for details.
 *
 * Author: Abhijit Hayatnagarkar
 *
 * This is the implementation file for the verifier.
 */
#include "validator-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>

#include <validator/resolver.h>
#include <validator/validator.h>
#include "val_support.h"
#include "val_cache.h"
#include "val_verify.h"
#include "val_crypto.h"


#define ZONE_KEY_FLAG 0x0100    /* Zone Key Flag, RFC 4034 */
#define BUFLEN 8192

/*
 * Verify a signature, given the data and the dnskey 
 * Pass in a context, to give acceptable time skew 
 */
static void
val_sigverify(val_context_t * ctx,
              const unsigned char *data,
              int data_len,
              const val_dnskey_rdata_t * dnskey,
              const val_rrsig_rdata_t * rrsig,
              val_astatus_t * dnskey_status, val_astatus_t * sig_status)
{
    struct timeval  tv;
    struct timezone tz;

    if (dnskey == NULL) {
        *dnskey_status = VAL_AC_INVALID_KEY;
        return;
    }

    if (rrsig == NULL) {
        *sig_status = VAL_AC_INVALID_RRSIG;
        return;
    }

    if ((data == NULL) && (data_len != 0)) {
        /*
         * assertion status should be VAL_AC_DATA_MISSING 
         */
        *sig_status = VAL_AC_INVALID_RRSIG;
        return;
    }

    /*
     * Check if the dnskey is a zone key 
     */
    if ((dnskey->flags & ZONE_KEY_FLAG) == 0) {
        val_log(ctx, LOG_DEBUG, "DNSKEY not a zone signing key");
        *dnskey_status = VAL_AC_INVALID_KEY;
        return;
    }

    /*
     * Check dnskey protocol value 
     */
    if (dnskey->protocol != 3) {
        val_log(ctx, LOG_DEBUG,
                "Invalid protocol field in DNSKEY record: %d",
                dnskey->protocol);
        *dnskey_status = VAL_AC_UNKNOWN_DNSKEY_PROTOCOL;
        return;
    }

    /*
     * Match dnskey and rrsig algorithms 
     */
    if (dnskey->algorithm != rrsig->algorithm) {
        val_log(ctx, LOG_DEBUG,
                "Algorithm mismatch between DNSKEY (%d) and RRSIG (%d) records.",
                dnskey->algorithm, rrsig->algorithm);
        *sig_status = VAL_AC_RRSIG_ALGORITHM_MISMATCH;
        return;
    }

    /*
     * Check signature inception and expiration times 
     */
    gettimeofday(&tv, &tz);
    if (tv.tv_sec < rrsig->sig_incp) {
        if (tv.tv_sec < rrsig->sig_incp - SIG_ACCEPT_WINDOW) {
            char            currTime[1028];
            char            incpTime[1028];
            int             len;
            bzero(currTime, 1028);
            bzero(incpTime, 1028);
#ifndef sun
            ctime_r((const time_t *) (&(tv.tv_sec)), currTime);
#else
            ctime_r((const time_t *) (&(tv.tv_sec)), currTime,
                    sizeof(currTime));
#endif
            len = strlen(currTime);
            if (len > 0)
                currTime[len - 1] = 0;
#ifndef sun
            ctime_r((const time_t *) (&(rrsig->sig_incp)), incpTime);
#else
            ctime_r((const time_t *) (&(tv.tv_sec)), incpTime,
                    sizeof(incpTime));
#endif
            len = strlen(incpTime);
            if (len > 0)
                incpTime[len - 1] = 0;
            val_log(ctx, LOG_DEBUG,
                    "Signature not yet valid. Current time (%s) is less than signature inception time (%s).",
                    currTime, incpTime);
            *sig_status = VAL_AC_RRSIG_NOTYETACTIVE;
            return;
        } else {
            val_log(ctx, LOG_WARNING,
                    "Signature not yet valid, but within acceptable skew.");
        }

    }

    if (tv.tv_sec > rrsig->sig_expr) {
        if (tv.tv_sec > rrsig->sig_expr + SIG_ACCEPT_WINDOW) {
            char            currTime[1028];
            char            exprTime[1028];
            int             len;
            bzero(currTime, 1028);
            bzero(exprTime, 1028);
#ifndef sun
            ctime_r((const time_t *) (&(tv.tv_sec)), currTime);
#else
            ctime_r((const time_t *) (&(tv.tv_sec)), currTime,
                    sizeof(currTime));
#endif
            len = strlen(currTime);
            if (len > 0)
                currTime[len - 1] = 0;
#ifndef sun
            ctime_r((const time_t *) (&(rrsig->sig_expr)), exprTime);
#else
            ctime_r((const time_t *) (&(tv.tv_sec)), exprTime,
                    sizeof(exprTime));
#endif
            len = strlen(exprTime);
            if (len > 0)
                exprTime[len - 1] = 0;
            val_log(ctx, LOG_DEBUG,
                    "Signature expired. Current time (%s) is greater than signature expiration time (%s).",
                    currTime, exprTime);
            *sig_status = VAL_AC_RRSIG_EXPIRED;
            return;
        } else {
            val_log(ctx, LOG_WARNING,
                    "Signature expired, but within acceptable skew.");
        }
    }

    switch (rrsig->algorithm) {

    case ALG_RSAMD5:
        rsamd5_sigverify(ctx, data, data_len, dnskey, rrsig, dnskey_status,
                         sig_status);
        return;

#ifdef LIBVAL_NSEC3
    case ALG_NSEC3_DSASHA1:
#endif
    case ALG_DSASHA1:
        dsasha1_sigverify(ctx, data, data_len, dnskey, rrsig,
                          dnskey_status, sig_status);
        return;

#ifdef LIBVAL_NSEC3
    case ALG_NSEC3_RSASHA1:
#endif
    case ALG_RSASHA1:
        rsasha1_sigverify(ctx, data, data_len, dnskey, rrsig,
                          dnskey_status, sig_status);
        return;

    case ALG_DH:
        val_log(ctx, LOG_DEBUG, "Unsupported algorithm %d.",
                rrsig->algorithm);
        *sig_status = VAL_AC_ALGORITHM_NOT_SUPPORTED;
        *dnskey_status = VAL_AC_ALGORITHM_NOT_SUPPORTED;
        break;

    default:
        val_log(ctx, LOG_DEBUG, "Unknown algorithm %d.", rrsig->algorithm);
        *sig_status = VAL_AC_UNKNOWN_ALGORITHM;
        *dnskey_status = VAL_AC_UNKNOWN_ALGORITHM;
        break;
    }
}

#if 0                           /* never used, so why compile it in? */
/*
 * returns the number of bytes that were put into rrBuf 
 * Concatenate the rrset into a buffer 
 * Assume canonical ordering of RRs in the rrset 
 */
static int
val_concat_rrset(struct rrset_rec *rrset,
                 const unsigned int orig_ttl,
                 unsigned char *rrBuf, int orig_rrBuf_len)
{

    int             rrBuf_len = 0;
    struct rr_rec  *rr = NULL;
    unsigned char  *cp;

    /*
     * Assume that elements of the rrs->val_rrset_data list are in canonical form 
     */
    /*
     * sort the rrs->val_rrset_rdata by bubble-sort 
     */
    int             sorted = 0;
    while (!sorted) {
        struct rr_rec  *first_rr = NULL, *prev_rr = NULL, *curr_rr1 =
            NULL, *curr_rr2 = NULL, *next_rr = NULL;
        sorted = 1;
        curr_rr1 = rrset->rrs.val_rrset_data;
        first_rr = curr_rr1;

        if (curr_rr1)
            curr_rr2 = curr_rr1->rr_next;
        while (curr_rr2 != NULL) {

            int             cmp_len =
                (curr_rr1->rr_rdata_length_h <
                 curr_rr2->rr_rdata_length_h) ? curr_rr1->
                rr_rdata_length_h : curr_rr2->rr_rdata_length_h;

            next_rr = curr_rr2->rr_next;
            int             cmp_res =
                memcmp(curr_rr1->rr_rdata, curr_rr2->rr_rdata, cmp_len);
            if ((cmp_res > 0)
                || ((cmp_res == 0)
                    && (curr_rr2->rr_rdata_length_h >
                        curr_rr1->rr_rdata_length_h))) {
                /*
                 * switch rrs 
                 */
                struct rr_rec  *tmp_rr = NULL;
                sorted = 0;
                curr_rr1->rr_next = next_rr;
                curr_rr2->rr_next = curr_rr1;
                if (prev_rr) {
                    prev_rr->rr_next = curr_rr2;
                } else {
                    first_rr = curr_rr2;
                }
                tmp_rr = curr_rr2;
                curr_rr2 = curr_rr1;
                curr_rr1 = tmp_rr;
            }

            prev_rr = curr_rr1;
            curr_rr1 = curr_rr2;
            curr_rr2 = curr_rr2->rr_next;
        }
        rrset->rrs.val_rrset_data = first_rr;
    }

    rr = rrset->rrs.val_rrset_data;
    while (rr) {
        memcpy(rrBuf + rrBuf_len, rrset->rrs.val_rrset_name_n,
               strlen((char *) rrset->rrs.val_rrset_name_n) + 1);
        rrBuf_len += strlen((char *) rrset->rrs.val_rrset_name_n) + 1;

        cp = rrBuf + rrBuf_len;
        NS_PUT16(rrset->rrs.val_rrset_type_h, cp);
        rrBuf_len += 2;

        NS_PUT16(rrset->rrs.val_rrset_class_h, cp);
        rrBuf_len += 2;

        /*
         * Put the original ttl 
         */
        NS_PUT32(orig_ttl, cp);
        rrBuf_len += 4;

        NS_PUT16(rr->rr_rdata_length_h, cp);
        rrBuf_len += 2;

        memcpy(rrBuf + rrBuf_len, rr->rr_rdata, rr->rr_rdata_length_h);
        rrBuf_len += rr->rr_rdata_length_h;

        rr = rr->rr_next;
    }
    return rrBuf_len;
}

/*
 * tells whether the response contain any rrsigs 
 */
static int
have_rrsigs(struct domain_info *response)
{
    struct rrset_rec *rrset;

    if (!response) {
        return 0;
    }

    rrset = response->di_answers;
    while (rrset) {
        struct rr_rec  *rrs_sig = rrset->rrs_sig;
        while (rrs_sig) {
            val_rrsig_rdata_t rrsig_rdata;
            bzero(&rrsig_rdata, sizeof(rrsig_rdata));
            val_parse_rrsig_rdata(rrs_sig->rr_rdata,
                                  rrs_sig->rr_rdata_length_h,
                                  &rrsig_rdata);
            if ((rrsig_rdata.type_covered == rrset->rrs.val_rrset_type_h)
                ||
#ifdef LIBVAL_NSEC3
                (rrsig_rdata.type_covered == ns_t_nsec3) ||
#endif
                (rrsig_rdata.type_covered == ns_t_nsec)) {
                if (rrsig_rdata.signature != NULL)
                    FREE(rrsig_rdata.signature);
                return 1;
            }
            if (rrsig_rdata.signature != NULL)
                FREE(rrsig_rdata.signature);
            rrs_sig = rrs_sig->rr_next;
        }
        rrset = rrset->rrs_next;
    }

    return 0;
}
#endif                          /* 0 */


static int
predict_sigbuflength(struct rrset_rec *rr_set,
                     size_t * field_length, int *signer_length)
{
    /*
     * Calculate the size of the field over which the verification
     * is done.  This is the sum of
     * the number of bytes through the signer name in the SIG RDATA
     * the length of the signer name (uncompressed)
     * the sum of the fully uncompressed lengths of the RRs in the set
     * *field_length is the field length
     * *signer_length is the length of the signer's name (used externally)
     */
    struct rr_rec  *rr;
    int             owner_length;

    if ((rr_set == NULL) || (field_length == NULL)
        || (signer_length == NULL) || (rr_set->rrs.val_rrset_sig == NULL)
        || (rr_set->rrs.val_rrset_sig->rr_rdata == NULL))
        return VAL_BAD_ARGUMENT;

    owner_length = wire_name_length(rr_set->rrs.val_rrset_name_n);

    *signer_length =
        wire_name_length(&rr_set->rrs.val_rrset_sig->rr_rdata[SIGNBY]);

    if (*signer_length == 0)
        return VAL_INTERNAL_ERROR;

    *field_length = SIGNBY + (*signer_length);

    for (rr = rr_set->rrs.val_rrset_data; rr; rr = rr->rr_next)
        *field_length += owner_length + ENVELOPE + rr->rr_rdata_length_h;

    return VAL_NO_ERROR;
}

static int
make_sigfield(u_int8_t ** field,
              size_t * field_length,
              struct rrset_rec *rr_set,
              struct rr_rec *rr_sig, int is_a_wildcard)
{
    struct rr_rec  *curr_rr;
    int             index;
    int             signer_length;
    int             owner_length;
    u_int16_t       type_n;
    u_int16_t       class_n;
    u_int32_t       ttl_n;
    u_int16_t       rdata_length_n;
    u_int8_t        lowered_owner_n[NS_MAXCDNAME];
    size_t          l_index;

    if ((field == NULL) || (field_length == NULL) || (rr_set == NULL) ||
        (rr_sig == NULL))
        return VAL_BAD_ARGUMENT;

    if (predict_sigbuflength(rr_set, field_length, &signer_length) !=
        VAL_NO_ERROR)
        return VAL_INTERNAL_ERROR;

    *field = (u_int8_t *) MALLOC(*field_length);

    if (*field == NULL)
        return VAL_OUT_OF_MEMORY;

    /*
     * Make sure we are using the correct TTL 
     */

    memcpy(&ttl_n, &rr_sig->rr_rdata[TTL], sizeof(u_int32_t));
    rr_set->rrs.val_rrset_ttl_h = ntohl(ttl_n);

    /*
     * While we're at it, we'll gather other common info, specifically
     * network ordered numbers (type, class) and name length.
     */

    owner_length = wire_name_length(rr_set->rrs.val_rrset_name_n);

    if (owner_length == 0)
        goto err;

    memcpy(lowered_owner_n, rr_set->rrs.val_rrset_name_n, owner_length);
    l_index = 0;
    lower_name(lowered_owner_n, &l_index);

    type_n = htons(rr_set->rrs.val_rrset_type_h);
    class_n = htons(rr_set->rrs.val_rrset_class_h);

    /*
     * Copy in the SIG RDATA (up to the signature 
     */

    index = 0;
    if ((index + SIGNBY + signer_length) > *field_length)
        goto err;
    memcpy(&(*field)[index], rr_sig->rr_rdata, SIGNBY + signer_length);
    index += SIGNBY + signer_length;

    /*
     * For each record of data, copy in the envelope & the lower cased rdata 
     */

    for (curr_rr = rr_set->rrs.val_rrset_data; curr_rr;
         curr_rr = curr_rr->rr_next) {
        if (curr_rr->rr_rdata == NULL)
            goto err;

        /*
         * Copy in the envelope information 
         */

        if (is_a_wildcard) {
            /*
             * Construct the original name 
             */
            u_char          wcard_n[NS_MAXCDNAME];
            u_int8_t       *np = lowered_owner_n;
            int             i;
            int             outer_len;

            for (i = 0; i < is_a_wildcard; i++)
                np += np[0] + 1;
            outer_len = wire_name_length(np);

            wcard_n[0] = (u_int8_t) 1;
            wcard_n[1] = '*';
            if ((outer_len + 2) > sizeof(wcard_n))
                goto err;
            memcpy(&wcard_n[2], np, outer_len);
            if ((index + outer_len + 2) > *field_length)
                goto err;
            memcpy(&(*field)[index], wcard_n, outer_len + 2);
            index += outer_len + 2;
        } else {
            if ((index + owner_length) > *field_length)
                goto err;
            memcpy(&(*field)[index], lowered_owner_n, owner_length);
            index += owner_length;
        }

        if ((index + sizeof(u_int16_t) + sizeof(u_int16_t) +
             sizeof(u_int32_t))
            > *field_length)
            goto err;
        memcpy(&(*field)[index], &type_n, sizeof(u_int16_t));
        index += sizeof(u_int16_t);
        memcpy(&(*field)[index], &class_n, sizeof(u_int16_t));
        index += sizeof(u_int16_t);
        memcpy(&(*field)[index], &ttl_n, sizeof(u_int32_t));
        index += sizeof(u_int32_t);

        /*
         * Now the RR-specific info, the length and the data 
         */

        rdata_length_n = htons(curr_rr->rr_rdata_length_h);
        if ((index + sizeof(u_int16_t) + curr_rr->rr_rdata_length_h)
            > *field_length)
            goto err;
        memcpy(&(*field)[index], &rdata_length_n, sizeof(u_int16_t));
        index += sizeof(u_int16_t);
        memcpy(&(*field)[index], curr_rr->rr_rdata,
               curr_rr->rr_rdata_length_h);
        index += curr_rr->rr_rdata_length_h;
    }

    *field_length = index;
    return VAL_NO_ERROR;

  err:
    FREE(*field);
    *field = NULL;
    *field_length = 0;
    return VAL_INTERNAL_ERROR;
}

#if 0                           /* never used, so why compile it in? */
static int
find_signature(u_int8_t ** field, struct rr_rec *rr_sig)
{
    int             sig_index;

    sig_index = SIGNBY + wire_name_length(&rr_sig->rr_rdata[SIGNBY]);

    *field = &rr_sig->rr_rdata[sig_index];

    return rr_sig->rr_rdata_length_h - sig_index;
}
#endif

static void
identify_key_from_sig(struct rr_rec *sig, u_int8_t ** name_n,
                      u_int16_t * footprint_n)
{
    if ((sig == NULL) || (sig->rr_rdata == NULL) || (name_n == NULL) ||
        (footprint_n == NULL) || (sig->rr_rdata_length_h < SIGNBY)) {
        if (name_n != NULL)
            *name_n = NULL;
        if (footprint_n != NULL)
            memset(footprint_n, 0, sizeof(u_int16_t));
        return;
    }

    *name_n = &sig->rr_rdata[SIGNBY];
    memcpy(footprint_n, &sig->rr_rdata[SIGNBY - sizeof(u_int16_t)],
           sizeof(u_int16_t));
}


static void
do_verify(val_context_t * ctx,
          val_astatus_t * dnskey_status,
          val_astatus_t * sig_status,
          struct rrset_rec *the_set,
          struct rr_rec *the_sig,
          val_dnskey_rdata_t * the_key, int is_a_wildcard)
{
    /*
     * Use the crypto routines to verify the signature
     */

    u_int8_t       *ver_field;
    size_t          ver_length;
    //   u_int8_t            *sig_field;
    //   size_t              sig_length;
    int             ret_val;
    val_rrsig_rdata_t rrsig_rdata;

    *dnskey_status = VAL_AC_UNSET;
    *sig_status = VAL_AC_UNSET;

    /*
     * Wildcard expansions for DNSKEYs and DSs are not permitted
     */
    if (is_a_wildcard &&
        ((the_set->rrs.val_rrset_type_h == ns_t_ds) ||
         (the_set->rrs.val_rrset_type_h == ns_t_dnskey))) {
        *dnskey_status = VAL_AC_INVALID_KEY;
        return;
    }

    if ((ret_val = make_sigfield(&ver_field, &ver_length, the_set, the_sig,
                                 is_a_wildcard)) != VAL_NO_ERROR) {
        *sig_status = VAL_AC_INVALID_RRSIG;
        return;
    }

    /*
     * Find the signature - no memory is malloc'ed for this operation  
     */

    //  sig_length = find_signature (&sig_field, the_sig);
    //      val_parse_rrsig_rdata(sig_field, sig_length, &rrsig_rdata);
    val_parse_rrsig_rdata(the_sig->rr_rdata, the_sig->rr_rdata_length_h,
                          &rrsig_rdata);
    rrsig_rdata.next = NULL;

    /*
     * Perform the verification 
     */
    val_sigverify(ctx, ver_field, ver_length, the_key,
                  &rrsig_rdata, dnskey_status, sig_status);

    if (rrsig_rdata.signature != NULL) {
        FREE(rrsig_rdata.signature);
        rrsig_rdata.signature = NULL;
    }

    FREE(ver_field);
    return;
}

static int
ds_hash_is_equal(u_int8_t ds_hashtype, u_int8_t * ds_hash,
                 u_int32_t ds_hash_len, u_int8_t * name_n,
                 struct rr_rec *dnskey, val_astatus_t * ds_status)
{
    /*
     * Only SHA-1 is understood 
     */
    if (ds_hashtype != ALG_DS_HASH_SHA1) {
        *ds_status = VAL_AC_UNKNOWN_ALGORITHM;
        return 0;
    }

    if ((dnskey == NULL) || (ds_hash == NULL) || (name_n == NULL)
        || (ds_hash_len != SHA_DIGEST_LENGTH))
        return 0;

    return ds_sha_hash_is_equal(name_n, dnskey->rr_rdata,
                                dnskey->rr_rdata_length_h, ds_hash);
}

/*
 * State returned in as->val_ac_status is one of:
 * VAL_AC_VERIFIED : at least one sig passed
 * VAL_AC_NOT_VERIFIED : multiple errors
 * the exact error
 */

#define SET_STATUS(savedstatus, rr, newstatus) \
	do { \
		rr->rr_status = newstatus; \
        /* Any success is good */\
		if (newstatus == VAL_AC_RRSIG_VERIFIED) \
            savedstatus = VAL_AC_VERIFIED;\
        else if (newstatus == VAL_AC_WCARD_VERIFIED)\
            savedstatus = VAL_AC_WCARD_VERIFIED;\
        /* we don't already have success and what we receive is bad */ \
        else if ((savedstatus != VAL_AC_VERIFIED) && \
                 (savedstatus != VAL_AC_WCARD_VERIFIED) &&\
                 (newstatus != VAL_AC_UNSET) &&\
                 /* success values for DNSKEYS are not relevant */\
                 (newstatus != VAL_AC_SIGNING_KEY) && \
                 (newstatus != VAL_AC_UNKNOWN_ALGORITHM_LINK) && \
                 (newstatus != VAL_AC_VERIFIED_LINK)){\
            savedstatus = VAL_AC_NOT_VERIFIED; \
        }\
        /* else leave savedstatus untouched */\
	} while (0)

void
verify_next_assertion(val_context_t * ctx,
                      struct val_digested_auth_chain *as,
                      struct val_digested_auth_chain *the_trust)
{
    struct rrset_rec *the_set;
    struct rr_rec  *the_sig;
    u_int8_t       *signby_name_n;
    u_int16_t       signby_footprint_n;
    val_dnskey_rdata_t dnskey;
    int             is_a_wildcard;
    struct rr_rec  *nextrr;
    struct rr_rec  *keyrr;
    u_int16_t       tag_h;

    if ((as == NULL) || (as->_as.ac_data == NULL) || (the_trust == NULL))
        return;

    as->val_ac_status = VAL_AC_UNSET;
    the_set = as->_as.ac_data;
    dnskey.public_key = NULL;

    if (the_set->rrs.val_rrset_sig == NULL) {
        as->val_ac_status = VAL_AC_RRSIG_MISSING;
        return;
    }

    for (the_sig = the_set->rrs.val_rrset_sig;
         the_sig; the_sig = the_sig->rr_next) {

        /*
         * do wildcard processing 
         */
        if (check_label_count(the_set, the_sig, &is_a_wildcard) !=
            VAL_NO_ERROR) {
            SET_STATUS(as->val_ac_status, the_sig,
                       VAL_AC_WRONG_LABEL_COUNT);
            continue;
        }

        /*
         * for each sig, identify key, 
         */
        identify_key_from_sig(the_sig, &signby_name_n,
                              &signby_footprint_n);

        if (the_set->rrs.val_rrset_type_h != ns_t_dnskey) {
            /*
             * trust path contains the key 
             */
            if (the_trust->_as.ac_data == NULL) {
                SET_STATUS(as->val_ac_status, the_sig,
                           VAL_AC_DNSKEY_NOMATCH);
                continue;
            }
            keyrr = the_trust->_as.ac_data->rrs.val_rrset_data;
        } else {
            /*
             * data itself contains the key 
             */
            if (the_set->rrs.val_rrset_data == NULL) {
                SET_STATUS(as->val_ac_status, the_sig,
                           VAL_AC_DNSKEY_NOMATCH);
                continue;
            }
            keyrr = the_set->rrs.val_rrset_data;
        }

        tag_h = ntohs(signby_footprint_n);
        for (nextrr = keyrr; nextrr; nextrr = nextrr->rr_next) {
            if (-1 == val_parse_dnskey_rdata(nextrr->rr_rdata,
                                             nextrr->rr_rdata_length_h,
                                             &dnskey)) {
                SET_STATUS(as->val_ac_status, nextrr, VAL_AC_INVALID_KEY);
                continue;
            }

            dnskey.next = NULL;
            if (dnskey.key_tag != tag_h) {
                if (dnskey.public_key != NULL) {
                    FREE(dnskey.public_key);
                    dnskey.public_key = NULL;
                }
                continue;
            }

            /*
             * check the signature 
             */
            do_verify(ctx, &nextrr->rr_status,
                      &the_sig->rr_status,
                      the_set, the_sig, &dnskey, is_a_wildcard);


            if (the_sig->rr_status == VAL_AC_RRSIG_VERIFIED) {
                SET_STATUS(as->val_ac_status, nextrr, VAL_AC_SIGNING_KEY);
                if (is_a_wildcard)
                    the_sig->rr_status = VAL_AC_WCARD_VERIFIED;

                SET_STATUS(as->val_ac_status, the_sig, the_sig->rr_status);
                SET_STATUS(as->val_ac_status, nextrr, nextrr->rr_status);
                break;
            }

            /*
             * There might be multiple keys with the same key tag; set this as
             * the signing key only if we dont have other status for this key
             */
            if (as->val_ac_status == VAL_AC_UNSET) {
                SET_STATUS(as->val_ac_status, nextrr, VAL_AC_SIGNING_KEY);
            }

            SET_STATUS(as->val_ac_status, the_sig, the_sig->rr_status);
            SET_STATUS(as->val_ac_status, nextrr, nextrr->rr_status);

            if (dnskey.public_key != NULL) {
                FREE(dnskey.public_key);
            }
            dnskey.public_key = NULL;
        }

        /*
         * If this record contains a DNSKEY, check if the DS record contains this key 
         * DNSKEYs cannot be wildcard expanded, so VAL_AC_WCARD_VERIFIED does not
         * count as a good sig
         * Create the link even if the DNSKEY algorithm is unknown since this 
         * may be the provably unsecure case
         */
        if (nextrr &&           /* also means that there is a valid dnskey */
            the_set->rrs.val_rrset_type_h == ns_t_dnskey &&
            (the_sig->rr_status == VAL_AC_RRSIG_VERIFIED ||
             the_sig->rr_status == VAL_AC_UNKNOWN_ALGORITHM)) {
            /*
             * follow the trust path 
             */
            struct rr_rec  *dsrec =
                the_trust->_as.ac_data->rrs.val_rrset_data;
            keyrr = nextrr;

            while (dsrec) {
                val_ds_rdata_t  ds;
                val_parse_ds_rdata(dsrec->rr_rdata,
                                   dsrec->rr_rdata_length_h, &ds);

                if (dnskey.key_tag == ds.d_keytag &&
                    ds.d_algo == dnskey.algorithm &&
                    ds_hash_is_equal(ds.d_type,
                                     ds.d_hash, ds.d_hash_len,
                                     the_set->rrs.val_rrset_name_n,
                                     nextrr, &dsrec->rr_status)) {

                    if (the_sig->rr_status == VAL_AC_RRSIG_VERIFIED)
                        SET_STATUS(as->val_ac_status, nextrr,
                                   VAL_AC_VERIFIED_LINK);
                    else
                        SET_STATUS(as->val_ac_status, nextrr,
                                   VAL_AC_UNKNOWN_ALGORITHM_LINK);

                    FREE(ds.d_hash);
                    if (dnskey.public_key) {
                        FREE(dnskey.public_key);
                    }
                    /*
                     * the first match is enough 
                     */
                    return;
                }

                SET_STATUS(as->val_ac_status, dsrec, dsrec->rr_status);
                FREE(ds.d_hash);
                ds.d_hash = NULL;
                dsrec = dsrec->rr_next;
            }
        }

        if (dnskey.public_key != NULL) {
            FREE(dnskey.public_key);
            dnskey.public_key = NULL;
        }
    }

    /*
     * Didn't find a valid entry in the DS record set 
     */
    if (the_set->rrs.val_rrset_type_h == ns_t_dnskey) {
        for (the_sig = the_set->rrs.val_rrset_sig;
             the_sig; the_sig = the_sig->rr_next) {
            SET_STATUS(as->val_ac_status, the_sig, VAL_AC_BAD_DELEGATION);
        }
    }
}
