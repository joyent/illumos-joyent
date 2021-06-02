/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2019 Joyent, Inc.
 */

#include <mdb/mdb_ctf.h>
#include <sys/mdb_modapi.h>

#include <sys/kstat.h>

typedef enum mdb_kstat_flags {
	MDB_KSTAT_FLAG_ADDRESS	= 1 << 0,
} mdb_kstat_flags_t;

/*
 * The kstat framework wraps kstat_t in an ekstat_t struct that holds some
 * internal data.
 */
typedef struct mdb_ekstat {
	kstat_t	e_ks;
} mdb_ekstat_t;

/*
 * ::kstat
 *
 * Print some relevant fields from kstats on the system.
 *
 */
static int
kstat_print(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	const char *mod, *name, *class;
	int inst;
	int output_flags;
	const char *search_mod = NULL;
	const char *search_name = NULL;
	const char *search_class = NULL;
	int search_inst;
	boolean_t search_inst_set = B_FALSE;

	mdb_ekstat_t ekstat;

	if (!(flags & DCMD_ADDRSPEC)) {
		if (mdb_walk_dcmd("kstat", "kstat", argc, argv) == -1) {
			return (DCMD_ERR);
		}

		return (DCMD_OK);
	}

	if (mdb_getopts(argc, argv,
	    'm', MDB_OPT_STR, &search_mod,
	    'i', MDB_OPT_INT_SET, &search_inst_set, &search_inst,
	    'n', MDB_OPT_STR, &search_name,
	    'c', MDB_OPT_STR, &search_class,
	    'a', MDB_OPT_SETBITS, MDB_KSTAT_FLAG_ADDRESS, &output_flags,
	    NULL) != argc) {
		return (DCMD_USAGE);
	}

	if ((search_mod != NULL && strlen(search_mod) >= KSTAT_STRLEN) ||
	    (search_name != NULL && strlen(search_name) >= KSTAT_STRLEN) ||
	    (search_class != NULL && strlen(search_class) >= KSTAT_STRLEN)) {
		mdb_warn("provided module, class, and name lengths must be"
		    " < KSTAT_STRLEN\n");
		return (DCMD_ERR);
	}

	if (DCMD_HDRSPEC(flags)) {
		if (output_flags & MDB_KSTAT_FLAG_ADDRESS) {
			mdb_printf("%<u>%-?s %</u>", "ADDRESS");
		}
		mdb_printf("%<u>%-10s %-10s %-20s %-15s%</u>\n", "MODULE",
		    "INSTANCE", "NAME", "CLASS");
	}

	if (mdb_ctf_vread(&ekstat, "ekstat_t", "mdb_ekstat_t", addr, 0) == -1) {
		mdb_warn("unable to read CTF data for 'ekstat_t'");
		return (DCMD_ERR);
	}

	mod = ekstat.e_ks.ks_module;
	inst = ekstat.e_ks.ks_instance;
	name = ekstat.e_ks.ks_name;
	class = ekstat.e_ks.ks_class;

	/*
	 * Short circuit if the kstat in question doesn't match the user's
	 * filter(s).
	 */
	if ((search_mod != NULL &&
	    strncmp(mod, search_mod, KSTAT_STRLEN) != 0) ||
	    (search_name != NULL &&
	    strncmp(name, search_name, KSTAT_STRLEN) != 0) ||
	    (search_class != NULL &&
	    strncmp(class, search_class, KSTAT_STRLEN) != 0) ||
	    (search_inst_set && search_inst != inst)) {

		return (DCMD_OK);
	}

	if (output_flags & MDB_KSTAT_FLAG_ADDRESS) {
		mdb_printf("%0?p ", addr);
	}
	mdb_printf("%-10s %-10d %-20s %-15s\n", mod,
	    ekstat.e_ks.ks_instance, name, class);

	return (DCMD_OK);
}

void
kstat_help(void)
{
	mdb_printf("Display kstat_t summaries.\n"
	    " -a   also display an address column for each kstat\n"
	    " -m   search for kstats with the given module (e.g. 'zfs')\n"
	    " -i   search for kstats with the given instance number"
	    " (e.g. 0t1)\n"
	    " -n   search for kstats with the given name (e.g. 'zfetchstats')\n"
	    " -c   search for kstats with the given class (e.g. 'misc')\n");
}

/*
 * ::walk kstat
 *
 * Walk all ekstat_t structures in the kstat AVL tree. This is nothing more than
 * a layered avl walk.
 */
static int
kstat_walk_init(mdb_walk_state_t *wsp)
{
	GElf_Sym sym;

	if (wsp->walk_addr != 0) {
		mdb_warn("kstat walk only supports global walks\n");
		return (WALK_ERR);
	}

	if (mdb_lookup_by_obj("unix", "kstat_avl_byname", &sym) == -1) {
		mdb_warn("failed to find symbol 'kstat_avl_byname'");
		return (WALK_ERR);
	}

	wsp->walk_addr = (uintptr_t)sym.st_value;

	if (mdb_layered_walk("avl", wsp) == -1) {
		mdb_warn("failed to walk 'avl'");
		return (WALK_ERR);
	}

	return (WALK_NEXT);
}

static int
kstat_walk_step(mdb_walk_state_t *wsp)
{
	return (wsp->walk_callback(wsp->walk_addr, NULL, wsp->walk_cbdata));
}

static const mdb_dcmd_t dcmds[] = {
	{ "kstat", "?[-a] [ -m module ] [ -i instance ] [ -n name ]"
	    " [ -c class ]\n",
	    "kstat summary", kstat_print, kstat_help },
	{ NULL }
};

static const mdb_walker_t walkers[] = {
	{ "kstat", "walk all kstat structures", kstat_walk_init,
	    kstat_walk_step, NULL },
	{ NULL }
};

static const mdb_modinfo_t modinfo = {
	MDB_API_VERSION, dcmds, walkers
};

const mdb_modinfo_t *
_mdb_init(void)
{
	return (&modinfo);
}
