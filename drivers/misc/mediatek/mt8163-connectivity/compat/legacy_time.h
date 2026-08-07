/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT8163_CONN_LEGACY_TIME_H
#define _MT8163_CONN_LEGACY_TIME_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/string.h>

#define strnicmp strncasecmp
#define ioremap_nocache ioremap
#define netif_rx_ni netif_rx
#define IEEE80211_BAND_2GHZ NL80211_BAND_2GHZ
#define IEEE80211_BAND_5GHZ NL80211_BAND_5GHZ
#define STATION_INFO_TX_BITRATE BIT_ULL(NL80211_STA_INFO_TX_BITRATE)
#define STATION_INFO_SIGNAL BIT_ULL(NL80211_STA_INFO_SIGNAL)
#define STATION_INFO_TX_PACKETS BIT_ULL(NL80211_STA_INFO_TX_PACKETS)
#define STATION_INFO_TX_FAILED BIT_ULL(NL80211_STA_INFO_TX_FAILED)
#define WIPHY_FLAG_SUPPORTS_SCHED_SCAN 0
#define ASSERT_BREAK(_exp)						\
	{								\
		if (!(_exp)) {						\
			ASSERT(FALSE);					\
			break;						\
		}							\
	}

/*
 * struct timeval was removed from the kernel-internal time API.  The vendor
 * connectivity code uses it only as a private seconds/microseconds tuple.
 */
struct timeval {
	time64_t tv_sec;
	long tv_usec;
};

/*
 * The vendor diagnostics store wall-clock seconds and microseconds in their
 * private rings.  Keep that representation without restoring any userspace
 * address-limit or syscall compatibility.
 */
static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 now;

	ktime_get_real_ts64(&now);
	tv->tv_sec = now.tv_sec;
	tv->tv_usec = now.tv_nsec / NSEC_PER_USEC;
}

#endif
