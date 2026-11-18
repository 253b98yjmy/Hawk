/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Public Suffix List: registrable domains ("eTLD+1"), used to tell first-party from third-party. */
#ifndef HK_PSL_H
#define HK_PSL_H

#include "base/hk_str.h"

/* The public suffix of host (e.g. "co.jp" for "www.example.co.jp"); a suffix of host. */
hk_str hk_psl_public_suffix(hk_str host);
/* The registrable domain ("example.co.jp"), or an empty string when host is itself a public
 * suffix. IP addresses are returned unchanged. */
hk_str hk_psl_registrable_domain(hk_str host);
/* Do two hosts belong to the same site? */
bool hk_psl_same_site(hk_str a, hk_str b);

#endif
