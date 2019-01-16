#include "postgres.h"
#include "access/twophase.h"
#include "executor/spi.h"
#include "utils/snapmgr.h"
#include "nodes/makefuncs.h"
#include "catalog/namespace.h"
#include "tcop/tcopprot.h"
#include "storage/ipc.h"
#include "miscadmin.h" /* PostmasterPid */

#include "multimaster.h"
#include "bkb.h"
#include "state.h"
#include "logger.h"

char const* const MtmNeighborEventMnem[] =
{
	"MTM_NEIGHBOR_CLIQUE_DISABLE",
	"MTM_NEIGHBOR_WAL_RECEIVER_START",
	"MTM_NEIGHBOR_WAL_SENDER_START_RECOVERY",
	"MTM_NEIGHBOR_WAL_SENDER_START_RECOVERED",
	"MTM_NEIGHBOR_RECOVERY_CAUGHTUP"
};

char const* const MtmEventMnem[] =
{
	"MTM_REMOTE_DISABLE",
	"MTM_CLIQUE_DISABLE",
	"MTM_CLIQUE_MINORITY",
	"MTM_ARBITER_RECEIVER_START",
	"MTM_RECOVERY_START1",
	"MTM_RECOVERY_START2",
	"MTM_RECOVERY_FINISH1",
	"MTM_RECOVERY_FINISH2",
	"MTM_NONRECOVERABLE_ERROR"
};

char const* const MtmNodeStatusMnem[] =
{
	"Disabled",
	"Recovery",
	"Recovered",
	"Online"
};

static int  MtmRefereeGetWinner(void);
static bool MtmRefereeClearWinner(void);
static int  MtmRefereeReadSaved(void);

static bool mtm_state_initialized;

// XXXX: allocate in context and clean it
static char *
maskToString(nodemask_t mask, int nNodes)
{
	char *strMask = palloc0(nNodes + 1);
	int i;

	for (i = 0; i < nNodes; i++)
		strMask[i] = BIT_CHECK(mask, i) ? '1' : '0';

	return strMask;
}

int
countZeroBits(nodemask_t mask, int nNodes)
{
	int i, count = 0;
	for (i = 0; i < nNodes; i++)
	{
		if (!BIT_CHECK(mask, i))
			count++;
	}
	return count;
}

static void
MtmStateFill(MtmConfig *cfg)
{
	Mtm->nAllNodes = cfg->n_nodes;
	Mtm->my_node_id = cfg->my_node_id;
}

static void
MtmSetClusterStatus(MtmNodeStatus status, char *statusReason)
{
	if (Mtm->status == status)
		return;

	mtm_log(MtmStateSwitch, "[STATE]   Switching status from %s to %s: %s",
			 MtmNodeStatusMnem[Mtm->status], MtmNodeStatusMnem[status],
			 statusReason);

	/*
	 * Do some actions on specific status transitions.
	 * This will be executed only once because of preceeding if stmt.
	 */
	if (status == MTM_DISABLED)
	{
		Mtm->recovered = false;
		Mtm->recoverySlot = 0;
		Mtm->pglogicalReceiverMask = 0;
		Mtm->pglogicalSenderMask = 0;
		Mtm->recoveryCount++; /* this will restart replication connection */
	}

	/*
	 * Check saved referee decision and clean it
	 */
	if (status == MTM_ONLINE)
	{
		int saved_winner_node_id = MtmRefereeReadSaved();
		if (!Mtm->refereeGrant && saved_winner_node_id > 0)
		{
			/*
			 * We booted after being with refereeGrant,
			 * but now have ordinary majority.
			 */
			// MtmPollStatusOfPreparedTransactions(true);
			ResolveAllTransactions();
			Mtm->refereeWinnerId = saved_winner_node_id;
		}
		MtmEnableNode(Mtm->my_node_id);
	}

	Mtm->status = status;
	Mtm->statusReason = statusReason;
}

