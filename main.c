/*
 * Part of DNS zone file validator `validns`.
 *
 * Copyright 2011-2014 Anton Berezin <tobez@tobez.org>
 * Modified BSD license.
 * (See LICENSE file in the distribution.)
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libgen.h>

#include "common.h"
#include "carp.h"
#include "mempool.h"
#include "textparse.h"
#include "rr.h"

struct globals G;
struct file_info *file_info = NULL;

int read_zone_file(void);
void open_zone_file(char *fname);

static char *process_directive(char *s)
{
	char *d = s+1;
	if (*(s+1) == 'O' && strncmp(s, "$ORIGIN", 7) == 0) {
		char *o;
		s += 7;
		if (!isspace(*s)) {
			if (isalnum(*s)) goto unrecognized_directive;
			return bitch("bad $ORIGIN format");
		}
		s = skip_white_space(s);
		o = extract_name(&s, "$ORIGIN value", 0);
		if (!o) {
			return NULL;
		}
		if (*s) {
			return bitch("garbage after valid $ORIGIN directive");
		}
		file_info->current_origin = o;
		if (G.opt.verbose) {
			fprintf(stderr, "-> %s:%d: ", file_info->name, file_info->line);
			fprintf(stderr, "origin is now %s\n", o);
		}
	} else if (*(s+1) == 'T' && strncmp(s, "$TTL", 4) == 0) {
		s += 4;
		if (!isspace(*s)) {
			if (isalnum(*s)) goto unrecognized_directive;
			return bitch("bad $TTL format");
		}
		s = skip_white_space(s);
		G.default_ttl = extract_timevalue(&s, "$TTL value");
		if (G.default_ttl < 0) {
			return NULL;
		}
		if (*s) {
			return bitch("garbage after valid $TTL directive");
		}
		if (G.opt.verbose) {
			fprintf(stderr, "-> %s:%d: ", file_info->name, file_info->line);
			fprintf(stderr, "default ttl is now %ld\n", G.default_ttl);
		}
	} else if (*(s+1) == 'I' && strncmp(s, "$INCLUDE", 8) == 0) {
		char *p, *f;
		char c;
		s += 8;
		if (!isspace(*s)) {
			if (isalnum(*s)) goto unrecognized_directive;
			return bitch("bad $INCLUDE format");
		}
		s = skip_white_space(s);
		p = s;
		while (*s && !isspace(*s) && *s != ';')
			s++;
		c = *s;
		*s = '\0';
		if (!*p) {
			return bitch("$INCLUDE directive with empty file name");
		}
		f = quickstrdup_temp(p);
		*s = c;
		s = skip_white_space(s);

		if (*s) {
			return bitch("garbage after valid $INCLUDE directive");
		}
		if (*f == '/') {
			open_zone_file(f);
		} else {
			char buf[1024];

			snprintf(buf, 1024, "%s/%s", G.opt.include_path, f);
			open_zone_file(buf);
		}
	} else {
unrecognized_directive:
		s = d-1;
		while (isalnum(*d))	d++;
		*d = '\0';
		return bitch("unrecognized directive: %s", s);
	}
	return s;
}

int
read_zone_file(void)
{
	char *s;
	char *name = NULL, *class, *rdtype;
	long ttl = -1;
	while (file_info) {
		while (fgets(file_info->buf, 2048, file_info->file)) {
			freeall_temp();
			file_info->line++;
			file_info->paren_mode = 0;
			rdtype = NULL;
			if (empty_line_or_comment(file_info->buf))
				continue;

			s = file_info->buf;
			if (!isspace(*s)) {
				/* <domain-name>, $INCLUDE, $ORIGIN */
				if (*s == '$') {
					process_directive(s);
					continue;
				} else {
					/* <domain-name> */
					name = extract_name(&s, "record name", 0);
					if (!name)
						continue;
				}
			} else {
				s = skip_white_space(s);
			}
			if (!s)
				continue;
			if (!name) {
				bitch("cannot assume previous name for it is not known");
				continue;
			}
			if (G.default_ttl >= 0)
				ttl = G.default_ttl;
			if (isdigit(*s)) {
				ttl = extract_timevalue(&s, "TTL");
				if (ttl < 0)
					continue;
				class = extract_label(&s, "class or type", "temporary");
				if (!class)
					continue;
				if (*class == 'i' && *(class+1) == 'n' && *(class+2) == 0) {
				} else if (*class == 'c' && *(class+1) == 's' && *(class+2) == 0) {
					bitch("CSNET class is not supported");
					continue;
				} else if (*class == 'c' && *(class+1) == 'h' && *(class+2) == 0) {
					bitch("CHAOS class is not supported");
					continue;
				} else if (*class == 'h' && *(class+1) == 's' && *(class+2) == 0) {
					bitch("HESIOD class is not supported");
					continue;
				} else {
					rdtype = class;
				}
			} else {
				class = extract_label(&s, "class or type", "temporary");
				if (!class)
					continue;
				if (*class == 'i' && *(class+1) == 'n' && *(class+2) == 0) {
					if (isdigit(*s)) {
						ttl = extract_timevalue(&s, "TTL");
						if (ttl < 0)
							continue;
					}
				} else if (*class == 'c' && *(class+1) == 's' && *(class+2) == 0) {
					bitch("CSNET class is not supported");
					continue;
				} else if (*class == 'c' && *(class+1) == 'h' && *(class+2) == 0) {
					bitch("CHAOS class is not supported");
					continue;
				} else if (*class == 'h' && *(class+1) == 's' && *(class+2) == 0) {
					bitch("HESIOD class is not supported");
					continue;
				} else {
					rdtype = class;
				}
			}
			if (!rdtype) {
				rdtype = extract_label(&s, "type", "temporary");
			}
			if (!rdtype) {
				continue;
			}
			if (ttl < 0) {
				ttl = G.default_ttl;
			}
			if (ttl < 0) {
				bitch("ttl not specified and default is not known");
				continue;
			}

			{
				int is_generic;
				int type = str2rdtype(rdtype, &is_generic);
				if (type <= 0) continue;
				if (is_generic)
					rr_parse_any(name, ttl, type, s);
				else if (type > T_MAX)
					rr_parse_any(name, ttl, type, s);
				else if (rr_methods[type].rr_parse)
					rr_methods[type].rr_parse(name, ttl, type, s);
				else
					rr_parse_any(name, ttl, type, s);
			}
		}
		if (ferror(file_info->file))
			croak(1, "read error for %s", file_info->name);
		file_info = file_info->next;
	}
	return 0;
}

