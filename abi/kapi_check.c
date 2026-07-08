// SPDX-License-Identifier: GPL-2.0
/*
 * kapi_check - boot-time ABI preflight for gh_hugepage_reserve.
 *
 * Reads the running kernel's BTF (/sys/kernel/btf/vmlinux), looks up each symbol
 * the module resolves, canonicalizes its real signature, and compares it against
 * the expected signature for this kernel version (from kapi_abi.gen.h, generated
 * from kapi_abi.tsv). Prints one machine-readable line to stdout:
 *
 *     disable=folio_isolate_lru,reclaim_pages        (comma list, may be empty)
 *
 * which post-fs-data.sh feeds to `insmod ... disable_kapi=<list>` so a
 * signature-incompatible symbol is left NULL instead of being called through a
 * mistyped pointer (kCFI panic). Per-symbol diagnostics go to stderr.
 *
 * A second machine-readable line carries the running kernel's MIGRATE_CMA
 * enumerator value (enum migratetype from BTF; -1 when absent, e.g. CONFIG_CMA=n):
 *
 *     migrate_cma=4
 *
 * which post-fs-data.sh feeds to `insmod ... migrate_cma_val=<N>`. The module
 * cannot use its build-time MIGRATE_CMA: the enumerator's position shifts with
 * the running kernel's config (CONFIG_MEMORY_ISOLATION etc.), and a wrong value
 * would label pageblocks with someone else's migratetype.
 *
 * Types are compared as DESUGARED canonical tokens (typedef/__bitwise
 * transparent), mirroring kCFI: gfp_t/acr_flags_t -> uint. A semantic value
 * change under an unchanged canonical type (6.18 migratetype->acr_flags) is
 * invisible here by design - the module's CONTIG_RANGE version wrapper owns it.
 *
 * Build: aarch64 static, see Makefile. Must run from a root domain that can read
 * /sys/kernel/btf/vmlinux (Magisk/KernelSU); the plain shell domain cannot.
 */
#define KAPI_ABI_WANT_TABLE
#include "kapi_abi.gen.h"

#include <bpf/btf.h>
#include <linux/btf.h>
#include <sys/utsname.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BTF_KIND_TYPE_TAG
#define BTF_KIND_TYPE_TAG 18
#endif

/* Canonicalize a BTF type to a token matching kapi_abi.tsv's spelling.
 * @pointee: true when naming a pointer's pointee - keep typedef names there
 * (e.g. nodemask_t) rather than desugaring, matching how the table is written. */
static void canon(const struct btf *btf, __u32 tid, char *out, size_t osz, int pointee)
{
	const struct btf_type *t;
	__u32 kind;

	if (tid == 0) { snprintf(out, osz, "void"); return; }
	t = btf__type_by_id(btf, tid);
	if (!t) { snprintf(out, osz, "?"); return; }
	kind = BTF_INFO_KIND(t->info);

	/* strip cv-quals + type_tags always; strip typedef only at top level */
	while (kind == BTF_KIND_CONST || kind == BTF_KIND_VOLATILE ||
	       kind == BTF_KIND_RESTRICT || kind == BTF_KIND_TYPE_TAG ||
	       (kind == BTF_KIND_TYPEDEF && !pointee)) {
		tid = t->type;
		if (tid == 0) { snprintf(out, osz, "void"); return; }
		t = btf__type_by_id(btf, tid);
		if (!t) { snprintf(out, osz, "?"); return; }
		kind = BTF_INFO_KIND(t->info);
	}

	switch (kind) {
	case BTF_KIND_TYPEDEF:	/* pointee typedef: keep the name */
	case BTF_KIND_FWD:
		snprintf(out, osz, "%s", btf__name_by_offset(btf, t->name_off));
		return;
	case BTF_KIND_INT: {
		__u32 ii = *(const __u32 *)(t + 1);
		int enc = BTF_INT_ENCODING(ii), bits = BTF_INT_BITS(ii), s;

		if (enc & BTF_INT_BOOL) { snprintf(out, osz, "bool"); return; }
		s = (enc & BTF_INT_SIGNED) || (enc & BTF_INT_CHAR);
		switch (bits) {
		case 8:  snprintf(out, osz, s ? "char"  : "uchar");  break;
		case 16: snprintf(out, osz, s ? "short" : "ushort"); break;
		case 32: snprintf(out, osz, s ? "int"   : "uint");   break;
		case 64: snprintf(out, osz, s ? "long"  : "ulong");  break;
		default: snprintf(out, osz, "%sint%d", s ? "" : "u", bits); break;
		}
		return;
	}
	case BTF_KIND_PTR: {
		char inner[80];
		canon(btf, t->type, inner, sizeof inner, 1);
		snprintf(out, osz, "%s*", inner);
		return;
	}
	case BTF_KIND_STRUCT:
	case BTF_KIND_UNION: {
		const char *n = btf__name_by_offset(btf, t->name_off);
		snprintf(out, osz, "%s", (n && *n) ? n : "anon");
		return;
	}
	case BTF_KIND_ENUM:
	case BTF_KIND_ENUM64:
		snprintf(out, osz, "enum");
		return;
	default:
		snprintf(out, osz, "kind%u", kind);
		return;
	}
}