static void
MtmCheckState(void)
{
	// int nVotingNodes = MtmGetNumberOfVotingNodes();
	bool isEnabledState;
	char *statusReason = "node is disabled by default";
	MtmNodeStatus old_status;
	int nEnabled   = countZeroBits(Mtm->disabledNodeMask, Mtm->nAllNodes);
	int nConnected = countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes);
	int nReceivers = Mtm->nAllNodes - countZeroBits(Mtm->pglogicalReceiverMask, Mtm->nAllNodes);
	int nSenders   = Mtm->nAllNodes - countZeroBits(Mtm->pglogicalSenderMask, Mtm->nAllNodes);

	old_status = Mtm->status;

	mtm_log(MtmStateMessage,
		"[STATE]   Status = (disabled=%s, unaccessible=%s, clique=%s, receivers=%s, senders=%s, total=%i, referee_grant=%d, stopped=%s)",
		maskToString(Mtm->disabledNodeMask, Mtm->nAllNodes),
		maskToString(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes),
		maskToString(Mtm->clique, Mtm->nAllNodes),
		maskToString(Mtm->pglogicalReceiverMask, Mtm->nAllNodes),
		maskToString(Mtm->pglogicalSenderMask, Mtm->nAllNodes),
		Mtm->nAllNodes,
		(Mtm->refereeGrant),
		maskToString(Mtm->stoppedNodeMask, Mtm->nAllNodes));

#define ENABLE_IF(cond, reason) if ((cond) && !isEnabledState) { \
	isEnabledState = true; statusReason = reason; }
#define DISABLE_IF(cond, reason) if ((cond) && isEnabledState) { \
	isEnabledState = false; statusReason = reason; }

	isEnabledState = false;
	ENABLE_IF(nConnected >= Mtm->nAllNodes/2+1,
			  "node belongs to the majority group");
	ENABLE_IF(nConnected == Mtm->nAllNodes/2 && Mtm->refereeGrant,
			  "node has a referee grant");
	DISABLE_IF(!BIT_CHECK(Mtm->clique, Mtm->my_node_id-1) && !Mtm->refereeGrant,
			   "node is not in clique and has no referee grant");
	DISABLE_IF(BIT_CHECK(Mtm->stoppedNodeMask, Mtm->my_node_id-1),
			   "node is stopped manually");