void
open_zone_file(char *fname)
{
	FILE *f;
	struct file_info *new_file_info;

	if (strcmp(fname, "-") == 0) {
		f = stdin;
		fname = "stdin";
	} else {
		f = fopen(fname, "r");
		if (!file_info && !G.opt.include_path_specified) {
			G.opt.include_path = quickstrdup(dirname(quickstrdup_temp(fname)));
		}
	}
	if (!f)
		croak(1, "open %s", fname);
	new_file_info = malloc(sizeof(*new_file_info) + strlen(fname) + 1);
	if (!new_file_info)
		croak(1, "malloc(file_info), %s", fname);
	new_file_info->next = file_info;
	new_file_info->file = f;
	new_file_info->line = 0;
	strcpy(new_file_info->name, fname);
	if (file_info) {
		new_file_info->current_origin = file_info->current_origin;
	} else {
		new_file_info->current_origin = G.opt.first_origin;
	}
	file_info = new_file_info;
}

void usage(char *err)
{
	if (err)
		fprintf(stderr, "%s\n", err);
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "    %s -h\n", thisprogname());
	fprintf(stderr, "    %s [options] zone-file\n", thisprogname());
	fprintf(stderr, "Usage parameters:\n");
	fprintf(stderr, "\t-h\t\tproduce usage text and quit\n");
	fprintf(stderr, "\t-f\t\tquit on first validation error\n");

	fprintf(stderr, "\t-p name\tperform policy check <name>\n");
	fprintf(stderr, "\t\t\tsingle-ns\n");
	fprintf(stderr, "\t\t\tcname-other-data\n");
	fprintf(stderr, "\t\t\tdname\n");
	fprintf(stderr, "\t\t\tnsec3param-not-apex\n");
	fprintf(stderr, "\t\t\tmx-alias\n");
	fprintf(stderr, "\t\t\tns-alias\n");
	fprintf(stderr, "\t\t\trp-txt-exists\n");
	fprintf(stderr, "\t\t\ttlsa-host\n");
	fprintf(stderr, "\t\t\tksk-exists\n");
	fprintf(stderr, "\t\t\tnsec3-consistency\n");
	fprintf(stderr, "\t\t\tds-requires-ns\n");
	fprintf(stderr, "\t\t\tall\n");

	fprintf(stderr, "\t-n N\t\tuse N worker threads\n");
	fprintf(stderr, "\t-q\t\tquiet - do not produce any output\n");
	fprintf(stderr, "\t-s\t\tprint validation summary/stats\n");
	fprintf(stderr, "\t-v\t\tbe extra verbose\n");
	fprintf(stderr, "\t-I path\tuse this path for $INCLUDE files\n");
	fprintf(stderr, "\t-z origin\tuse this origin as initial $ORIGIN\n");
	fprintf(stderr, "\t-t epoch-time\tuse this time instead of \"now\"\n");
	exit(1);
}