/* Fill @ret/@params (canonical tokens) for @sym. func: try the name then the
 * *_noprof variant. hook: resolve btf_trace_<sym> (typedef->ptr->func_proto) and
 * skip the implicit leading void*data. Returns 1 if found in BTF, else 0. */
static int get_sig(const struct btf *btf, const char *sym, int is_hook,
		   char *ret, size_t rsz, char *params, size_t psz)
{
	const struct btf_type *t, *proto;
	const struct btf_param *p;
	__s32 id;
	int skip = 0, vlen, i, first = 1;

	if (is_hook) {
		char nm[192];

		snprintf(nm, sizeof nm, "btf_trace_%s", sym);
		id = btf__find_by_name_kind(btf, nm, BTF_KIND_TYPEDEF);
		if (id < 0)
			return 0;
		t = btf__type_by_id(btf, id);		/* TYPEDEF */
		t = btf__type_by_id(btf, t->type);	/* -> PTR */
		if (!t || BTF_INFO_KIND(t->info) != BTF_KIND_PTR)
			return 0;
		proto = btf__type_by_id(btf, t->type);	/* -> FUNC_PROTO */
		skip = 1;
	} else {
		id = btf__find_by_name_kind(btf, sym, BTF_KIND_FUNC);
		if (id < 0) {
			char np[192];

			snprintf(np, sizeof np, "%s_noprof", sym);
			id = btf__find_by_name_kind(btf, np, BTF_KIND_FUNC);
		}
		if (id < 0)
			return 0;
		t = btf__type_by_id(btf, id);		/* FUNC */
		proto = btf__type_by_id(btf, t->type);	/* -> FUNC_PROTO */
	}
	if (!proto || BTF_INFO_KIND(proto->info) != BTF_KIND_FUNC_PROTO)
		return 0;

	canon(btf, proto->type, ret, rsz, 0);

	vlen = BTF_INFO_VLEN(proto->info);
	p = (const struct btf_param *)(proto + 1);
	params[0] = '\0';
	for (i = skip; i < vlen; i++) {
		char tok[96];
		size_t cur;

		if (p[i].type == 0) {			/* variadic "..." sentinel */
			snprintf(tok, sizeof tok, "...");
		} else {
			canon(btf, p[i].type, tok, sizeof tok, 0);
		}
		cur = strlen(params);
		snprintf(params + cur, psz - cur, "%s%s", first ? "" : ",", tok);
		first = 0;
	}
	if (params[0] == '\0')
		snprintf(params, psz, "void");
	return 1;
}

