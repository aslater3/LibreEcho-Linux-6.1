/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AIS_FSM_RECOVERY_H
#define _AIS_FSM_RECOVERY_H

/*
 * Keep the state decision independent of adapter internals so the exact
 * production decision can be exercised by the host contract test.
 */
enum ENUM_AIS_STALE_RECOVERY_ACTION {
	AIS_STALE_RECOVERY_NONE,
	AIS_STALE_RECOVERY_WAIT_ROAMING,
	AIS_STALE_RECOVERY_DISCONNECT_RETRY,
	AIS_STALE_RECOVERY_ABORT,
};

static inline BOOLEAN aisFsmHasStaleConnectedState(
	BOOLEAN fgBssConnected,
	BOOLEAN fgHostConnected,
	BOOLEAN fgBssidZero,
	BOOLEAN fgConnectionRequested)
{
	return fgBssConnected && fgHostConnected && fgBssidZero &&
	       fgConnectionRequested;
}

static inline enum ENUM_AIS_STALE_RECOVERY_ACTION aisFsmClassifyJoinFailure(
	BOOLEAN fgStaleConnected,
	BOOLEAN fgBssConnected,
	BOOLEAN fgDeadlineExpired)
{
	if (fgStaleConnected)
		return fgDeadlineExpired ? AIS_STALE_RECOVERY_ABORT :
			AIS_STALE_RECOVERY_DISCONNECT_RETRY;

	if (fgBssConnected)
		return AIS_STALE_RECOVERY_WAIT_ROAMING;

	return AIS_STALE_RECOVERY_NONE;
}

#endif /* _AIS_FSM_RECOVERY_H */