#undef ENABLE_IF
#undef DISABLE_IF

	/* ANY -> MTM_DISABLED */
	if (!isEnabledState)
	{
		// BIT_SET(Mtm->disabledNodeMask, MtmNodeId-1);
		MtmSetClusterStatus(MTM_DISABLED, statusReason);
		MtmDisableNode(Mtm->my_node_id);
		return;
	}

	switch (Mtm->status)
	{
		case MTM_DISABLED:
			if (isEnabledState)
			{
				MtmSetClusterStatus(MTM_RECOVERY, statusReason);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		case MTM_RECOVERY:
			if (Mtm->recovered)
			{
				MtmSetClusterStatus(MTM_RECOVERED, statusReason);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		/*
		 * Switching from MTM_RECOVERY to MTM_ONLINE requires two state
		 * re-checks. If by the time of MTM_RECOVERY -> MTM_RECOVERED all
		 * senders/receiveirs already atarted we can stuck in MTM_RECOVERED
		 * state. Hence call MtmCheckState() from periodic status check while
		 * in MTM_RECOVERED state.
		 */
		case MTM_RECOVERED:
			if (nReceivers == nEnabled && nSenders == nEnabled && nEnabled == nConnected-1)
			{
				MtmSetClusterStatus(MTM_ONLINE, statusReason);

				if (old_status != Mtm->status)
					MtmCheckState();
				return;
			}
			break;

		case MTM_ONLINE:
			{
				int nEnabled = countZeroBits(Mtm->disabledNodeMask, Mtm->nAllNodes);
				// Assert( (nEnabled >= Mtm->nAllNodes/2+1) ||
				// 		(nEnabled == Mtm->nAllNodes/2 && Mtm->refereeGrant));
				if ( !((nEnabled >= Mtm->nAllNodes/2+1) ||
						(nEnabled == Mtm->nAllNodes/2 && Mtm->refereeGrant)) )
				{
					mtm_log(MtmStateMessage, "[STATE] disable myself, nEnabled less then majority");
					MtmSetClusterStatus(MTM_DISABLED, statusReason);
					MtmDisableNode(Mtm->my_node_id);
					/* do not recur */
					return;
				}
			}
			break;
	}

}


void
MtmStateProcessNeighborEvent(int node_id, MtmNeighborEvent ev, bool locked) // XXXX camelcase node_id
{
	mtm_log(MtmStateMessage, "[STATE] Node %i: %s", node_id, MtmNeighborEventMnem[ev]);

	Assert(node_id != Mtm->my_node_id);

	if (!locked)
		MtmLock(LW_EXCLUSIVE);

	switch(ev)
	{
		case MTM_NEIGHBOR_CLIQUE_DISABLE:
			MtmDisableNode(node_id);
			break;

		// XXX: now that means reception of parallel-safe message  through
		// replication. Need to be renamed.
		case MTM_NEIGHBOR_WAL_RECEIVER_START:
			BIT_SET(Mtm->pglogicalReceiverMask, node_id - 1);
			break;

		case MTM_NEIGHBOR_WAL_SENDER_START_RECOVERY:
			/*
			 * With big heartbeat recv timeout it can heppend that other node will
			 * restart faster than we can detect that. Without disabledNodeMask bit set
			 * we will never send recovery_finish in such case. So set it now.
			 *
			 * It is also possible to change logic of recovery_finish but for
			 * now it is easier to do it here. MtmIsRecoveredNode deserves rewrite anyway.
			 */
			if (!BIT_CHECK(Mtm->disabledNodeMask, node_id-1))
			{
				mtm_log(MtmStateMessage, "[WARN] node %d started recovery, but it wasn't disabled", node_id);
				MtmDisableNode(node_id);
			}
			break;

		case MTM_NEIGHBOR_WAL_SENDER_START_RECOVERED:
			BIT_SET(Mtm->pglogicalSenderMask, node_id - 1);
			MtmEnableNode(node_id);
			break;

		case MTM_NEIGHBOR_RECOVERY_CAUGHTUP:
			// MtmEnableNode(node_id);
			break;

	}
	MtmCheckState();

	if (!locked)
		MtmUnlock();
}


void
MtmStateProcessEvent(MtmEvent ev, bool locked)
{
	mtm_log(MtmStateMessage, "[STATE] %s", MtmEventMnem[ev]);

	if (!locked)
		MtmLock(LW_EXCLUSIVE);

	switch (ev)
	{
		case MTM_CLIQUE_DISABLE:
			BIT_SET(Mtm->disabledNodeMask, Mtm->my_node_id-1);
			Mtm->recoveryCount++; /* this will restart replication connection */
			break;

		case MTM_REMOTE_DISABLE:
		case MTM_CLIQUE_MINORITY:
			break;

		case MTM_ARBITER_RECEIVER_START:
			// MtmOnNodeConnect(MtmNodeId);
			break;

		case MTM_RECOVERY_START1:
		case MTM_RECOVERY_START2:
			break;

		case MTM_RECOVERY_FINISH1:
		case MTM_RECOVERY_FINISH2:
			{
				Mtm->recovered = true;
				Mtm->recoveryCount++; /* this will restart replication connection */
				Mtm->recoverySlot = 0;
			}
			break;

		case MTM_NONRECOVERABLE_ERROR:
			// kill(PostmasterPid, SIGQUIT);
			break;
	}

	MtmCheckState();

	if (!locked)
		MtmUnlock();

}

/*
 * Node is disabled if it is not part of clique built using connectivity masks of all nodes.
 * There is no warranty that all nodes will make the same decision about clique, but as far as we want to avoid
 * some global coordinator (which will be SPOF), we have to rely on Bron–Kerbosch algorithm locating maximum clique in graph
 */
void MtmDisableNode(int nodeId)
{
	if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1))
		return;

	mtm_log(MtmStateMessage, "[STATE] Node %i: disabled", nodeId);

	BIT_SET(Mtm->disabledNodeMask, nodeId-1);
	// Mtm->nodes[nodeId-1].timeline += 1;

	if (Mtm->status == MTM_ONLINE) {
		/* Make decision about prepared transaction status only in quorum */
		// MtmLock(LW_EXCLUSIVE);
		// MtmPollStatusOfPreparedTransactionsForDisabledNode(nodeId, false);
		ResolveTransactionsForNode(nodeId);

		// MtmUnlock();
	}
}


