/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GH_STATIC_KEY_H
#define GH_STATIC_KEY_H

/* The including environment supplies struct static_key and atomic_read().
 * static_key_enabled() calls the out-of-line static_key_count() when jump
 * labels are enabled. Some older vendor GKI kernels do not export that
 * function, even though newer headers declare it as a module API. Read the
 * counter directly, preserving kernel/jump_label.c's negative-count rule:
 * a first enable in progress (-1) must already read as enabled.
 */
static inline bool gh_static_key_enabled(const struct static_key *key)
{
#ifdef CONFIG_JUMP_LABEL
	return atomic_read(&key->enabled) != 0;
#else
	return atomic_read(&key->enabled) > 0;
#endif
}

#endif /* GH_STATIC_KEY_H */