static const struct kapi_abi_row *row_for(const char *sym, unsigned int kver)
{
	int i;

	for (i = 0; i < KAPI_ABI_NROWS; i++) {
		const struct kapi_abi_row *r = &kapi_abi_table[i];

		if (!strcmp(r->sym, sym) && kver >= r->since && kver < r->until)
			return r;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/sys/kernel/btf/vmlinux";
	unsigned int a = 0, b = 0, c = 0, kver;
	char disable[1024] = "";
	int i, ndis = 0;
	struct btf *btf;

#define P(n) #n,
	static const char *syms[] = { KAPI_SYMBOLS(P) };
#undef P
	const int nsyms = (int)(sizeof(syms) / sizeof(syms[0]));

	if (argc > 2) {				/* explicit "maj.min.patch" (testing) */
		sscanf(argv[2], "%u.%u.%u", &a, &b, &c);
	} else {
		struct utsname u;

		if (uname(&u) == 0)
			sscanf(u.release, "%u.%u.%u", &a, &b, &c);
	}
	kver = (a << 16) | (b << 8) | c;

	btf = btf__parse(path, NULL);
	if (!btf) {
		fprintf(stderr, "kapi_check: cannot load BTF from %s\n", path);
		return 2;			/* caller fail-safe: trust compile gates */
	}
	fprintf(stderr, "kapi_check: kernel %u.%u.%u (0x%06x), BTF %s\n",
		a, b, c, kver, path);

	for (i = 0; i < nsyms; i++) {
		const char *sym = syms[i];
		const struct kapi_abi_row *r = row_for(sym, kver);
		char aret[96], aparams[256];
		size_t cur;

		if (!r) {
			fprintf(stderr, "  %-32s no table row for this kver - skip\n", sym);
			continue;
		}
		if (!get_sig(btf, sym, r->hook, aret, sizeof aret, aparams, sizeof aparams)) {
			/* hook: GKI vmlinux BTF carries no btf_trace_* typedefs, so a
			 * vendor tracepoint is simply not BTF-checkable - the module
			 * locates and registers it by name at runtime. func: absent
			 * symbol -> resolve_kfunc returns NULL -> feature is refused. */
			if (r->hook)
				fprintf(stderr, "  %-32s not BTF-checkable (hook, runtime-registered by name)\n", sym);
			else
				fprintf(stderr, "  %-32s MISSING in BTF (kapi stays NULL, graceful)\n", sym);
			continue;
		}
		if (!strcmp(aret, r->ret) && !strcmp(aparams, r->params)) {
			fprintf(stderr, "  %-32s OK    %s(%s)\n", sym, r->ret, r->params);
			continue;
		}
		fprintf(stderr, "  %-32s MISMATCH exp %s(%s) got %s(%s) -> DISABLE\n",
			sym, r->ret, r->params, aret, aparams);
		cur = strlen(disable);
		snprintf(disable + cur, sizeof disable - cur, "%s%s", ndis ? "," : "", sym);
		ndis++;
	}

	/* MIGRATE_CMA's value on THIS kernel (enum migratetype). -1 = absent
	 * (CONFIG_CMA=n or stripped BTF) -> the module keeps the CMA reservoir
	 * feature off. Read from BTF, never from our build headers: the
	 * enumerator's value is config/vendor-dependent. */
	{
		int cma_val = -1;
		__s32 id = btf__find_by_name_kind(btf, "migratetype", BTF_KIND_ENUM);

		if (id >= 0) {
			const struct btf_type *t = btf__type_by_id(btf, id);
			const struct btf_enum *e = (const struct btf_enum *)(t + 1);
			int vlen = BTF_INFO_VLEN(t->info), i;

			for (i = 0; i < vlen; i++) {
				const char *n = btf__name_by_offset(btf, e[i].name_off);

				if (n && !strcmp(n, "MIGRATE_CMA")) {
					cma_val = e[i].val;
					break;
				}
			}
		}
		if (cma_val < 0)
			fprintf(stderr, "kapi_check: MIGRATE_CMA not in BTF (CONFIG_CMA=n?) -> reservoir off\n");
		else
			fprintf(stderr, "kapi_check: MIGRATE_CMA=%d\n", cma_val);
		printf("migrate_cma=%d\n", cma_val);
	}

	printf("disable=%s\n", disable);
	fprintf(stderr, "kapi_check: %d symbol(s) flagged for disable_kapi\n", ndis);
	btf__free(btf);
	return 0;
}