/*
 * Node is enabled when it's recovery is completed.
 * This why node is mostly marked as recovered when logical sender/receiver to this node is (re)started.
 */
void
MtmEnableNode(int nodeId)
{
	mtm_log(MtmStateMessage, "[STATE] Node %i: enabled", nodeId);
	BIT_CLEAR(Mtm->disabledNodeMask, nodeId-1);
}

/*
 *
 */

void MtmOnNodeDisconnect(char *node_name)
{
	int nodeId;

	sscanf(node_name, "node%d", &nodeId);

	if (BIT_CHECK(SELF_CONNECTIVITY_MASK, nodeId-1))
		return;

	mtm_log(MtmStateMessage, "[STATE] Node %i: disconnected", nodeId);

	/*
	 * We should disable it, as clique detector will not necessarily
	 * do that. For example it will anyway find clique with one node.
	 */

	MtmLock(LW_EXCLUSIVE);
	BIT_SET(SELF_CONNECTIVITY_MASK, nodeId-1);
	MtmDisableNode(nodeId);
	MtmCheckState();
	MtmUnlock();
}

// XXXX: make that event too
void MtmOnNodeConnect(char *node_name)
{
	int nodeId;

	sscanf(node_name, "node%d", &nodeId);

	mtm_log(MtmStateMessage, "[STATE] Node %i: connected", nodeId);

	MtmLock(LW_EXCLUSIVE);
	BIT_CLEAR(SELF_CONNECTIVITY_MASK, nodeId-1);
	MtmCheckState();
	MtmUnlock();
}

/**
 * Build internode connectivity mask. 1 - means that node is disconnected.
 */
static void
MtmBuildConnectivityMatrix(nodemask_t* matrix)
{
	int i, j, n = Mtm->nAllNodes;

	// for (i = 0; i < n; i++)
	// 	matrix[i] = Mtm->nodes[i].connectivityMask;

	/* make matrix symmetric: required for Bron–Kerbosch algorithm */
	for (i = 0; i < n; i++) {
		for (j = 0; j < i; j++) {
			matrix[i] |= ((matrix[j] >> i) & 1) << j;
			matrix[j] |= ((matrix[i] >> j) & 1) << i;
		}
		matrix[i] &= ~((nodemask_t)1 << i);
	}
}



/**
 * Build connectivity graph, find clique in it and extend disabledNodeMask by nodes not included in clique.
 * This function is called by arbiter monitor process with period MtmHeartbeatSendTimeout
 */