struct rr_methods rr_methods[T_MAX+1];

static void initialize_globals(void)
{
	int i;

	setenv("TZ", "GMT0", 1);	tzset();
	memset(&G, 0, sizeof(G));
	memset(&G.opt, 0, sizeof(G.opt));
	memset(&G.stats, 0, sizeof(G.stats));
	G.default_ttl = -1; /* XXX orly? */
	G.opt.times_to_check[0] = time(NULL);
	G.opt.n_times_to_check = 0;
	G.opt.include_path = ".";

	for (i = 0; i <= T_MAX; i++) {
		rr_methods[i] = unknown_methods;
	}
	rr_methods[T_AAAA]         =       aaaa_methods;
	rr_methods[T_A]            =          a_methods;
	rr_methods[T_AFSDB]        =      afsdb_methods;
	rr_methods[T_CERT]         =       cert_methods;
	rr_methods[T_CNAME]        =      cname_methods;
	rr_methods[T_DHCID]        =      dhcid_methods;
	rr_methods[T_DLV]          =        dlv_methods;
	rr_methods[T_DNAME]        =      dname_methods;
	rr_methods[T_DNSKEY]       =     dnskey_methods;
	rr_methods[T_DS]           =         ds_methods;
	rr_methods[T_HINFO]        =      hinfo_methods;
	rr_methods[T_IPSECKEY]     =   ipseckey_methods;
	rr_methods[T_ISDN]         =       isdn_methods;
	rr_methods[T_KX]           =         kx_methods;
	rr_methods[T_L32]          =        l32_methods;
	rr_methods[T_L64]          =        l64_methods;
	rr_methods[T_LOC]          =        loc_methods;
	rr_methods[T_LP]           =         lp_methods;
	rr_methods[T_MB]           =         mb_methods;
	rr_methods[T_MG]           =         mg_methods;
	rr_methods[T_MINFO]        =      minfo_methods;
	rr_methods[T_MR]           =         mr_methods;
	rr_methods[T_MX]           =         mx_methods;
	rr_methods[T_NAPTR]        =      naptr_methods;
	rr_methods[T_NID]          =        nid_methods;
	rr_methods[T_NSAP]         =       nsap_methods;
	rr_methods[T_NSEC3PARAM]   = nsec3param_methods;
	rr_methods[T_NSEC3]        =      nsec3_methods;
	rr_methods[T_NSEC]         =       nsec_methods;
	rr_methods[T_NS]           =         ns_methods;
	rr_methods[T_PTR]          =        ptr_methods;
	rr_methods[T_PX]           =         px_methods;
	rr_methods[T_RP]           =         rp_methods;
	rr_methods[T_RT]           =         rt_methods;
	rr_methods[T_RRSIG]        =      rrsig_methods;
	rr_methods[T_SOA]          =        soa_methods;
	rr_methods[T_SPF]          =        spf_methods;
	rr_methods[T_SRV]          =        srv_methods;
	rr_methods[T_SSHFP]        =      sshfp_methods;
	rr_methods[T_TLSA]         =       tlsa_methods;
	rr_methods[T_TXT]          =        txt_methods;
	rr_methods[T_X25]          =        x25_methods;
}

