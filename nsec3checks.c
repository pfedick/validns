/*
 * Part of DNS zone file validator `validns`.
 *
 * Copyright 2011, Anton Berezin <tobez@tobez.org>
 * Modified BSD license.
 * (See LICENSE file in the distribution.)
 *
 */

#include <ctype.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <Judy.h>

#include "common.h"
#include "textparse.h"
#include "mempool.h"
#include "carp.h"
#include "rr.h"
#include "base32hex.h"

static struct binary_data name2hash(char *name, struct rr *param)
{
    struct rr_nsec3param *p = (struct rr_nsec3param *)param;
	EVP_MD_CTX ctx;
	unsigned char md0[EVP_MAX_MD_SIZE];
	unsigned char md1[EVP_MAX_MD_SIZE];
	unsigned char *md[2];
	int mdi = 0;
	struct binary_data r = bad_binary_data();
	struct binary_data wire_name = name2wire_name(name);
	int i;
	int digest_size;

	md[0] = md0;
	md[1] = md1;
	if (wire_name.length < 0)
		return r;

	/* XXX Maybe use Init_ex and Final_ex for speed? */

	EVP_MD_CTX_init(&ctx);
	if (EVP_DigestInit(&ctx, EVP_sha1()) != 1)
		return r;
	digest_size = EVP_MD_CTX_size(&ctx);
	EVP_DigestUpdate(&ctx, wire_name.data, wire_name.length);
	EVP_DigestUpdate(&ctx, p->salt.data, p->salt.length);
	EVP_DigestFinal(&ctx, md[mdi], NULL);

	for (i = 0; i < p->iterations; i++) {
		if (EVP_DigestInit(&ctx, EVP_sha1()) != 1)
			return r;
		EVP_DigestUpdate(&ctx, md[mdi], digest_size);
		mdi = (mdi + 1) % 2;
		EVP_DigestUpdate(&ctx, p->salt.data, p->salt.length);
		EVP_DigestFinal(&ctx, md[mdi], NULL);
	}

	r.length = digest_size;
	r.data = getmem(digest_size);
	memcpy(r.data, md[mdi], digest_size);
	return r;
}

int sorted_hashed_names_count;
struct binary_data *sorted_hashed_names;
void *nsec3_hash;

static void *verify_record_types(struct named_rr *named_rr, struct rr_nsec3 *rr)
{
	int type;
	char *base;
	int i, k;
	struct rr_set *set;
	uint32_t nsec3_distinct_types = 0;
	uint32_t real_distinct_types;

	base = rr->type_bitmap.data;
	while (base - rr->type_bitmap.data < rr->type_bitmap.length) {
		for (i = 0; i < base[1]; i++) {
			for (k = 0; k <= 7; k++) {
				if (base[2+i] & (0x80 >> k)) {
					type = base[0]*256 + i*8 + k;
					nsec3_distinct_types++;
					set = find_rr_set_in_named_rr(named_rr, type);
					if (!set) {
						return moan(rr->rr.file_name, rr->rr.line,
									"NSEC3 mentions %s, but no such record found for %s",
									rdtype2str(type), named_rr->name);
					}
				}
			}
		}
		base += base[1]+2;
	}
	real_distinct_types = get_rr_set_count(named_rr);
	if (real_distinct_types > nsec3_distinct_types) {
		return moan(rr->rr.file_name, rr->rr.line,
					"there are more record types than NSEC3 mentions for %s",
					named_rr->name);
	}
	return rr;
}

void calculate_hashed_names(void)
{
	unsigned char sorted_name[512];
	struct named_rr **named_rr_p;
	struct named_rr *named_rr;
	void *x = name2hash;
	x = name2hash;
	uint32_t mask;

	sorted_hashed_names_count = 0;
	mask = NAME_FLAG_NOT_AUTHORITATIVE|NAME_FLAG_NSEC3_ONLY|NAME_FLAG_KIDS_WITH_RECORDS;
	if (G.nsec3_opt_out_present) {
		mask |= NAME_FLAG_DELEGATION;
	}

	sorted_name[0] = 0;
	JSLF(named_rr_p, zone_data, sorted_name);
	while (named_rr_p) {
		named_rr = *named_rr_p;
		if ((named_rr->flags & mask) == NAME_FLAG_KIDS_WITH_RECORDS) {
			struct binary_data hash;
			struct rr_nsec3 **nsec3_slot;
			struct rr_nsec3 *nsec3;

			hash = name2hash(named_rr->name, nsec3param);
			if (hash.length < 0) {
				moan(named_rr->file_name, named_rr->line, "internal: cannot calculate hashed name");
				goto next;
			}
			if (hash.length != 20)
				croak(4, "assertion failed: wrong hashed name size %d", hash.length);
			JHSG(nsec3_slot, nsec3_hash, hash.data, hash.length);
			if (nsec3_slot == PJERR)
				croak(5, "calculate_hashed_names: JHSG failed");
			if (!nsec3_slot) {
				moan(named_rr->file_name, named_rr->line,
					 "no corresponding NSEC3 found for %s",
					 named_rr->name);
				goto next;
			}
			nsec3 = *nsec3_slot;
			if (!nsec3)
				croak(6, "assertion failed: existing nsec3 from hash is empty");
			nsec3->corresponding_name = named_rr;
			sorted_hashed_names_count++;
			verify_record_types(named_rr, nsec3);
		}
next:
		JSLN(named_rr_p, zone_data, sorted_name);
	}
}

void *remember_nsec3(char *name, struct rr_nsec3 *rr)
{
	char hashed_name[33];
	char binary_hashed_name[20];
	int l;
	struct rr_nsec3 **nsec3_slot;

	l = strlen(name);
	if (l < 33 || name[32] != '.')
		return bitch("NSEC3 record name is not valid");
	if (l == 33 && zone_apex_l != 1)  /* root zone */
		return bitch("NSEC3 record name is not valid");
	if (l > 33 && strcmp(name+33, zone_apex) != 0)
		return bitch("NSEC3 record name is not valid");

	memcpy(hashed_name, name, 32);  hashed_name[32] = 0;
	l = decode_base32hex(binary_hashed_name, hashed_name, 20);
	if (l != 20)
		return bitch("NSEC3 record name is not valid");
	JHSI(nsec3_slot, nsec3_hash, binary_hashed_name, 20);
	if (nsec3_slot == PJERR)
		croak(2, "remember_nsec3: JHSI failed");
	if (*nsec3_slot)
		return bitch("multiple NSEC3 with the same record name");
	*nsec3_slot = rr;
	return rr;
}