void
MtmRefreshClusterStatus()
{
	nodemask_t newClique, oldClique;
	nodemask_t matrix[MTM_MAX_NODES];
	nodemask_t trivialClique = ~SELF_CONNECTIVITY_MASK & (((nodemask_t)1 << Mtm->nAllNodes)-1);
	int cliqueSize;
	int i;

	/*
	 * Periodical check that we are still in RECOVERED state.
	 * See comment to MTM_RECOVERED -> MTM_ONLINE transition in MtmCheckState()
	 */
	// MtmLock(LW_EXCLUSIVE);
	// MtmCheckState();
	// MtmUnlock();

	return;

	/*
	 * Check for referee decision when only half of nodes are visible.
	 * Do not hold lock here, but recheck later wheter mask changed.
	 */
	if (MtmRefereeConnStr && *MtmRefereeConnStr && !Mtm->refereeWinnerId &&
		countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == Mtm->nAllNodes/2)
	{
		int winner_node_id = MtmRefereeGetWinner();

		/* We also can have old value. Do that only from single mtm-monitor process */
		if (winner_node_id <= 0 && !mtm_state_initialized)
		{
			winner_node_id = MtmRefereeReadSaved();
			mtm_state_initialized = true;
		}

		if (winner_node_id > 0)
		{
			Mtm->refereeWinnerId = winner_node_id;
			if (!BIT_CHECK(SELF_CONNECTIVITY_MASK, winner_node_id - 1))
			{
				/*
				 * By the time we enter this block we can already see other nodes.
				 * So recheck old conditions under lock.
				 */
				MtmLock(LW_EXCLUSIVE);
				if (countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == Mtm->nAllNodes/2 &&
					!BIT_CHECK(SELF_CONNECTIVITY_MASK, winner_node_id - 1))
				{
					mtm_log(MtmStateMessage, "[STATE] Referee allowed to proceed with half of the nodes (winner_id = %d)",
					winner_node_id);
					Mtm->refereeGrant = true;
					if (countZeroBits(SELF_CONNECTIVITY_MASK, Mtm->nAllNodes) == 1)
					{
						// MtmPollStatusOfPreparedTransactions(true);
						ResolveAllTransactions();
					}
					MtmEnableNode(Mtm->my_node_id);
					MtmCheckState();
				}
				MtmUnlock();
			}
		}
	}

	/*
	 * Clear winner if we again have all nodes recovered.
	 * We should clean old value based on disabledNodeMask instead of SELF_CONNECTIVITY_MASK
	 * because we can clean old value before failed node starts it recovery and that node
	 * can get refereeGrant before start of walsender, so it start in recovered mode.
	 */
	if (MtmRefereeConnStr && *MtmRefereeConnStr && Mtm->refereeWinnerId &&
		countZeroBits(Mtm->disabledNodeMask, Mtm->nAllNodes) == Mtm->nAllNodes &&
		MtmGetCurrentStatus() == MTM_ONLINE) /* restrict this actions only to major -> online transition */
	{
		if (MtmRefereeClearWinner())
		{
			Mtm->refereeWinnerId = 0;
			Mtm->refereeGrant = false;
			mtm_log(MtmStateMessage, "[STATE] Cleaning old referee decision");
		}
	}

	// Mtm->clique = (((nodemask_t)1 << Mtm->nAllNodes) - 1);
	// return;

	/*
	 * Check for clique.
	 */
	MtmBuildConnectivityMatrix(matrix);
	newClique = MtmFindMaxClique(matrix, Mtm->nAllNodes, &cliqueSize);

	if (newClique == Mtm->clique)
		return;

	mtm_log(MtmStateMessage, "[STATE] Old clique: %s", maskToString(Mtm->clique, Mtm->nAllNodes));

	/*
	 * Otherwise make sure that all nodes have a chance to replicate their connectivity
	 * mask and we have the "consistent" picture. Obviously we can not get true consistent
	 * snapshot, but at least try to wait heartbeat send timeout is expired and
	 * connectivity graph is stabilized.
	 */
	do {
		oldClique = newClique;
		/*
		 * Double timeout to consider the worst case when heartbeat receive interval is added
		 * with refresh cluster status interval.
		 */
		MtmSleep(1000L*(MtmHeartbeatRecvTimeout)*2);
		MtmBuildConnectivityMatrix(matrix);
		newClique = MtmFindMaxClique(matrix, Mtm->nAllNodes, &cliqueSize);
	} while (newClique != oldClique);

	mtm_log(MtmStateMessage, "[STATE] New clique: %s", maskToString(oldClique, Mtm->nAllNodes));

	if (newClique != trivialClique)
	{
		mtm_log(MtmStateMessage, "[STATE] NONTRIVIAL CLIQUE! (trivial: %s)", maskToString(trivialClique, Mtm->nAllNodes)); // XXXX some false-positives, fixme
	}

	/*
	 * We are using clique only to disable nodes.
	 * So find out what node should be disabled and disable them.
	 */
	MtmLock(LW_EXCLUSIVE);

	Mtm->clique = newClique;

	/*
	 * Do not perform any action based on clique with referee grant,
	 * because we can disable ourself.
	 * But we also need to maintain actual clique not disable ourselves
	 * when neighbour node will come back and we erase refereeGrant.
	 */
	if (Mtm->refereeGrant)
	{
		MtmUnlock();
		return;
	}

	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		bool old_status = BIT_CHECK(Mtm->disabledNodeMask, i);
		bool new_status = BIT_CHECK(~newClique, i);

		if (new_status && new_status != old_status)
		{
			if ( i+1 == Mtm->my_node_id )
				MtmStateProcessEvent(MTM_CLIQUE_DISABLE, true);
			else
				MtmStateProcessNeighborEvent(i+1, MTM_NEIGHBOR_CLIQUE_DISABLE, true);
		}
	}

	MtmCheckState();
	MtmUnlock();
}