int
main(int argc, char **argv)
{
	int o;
	struct timeval start, stop;

	initialize_globals();
	while ((o = getopt(argc, argv, "fhqsvI:z:t:p:n:")) != -1) {
		switch(o) {
		case 'h':
			usage(NULL);
			break;
		case 'f':
			G.opt.die_on_first_error = 1;
			break;
		case 'q':
			G.opt.no_output = 1;
			break;
		case 's':
			G.opt.summary = 1;
			break;
		case 'v':
			G.opt.verbose = 1;
			break;
		case 'p':
			if (strcmp(optarg, "all") == 0) {
				int i;
				for (i = 0; i < N_POLICY_CHECKS; i++) {
					G.opt.policy_checks[i] = 1;
				}
			} else if (strcmp(optarg, "single-ns") == 0) {
				G.opt.policy_checks[POLICY_SINGLE_NS] = 1;
			} else if (strcmp(optarg, "cname-other-data") == 0) {
				G.opt.policy_checks[POLICY_CNAME_OTHER_DATA] = 1;
			} else if (strcmp(optarg, "dname") == 0) {
				G.opt.policy_checks[POLICY_DNAME] = 1;
			} else if (strcmp(optarg, "dnskey") == 0) {
				G.opt.policy_checks[POLICY_DNSKEY] = 1;
			} else if (strcmp(optarg, "nsec3param-not-apex") == 0) {
				G.opt.policy_checks[POLICY_NSEC3PARAM_NOT_APEX] = 1;
			} else if (strcmp(optarg, "mx-alias") == 0) {
				G.opt.policy_checks[POLICY_MX_ALIAS] = 1;
			} else if (strcmp(optarg, "ns-alias") == 0) {
				G.opt.policy_checks[POLICY_NS_ALIAS] = 1;
			} else if (strcmp(optarg, "rp-txt-exists") == 0) {
				G.opt.policy_checks[POLICY_RP_TXT_EXISTS] = 1;
			} else if (strcmp(optarg, "tlsa-host") == 0) {
				G.opt.policy_checks[POLICY_TLSA_HOST] = 1;
			} else if (strcmp(optarg, "ksk-exists") == 0) {
				G.opt.policy_checks[POLICY_KSK_EXISTS] = 1;
			} else if (strcmp(optarg, "nsec3-consistency") == 0) {
				G.opt.policy_checks[POLICY_NSEC3_CONSISTENCY] = 1;		
			} else if (strcmp(optarg, "ds-requires-ns") == 0) {
				G.opt.policy_checks[POLICY_DS_REQUIRES_NS] = 1;
			} else {
				usage("unknown policy name");
			}
			break;
		case 'I':
			G.opt.include_path = optarg;
			G.opt.include_path_specified = 1;
			break;
		case 'z':
			if (strlen(optarg) && *(optarg+strlen(optarg)-1) == '.') {
				G.opt.first_origin = optarg;
			} else if (strlen(optarg)) {
				G.opt.first_origin = getmem(strlen(optarg)+2);
				strcpy(mystpcpy(G.opt.first_origin, optarg), ".");
			} else {
				usage("origin must not be empty");
			}
			break;
		case 'n':
			G.opt.n_threads = strtol(optarg, NULL, 10);
			if (G.opt.n_threads > 256)
				usage("non-sensical number of threads requested");
			if (G.opt.verbose)
				fprintf(stderr, "using %d worker threads\n", G.opt.n_threads);
			break;
		case 't':
			if (G.opt.n_times_to_check >= MAX_TIMES_TO_CHECK)
				usage("too many -t specified");
			G.opt.times_to_check[G.opt.n_times_to_check++] = strtol(optarg, NULL, 10);
			break;
		default:
			usage(NULL);
		}
	}
	if (G.opt.n_times_to_check <= 0)
		G.opt.n_times_to_check = 1;
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage(NULL);
	gettimeofday(&start, NULL);
	open_zone_file(argv[0]);
	read_zone_file();
	validate_zone();
	verify_all_keys();
	if (G.nsec3_present) {
		if (first_nsec3) nsec3_validate(&first_nsec3->rr);
		perform_remaining_nsec3checks();
		if (G.opt.policy_checks[POLICY_NSEC3_CONSISTENCY]) nsec3_consistency_policy_check();
	}
	if (G.dnssec_active && G.opt.policy_checks[POLICY_KSK_EXISTS]) {
		dnskey_ksk_policy_check();
	}
	if (G.dnssec_active && G.opt.policy_checks[POLICY_DS_REQUIRES_NS]) {
		ds_requires_ns_policy_check();
	}

	gettimeofday(&stop, NULL);
	if (G.opt.summary) {
		printf("records found:       %d\n", G.stats.rr_count);
		printf("skipped dups:        %d\n", G.stats.skipped_dup_rr_count);
		printf("record sets found:   %d\n", G.stats.rrset_count);
		printf("unique names found:  %d\n", G.stats.names_count);
		printf("delegations found:   %d\n", G.stats.delegations);
		printf("    nsec3 records:   %d\n", G.stats.nsec3_count);
		/* "not authoritative names" - non-empty terminals without any authoritative records */
		/* delegation points count as authoritative, which might or might not be correct */
		printf("not authoritative names, not counting delegation points:\n"
			   "                     %d\n", G.stats.not_authoritative);
		printf("validation errors:   %d\n", G.stats.error_count);
		printf("signatures verified: %d\n", G.stats.signatures_verified);

		printf ("record count by type:\n");
		int i;
		for (i=0;i<MAX_RR_COUNT_ID;i++) {
			if (G.stats.rr_count_by_type[i]>0) {
				printf("    %10s: %d\n",
						rdtype2str(i),
						G.stats.rr_count_by_type[i]);
			}
		}
		printf("time taken:          %.3fs\n",
			   stop.tv_sec - start.tv_sec + (stop.tv_usec - start.tv_usec)/1000000.);
	}
	return G.exit_code;
}
