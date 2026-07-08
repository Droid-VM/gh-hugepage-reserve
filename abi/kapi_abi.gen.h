/* AUTO-GENERATED from abi/kapi_abi.tsv by abi/gen_kapi.awk - DO NOT EDIT. */
#ifndef KAPI_ABI_GEN_H
#define KAPI_ABI_GEN_H

/* Every resolvable kapi logical symbol (canonical kernel name). Consumed by
 * the module (disable_kapi validation/logging) and the userspace preflight,
 * which must emit these exact names in the disable list. */
#define KAPI_SYMBOLS(X) \
	X(alloc_contig_pages) \
	X(alloc_contig_range) \
	X(prep_compound_page) \
	X(folio_isolate_lru) \
	X(reclaim_pages) \
	X(lru_add_drain_all) \
	X(drop_slab) \
	X(drain_all_pages) \
	X(set_pageblock_migratetype) \
	X(get_pfnblock_flags_mask) \
	X(mem_cgroup_from_task) \
	X(try_to_free_mem_cgroup_pages) \
	X(android_vh_free_one_page_bypass)

#ifdef KAPI_ABI_WANT_TABLE
/* Expected ABI signature per [since, until) version range. Types are
 * DESUGARED canonical (gfp_t/acr_flags_t -> uint) to mirror kCFI. */
struct kapi_abi_row {
	const char *sym;
	unsigned int since;   /* >= this kver (maj<<16|min<<8|patch); 0 = any */
	unsigned int until;   /* <  this kver; 0xffffffff = open-ended */
	int hook;             /* 1 = vendor tracepoint: btf_trace_<sym>, +void*data */
	const char *ret;
	const char *params;   /* comma-separated; "void" = no params */
	const char *feature;
};
static const struct kapi_abi_row kapi_abi_table[] = {
	{ "alloc_contig_pages", 0x000000u, 0xffffffffu, 0, "page*", "ulong,uint,int,nodemask_t*", "id1" },
	{ "alloc_contig_range", 0x000000u, 0xffffffffu, 0, "int", "ulong,ulong,uint,uint", "id2,id3" },
	{ "prep_compound_page", 0x000000u, 0xffffffffu, 0, "void", "page*,uint", "id1,id2,id3" },
	{ "folio_isolate_lru", 0x000000u, 0x060300u, 0, "int", "folio*", "id3" },
	{ "folio_isolate_lru", 0x060300u, 0xffffffffu, 0, "bool", "folio*", "id3" },
	{ "reclaim_pages", 0x000000u, 0x060600u, 0, "ulong", "list_head*", "id3" },
	{ "reclaim_pages", 0x060600u, 0x060c00u, 0, "ulong", "list_head*,bool", "id3" },
	{ "reclaim_pages", 0x060c00u, 0xffffffffu, 0, "ulong", "list_head*", "id3" },
	{ "lru_add_drain_all", 0x000000u, 0xffffffffu, 0, "void", "void", "id2,id3" },
	{ "drop_slab", 0x000000u, 0xffffffffu, 0, "void", "void", "id2,id3" },
	{ "drain_all_pages", 0x000000u, 0xffffffffu, 0, "void", "zone*", "core" },
	{ "set_pageblock_migratetype", 0x000000u, 0x061000u, 0, "void", "page*,int", "cma" },
	{ "get_pfnblock_flags_mask", 0x000000u, 0x061000u, 0, "ulong", "page*,ulong,ulong", "cma" },
	{ "mem_cgroup_from_task", 0x000000u, 0xffffffffu, 0, "mem_cgroup*", "task_struct*", "A" },
	{ "try_to_free_mem_cgroup_pages", 0x000000u, 0x060c00u, 0, "ulong", "mem_cgroup*,ulong,uint,uint", "A" },
	{ "try_to_free_mem_cgroup_pages", 0x060c00u, 0xffffffffu, 0, "ulong", "mem_cgroup*,ulong,uint,uint,int*", "A" },
	{ "android_vh_free_one_page_bypass", 0x000000u, 0xffffffffu, 1, "void", "page*,zone*,int,int,int,bool*", "hook" },
};
#define KAPI_ABI_NROWS ((int)(sizeof(kapi_abi_table) / sizeof(kapi_abi_table[0])))
#endif /* KAPI_ABI_WANT_TABLE */

#endif /* KAPI_ABI_GEN_H */