/*
 * Referee caches decision in mtm.referee_decision
 */
static bool
MtmRefereeHasLocalTable()
{
	RangeVar   *rv;
	Oid			rel_oid;
	static bool _has_local_tables;
	bool		txstarted = false;

	/* memoized */
	if (_has_local_tables)
		return true;

	if (!IsTransactionState())
	{
		txstarted = true;
		StartTransactionCommand();
	}

	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, "referee_decision", -1);
	rel_oid = RangeVarGetRelid(rv, NoLock, true);

	if (txstarted)
		CommitTransactionCommand();

	if (OidIsValid(rel_oid))
	{
		// MtmMakeRelationLocal(rel_oid);
		_has_local_tables = true;
		return true;
	}
	else
		return false;
}

static int
MtmRefereeReadSaved(void)
{
	int winner = -1;
	int rc;
	bool txstarted = false;

	if (!MtmRefereeHasLocalTable())
		return -1;

	/* Save result locally */
	if (!IsTransactionState())
	{
		txstarted = true;
		StartTransactionCommand();
	}
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	rc = SPI_execute("select node_id from mtm.referee_decision where key = 'winner';", true, 0);
	if (rc != SPI_OK_SELECT)
	{
		mtm_log(WARNING, "Failed to load referee decision");
	}
	else if (SPI_processed > 0)
	{
		bool isnull;
		winner = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		Assert(SPI_processed == 1);
		Assert(!isnull);
	}
	else
	{
		/* no saved decision found */
		Assert(SPI_processed == 0);
	}
	SPI_finish();
	PopActiveSnapshot();
	if (txstarted)
		CommitTransactionCommand();

	mtm_log(MtmStateMessage, "Read saved referee decision, winner=%d.", winner);
	return winner;
}

static int
MtmRefereeGetWinner(void)
{
	PGconn* conn;
	PGresult *res;
	char sql[128];
	int  winner_node_id;
	int  old_winner = -1;
	int  rc;

	conn = PQconnectdb(MtmRefereeConnStr);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		mtm_log(WARNING, "Could not connect to referee");
		PQfinish(conn);
		return -1;
	}

	sprintf(sql, "select referee.get_winner(%d)", Mtm->my_node_id);
	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 ||
		PQnfields(res) != 1)
	{
		mtm_log(WARNING, "Refusing unexpected result (r=%d, n=%d, w=%d, k=%s) from referee.get_winner()",
			PQresultStatus(res), PQntuples(res), PQnfields(res), PQgetvalue(res, 0, 0));
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	winner_node_id = atoi(PQgetvalue(res, 0, 0));

	if (winner_node_id < 1 || winner_node_id > Mtm->nAllNodes)
	{
		mtm_log(WARNING,
			"Referee responded with node_id=%d, it's out of our node range",
			winner_node_id);
		PQclear(res);
		PQfinish(conn);
		return -1;
	}

	/* Ok, we finally got it! */
	PQclear(res);
	PQfinish(conn);

	/* Save result locally */
	if (MtmRefereeHasLocalTable())
	{
		// MtmEnforceLocalTx = true;
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		/* Check old value if any */
		rc = SPI_execute("select node_id from mtm.referee_decision where key = 'winner';", true, 0);
		if (rc != SPI_OK_SELECT)
		{
			mtm_log(WARNING, "Failed to load previous referee decision");
		}
		else if (SPI_processed > 0)
		{
			bool isnull;
			old_winner = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
			Assert(SPI_processed == 1);
			Assert(!isnull);
		}
		else
		{
			/* no saved decision found */
			Assert(SPI_processed == 0);
		}
		/* Update actual key */
		sprintf(sql,
			"insert into mtm.referee_decision values ('winner', %d) on conflict(key) do nothing;",
			winner_node_id);
		rc = SPI_execute(sql, false, 0);
		SPI_finish();
		if (rc < 0)
			mtm_log(WARNING, "Failed to save referee decision, but proceeding anyway");
		PopActiveSnapshot();
		CommitTransactionCommand();
		// MtmEnforceLocalTx = false;

		if (old_winner > 0 && old_winner != winner_node_id)
			mtm_log(MtmStateMessage, "WARNING Overriding old referee decision (%d) with new one (%d)", old_winner, winner_node_id);
	}

	mtm_log(MtmStateMessage, "Got referee response, winner node_id=%d.", winner_node_id);
	return winner_node_id;
}

static bool
MtmRefereeClearWinner(void)
{
	PGconn* conn;
	PGresult *res;
	char *response;
	int rc;

	/*
	 * Delete result locally first.
	 *
	 * If we delete decision from referee but fail to delete local cached
	 * that will be pretty bad -- on the next reboot we can read
	 * stale referee decision and on next failure end up with two masters.
	 * So just delete local cache first.
	 */
	if (MtmRefereeHasLocalTable())
	{
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		rc = SPI_execute("delete from mtm.referee_decision where key = 'winner'", false, 0);
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		if (rc < 0)
		{
			mtm_log(WARNING, "Failed to clean referee decision");
			return false;
		}
	}


	conn = PQconnectdb(MtmRefereeConnStr);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		mtm_log(WARNING, "Could not connect to referee");
		PQfinish(conn);
		return false;
	}

	res = PQexec(conn, "select referee.clean()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK ||
		PQntuples(res) != 1 ||
		PQnfields(res) != 1)
	{
		mtm_log(WARNING, "Refusing unexpected result (r=%d, n=%d, w=%d, k=%s) from referee.clean().",
			PQresultStatus(res), PQntuples(res), PQnfields(res), PQgetvalue(res, 0, 0));
		PQclear(res);
		PQfinish(conn);
		return false;
	}

	response = PQgetvalue(res, 0, 0);

	if (strncmp(response, "t", 1) != 0)
	{
		mtm_log(WARNING, "Wrong response from referee.clean(): '%s'", response);
		PQclear(res);
		PQfinish(conn);
		return false;
	}

	/* Ok, we finally got it! */
	mtm_log(MtmStateMessage, "Got referee clear response '%s'", response);
	PQclear(res);
	PQfinish(conn);
	return true;
}

/*
 * Mtm current status accessor.
 */
MtmNodeStatus
MtmGetCurrentStatus()
{
	volatile MtmNodeStatus status;
	MtmLock(LW_SHARED);
	status = Mtm->status;
	MtmUnlock();
	return status;
}

/*
 * Mtm current disabledMask accessor.
 */
nodemask_t
MtmGetDisabledNodeMask()
{
	volatile nodemask_t disabledMask;
	MtmLock(LW_SHARED);
	disabledMask = Mtm->disabledNodeMask;
	MtmUnlock();
	return disabledMask;
}

/*****************************************************************************
 *
 * Mtm monitor
 *
 *****************************************************************************/


#include "storage/latch.h"
#include "postmaster/bgworker.h"
#include "utils/guc.h"
#include "pgstat.h"

void MtmMonitor(Datum arg);

static int		sender_to_node[MTM_MAX_NODES];

void
MtmMonitorStart(Oid db_id, Oid user_id)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |	BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = MULTIMASTER_BGW_RESTART_TIMEOUT;
	worker.bgw_main_arg = Int32GetDatum(0);

	memcpy(worker.bgw_extra, &db_id, sizeof(Oid));
	memcpy(worker.bgw_extra + sizeof(Oid), &user_id, sizeof(Oid));

	sprintf(worker.bgw_library_name, "multimaster");
	sprintf(worker.bgw_function_name, "MtmMonitor");
	snprintf(worker.bgw_name, BGW_MAXLEN, "mtm-monitor");

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		elog(ERROR, "Failed to start monitor worker");
}

static void
check_status_requests(MtmConfig *mtm_cfg)
{
	DmqSenderId sender_id;
	StringInfoData buffer;

	while(dmq_pop_nb(&sender_id, &buffer, ~SELF_CONNECTIVITY_MASK))
	{
		int sender_node_id;
		MtmArbiterMessage *msg;
		DmqDestinationId dest_id;
		char *state_3pc;

		sender_node_id = sender_to_node[sender_id];
		msg = (MtmArbiterMessage *) buffer.data;

		Assert(msg->node == sender_node_id);
		Assert(msg->code == MSG_POLL_REQUEST);

		mtm_log(StatusRequest, "got status request for %s from %d",
				msg->gid, sender_node_id);

		state_3pc = GetLoggedPreparedXactState(msg->gid);

		// XXX: define this strings as constants like MULTIMASTER_PRECOMMITTED
		if (strncmp(state_3pc, "notfound", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxNotFound;
		else if (strncmp(state_3pc, "prepared", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxPrepared;
		else if (strncmp(state_3pc, "precommitted", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxPreCommited;
		else if (strncmp(state_3pc, "preaborted", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxPreAborted;
		else if (strncmp(state_3pc, "committed", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxCommited;
		else if (strncmp(state_3pc, "aborted", MAX_3PC_STATE_SIZE) == 0)
			msg->state = MtmTxAborted;
		else
			Assert(false);

		mtm_log(StatusRequest, "responding to %d with %s -> %s",
				sender_node_id, msg->gid, MtmTxStateMnem(msg->state));

		pfree(state_3pc);

		msg->code = MSG_POLL_STATUS;
		msg->node = Mtm->my_node_id;

		dest_id = mtm_cfg->nodes[sender_node_id - 1].destination_id;

		// XXX: and define channels as strings too
		dmq_push_buffer(dest_id, "txresp", msg,
						sizeof(MtmArbiterMessage));

		mtm_log(StatusRequest, "responded to %d with %s -> %s, code = %d",
				sender_node_id, msg->gid, MtmTxStateMnem(msg->state), msg->code);
	}
}

void
MtmMonitor(Datum arg)
{
	int			i,
				sender_id = 0;
	Oid			db_id,
				user_id;
	MtmConfig  *mtm_cfg;

	pqsignal(SIGTERM, die);
	pqsignal(SIGHUP, PostgresSigHupHandler);
	
	MtmBackgroundWorker = true;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to a database */
	memcpy(&db_id, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	memcpy(&user_id, MyBgworkerEntry->bgw_extra + sizeof(Oid), sizeof(Oid));
	BackgroundWorkerInitializeConnectionByOid(db_id, user_id, 0);

	/*
	 * During init_node() our worker is started from transaction that created
	 * mtm config, so we can get here before this transaction is committed,
	 * so we won't see config yet. Just wait for it to became visible.
	 */
	for (;;)
	{
		mtm_cfg = MtmLoadConfig();
		if (mtm_cfg->n_nodes > 0)
			break;
		pfree(mtm_cfg);
		MtmSleep(USECS_PER_SEC);
	}

	/* Set proper values in Mtm structure */
	MtmStateFill(mtm_cfg);

	/* Now start all other necessary workers */

	/* Launch resolver */
	ResolverStart(db_id, user_id);

	/* Order dmq-sender to connect with publishers */
	for (i = 0; i < mtm_cfg->n_nodes; i++)
	{
		char   *connstr_with_appname;

		if (i + 1 == Mtm->my_node_id)
			continue;

		connstr_with_appname = psprintf("%s application_name=%s",
										mtm_cfg->nodes[i].conninfo,
										MULTIMASTER_BROADCAST_SERVICE);

		dmq_destination_add(connstr_with_appname,
							psprintf("node%d", mtm_cfg->my_node_id),
							psprintf("node%d", i + 1),
							MtmHeartbeatSendTimeout);

		pfree(connstr_with_appname);
	}

	/* Launch receivers */
	for (i = 0; i < mtm_cfg->n_nodes; i++)
	{
		if (i == mtm_cfg->my_node_id - 1)
			continue;
		MtmStartReceiver(i + 1, db_id, user_id);
	}

	// XXX: turn this into a function
	/* subscribe to status-requests channels from other nodes */
	for (i = 0; i < mtm_cfg->n_nodes; i++)
	{
		if (i + 1 != mtm_cfg->my_node_id)
		{
			dmq_attach_receiver(psprintf("node%d", i + 1), i);
			sender_to_node[sender_id++] = i + 1;
		}
	}

	dmq_stream_subscribe("txreq");

	for (;;)
	{
		int rc;

		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		MtmRefreshClusterStatus();

		check_status_requests(mtm_cfg);

		// MtmCheckSlots(); // XXX: add locking

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   MtmHeartbeatRecvTimeout, PG_WAIT_EXTENSION);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);

	}
}
