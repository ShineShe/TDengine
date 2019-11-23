/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"

#include "taosmsg.h"
#include "vnode.h"
#include "vnodeShell.h"
#include "tschemautil.h"

#include "textbuffer.h"
#include "trpc.h"
#include "tscJoinProcess.h"
#include "vnode.h"
#include "vnodeRead.h"
#include "vnodeUtil.h"
#include "vnodeStore.h"

#pragma GCC diagnostic ignored "-Wint-conversion"
extern int tsMaxQueues;

void *      pShellServer = NULL;
SShellObj **shellList = NULL;

int vnodeProcessRetrieveRequest(char *pMsg, int msgLen, SShellObj *pObj);
int vnodeProcessQueryRequest(char *pMsg, int msgLen, SShellObj *pObj);
int vnodeProcessShellSubmitRequest(char *pMsg, int msgLen, SShellObj *pObj);
static void vnodeProcessBatchImportTimer(void *param, void *tmrId);

int vnodeSelectReqNum = 0;
int vnodeInsertReqNum = 0;

typedef struct {
  int32_t import;
  int32_t vnode;
  int32_t numOfSid;
  int32_t ssid;   // Start sid
  SShellObj *pObj;
  int64_t offset; // offset relative the blks
  char    blks[];
} SBatchImportInfo;

void *vnodeProcessMsgFromShell(char *msg, void *ahandle, void *thandle) {
  int        sid, vnode;
  SShellObj *pObj = (SShellObj *)ahandle;
  SIntMsg *  pMsg = (SIntMsg *)msg;
  uint32_t   peerId, peerIp;
  uint16_t   peerPort;
  char       ipstr[20];

  if (msg == NULL) {
    if (pObj) {
      pObj->thandle = NULL;
      dTrace("QInfo:%p %s free qhandle", pObj->qhandle, __FUNCTION__);
      vnodeFreeQInfoInQueue(pObj->qhandle);
      pObj->qhandle = NULL;
      vnodeList[pObj->vnode].shellConns--;
      dTrace("vid:%d, shell connection:%d is gone, shellConns:%d", pObj->vnode, pObj->sid,
             vnodeList[pObj->vnode].shellConns);
    }
    return NULL;
  }

  taosGetRpcConnInfo(thandle, &peerId, &peerIp, &peerPort, &vnode, &sid);

  if (pObj == NULL) {
    if (shellList[vnode]) {
      pObj = shellList[vnode] + sid;
      pObj->thandle = thandle;
      pObj->sid = sid;
      pObj->vnode = vnode;
      pObj->ip = peerIp;
      tinet_ntoa(ipstr, peerIp);
      vnodeList[pObj->vnode].shellConns++;
      dTrace("vid:%d, shell connection:%d from ip:%s is created, shellConns:%d", vnode, sid, ipstr,
             vnodeList[pObj->vnode].shellConns);
    } else {
      dError("vid:%d, vnode not there, shell connection shall be closed", vnode);
      return NULL;
    }
  } else {
    if (pObj != shellList[vnode] + sid) {
      dError("vid:%d, shell connection:%d, pObj:%p is not matched with:%p", vnode, sid, pObj, shellList[vnode] + sid);
      return NULL;
    }
  }

  // if ( vnodeList[vnode].status != TSDB_STATUS_MASTER && pMsg->msgType != TSDB_MSG_TYPE_RETRIEVE ) {

#ifdef CLUSTER
  if (vnodeList[vnode].vnodeStatus != TSDB_VNODE_STATUS_MASTER) {
    taosSendSimpleRsp(thandle, pMsg->msgType + 1, TSDB_CODE_NOT_READY);
    dTrace("vid:%d sid:%d, shell msg is ignored since in state:%d", vnode, sid, vnodeList[vnode].vnodeStatus);
  } else {
#endif
    dTrace("vid:%d sid:%d, msg:%s is received pConn:%p", vnode, sid, taosMsg[pMsg->msgType], thandle);

    if (pMsg->msgType == TSDB_MSG_TYPE_QUERY) {
      vnodeProcessQueryRequest((char *)pMsg->content, pMsg->msgLen - sizeof(SIntMsg), pObj);
    } else if (pMsg->msgType == TSDB_MSG_TYPE_RETRIEVE) {
      vnodeProcessRetrieveRequest((char *)pMsg->content, pMsg->msgLen - sizeof(SIntMsg), pObj);
    } else if (pMsg->msgType == TSDB_MSG_TYPE_SUBMIT) {
      vnodeProcessShellSubmitRequest((char *)pMsg->content, pMsg->msgLen - sizeof(SIntMsg), pObj);
    } else {
      dError("%s is not processed", taosMsg[pMsg->msgType]);
    }
#ifdef CLUSTER
  }
#endif

  return pObj;
}

int vnodeInitShell() {
  int      size;
  SRpcInit rpcInit;

  size = TSDB_MAX_VNODES * sizeof(SShellObj *);
  shellList = (SShellObj **)malloc(size);
  if (shellList == NULL) return -1;
  memset(shellList, 0, size);

  int numOfThreads = tsNumOfCores * tsNumOfThreadsPerCore;
  numOfThreads = (1.0 - tsRatioOfQueryThreads) * numOfThreads / 2.0;
  if (numOfThreads < 1) numOfThreads = 1;

  memset(&rpcInit, 0, sizeof(rpcInit));
#ifdef CLUSTER  
  rpcInit.localIp = tsInternalIp;
#else
  rpcInit.localIp = "0.0.0.0";
#endif
  rpcInit.localPort = tsVnodeShellPort;
  rpcInit.label = "DND-shell";
  rpcInit.numOfThreads = numOfThreads;
  rpcInit.fp = vnodeProcessMsgFromShell;
  rpcInit.bits = TSDB_SHELL_VNODE_BITS;
  rpcInit.numOfChanns = TSDB_MAX_VNODES;
  rpcInit.sessionsPerChann = 16;
  rpcInit.idMgmt = TAOS_ID_FREE;
  rpcInit.connType = TAOS_CONN_SOCKET_TYPE_S();
  rpcInit.idleTime = tsShellActivityTimer * 2000;
  rpcInit.qhandle = rpcQhandle[0];
  rpcInit.efp = vnodeSendVpeerCfgMsg;

  pShellServer = taosOpenRpc(&rpcInit);
  if (pShellServer == NULL) {
    dError("failed to init connection to shell");
    return -1;
  }

  return 0;
}

int vnodeOpenShellVnode(int vnode) {
  if (shellList[vnode] != NULL) {
    dError("vid:%d, shell is already opened", vnode);
    return -1;
  }

  const int32_t MIN_NUM_OF_SESSIONS = 300;

  SVnodeCfg *pCfg = &vnodeList[vnode].cfg;
  int32_t sessions = (int32_t) MAX(pCfg->maxSessions * 1.1, MIN_NUM_OF_SESSIONS);

  size_t size = sessions * sizeof(SShellObj);
  shellList[vnode] = (SShellObj *)calloc(1, size);
  if (shellList[vnode] == NULL) {
    dError("vid:%d, sessions:%d, failed to allocate shellObj, size:%d", vnode, pCfg->maxSessions, size);
    return -1;
  }

  if(taosOpenRpcChannWithQ(pShellServer, vnode, sessions, rpcQhandle[(vnode+1)%tsMaxQueues]) != TSDB_CODE_SUCCESS) {
    dError("vid:%d, sessions:%d, failed to open shell", vnode, pCfg->maxSessions);
    return -1;
  }

  dTrace("vid:%d, sessions:%d, shell is opened", vnode, pCfg->maxSessions);
  return TSDB_CODE_SUCCESS;
}

static void vnodeDelayedFreeResource(void *param, void *tmrId) {
  int32_t vnode = *(int32_t*) param;
  dTrace("vid:%d, start to free resources", vnode);

  taosCloseRpcChann(pShellServer, vnode); // close connection
  tfree(shellList[vnode]);  //free SShellObj
  tfree(param);

  memset(vnodeList + vnode, 0, sizeof(SVnodeObj));
  vnodeCalcOpenVnodes();
}

void vnodeCloseShellVnode(int vnode) {
  if (shellList[vnode] == NULL) return;

  for (int i = 0; i < vnodeList[vnode].cfg.maxSessions; ++i) {
    vnodeFreeQInfo(shellList[vnode][i].qhandle, true);
  }

  int32_t* v = malloc(sizeof(int32_t));
  *v = vnode;

  /*
   * free the connection related resource after 5sec.
   * 1. The msg, as well as SRpcConn may be in the task queue, free it immediate will cause crash
   * 2. Free connection may cause *(SRpcConn*)pObj->thandle to be invalid to access.
   */
  dTrace("vid:%d, free resources in 500ms", vnode);
  taosTmrStart(vnodeDelayedFreeResource, 500, v, vnodeTmrCtrl);
}

void vnodeCleanUpShell() {
  if (pShellServer) taosCloseRpc(pShellServer);

  tfree(shellList);
}

int vnodeSendQueryRspMsg(SShellObj *pObj, int code, void *qhandle) {
  char *pMsg, *pStart;
  int   msgLen;

  pStart = taosBuildRspMsgWithSize(pObj->thandle, TSDB_MSG_TYPE_QUERY_RSP, 128);
  if (pStart == NULL) return -1;
  pMsg = pStart;

  *pMsg = code;
  pMsg++;

  *((uint64_t *)pMsg) = (uint64_t)qhandle;
  pMsg += 8;

  msgLen = pMsg - pStart;
  taosSendMsgToPeer(pObj->thandle, pStart, msgLen);

  return msgLen;
}

int vnodeSendShellSubmitRspMsg(SShellObj *pObj, int code, int numOfPoints) {
  char *pMsg, *pStart;
  int   msgLen;

  dTrace("code:%d numOfTotalPoints:%d", code, numOfPoints);
  pStart = taosBuildRspMsgWithSize(pObj->thandle, TSDB_MSG_TYPE_SUBMIT_RSP, 128);
  if (pStart == NULL) return -1;
  pMsg = pStart;

  *pMsg = code;
  pMsg++;

  *(int32_t *)pMsg = numOfPoints;
  pMsg += sizeof(numOfPoints);

  msgLen = pMsg - pStart;
  taosSendMsgToPeer(pObj->thandle, pStart, msgLen);

  return msgLen;
}

int vnodeProcessQueryRequest(char *pMsg, int msgLen, SShellObj *pObj) {
  int                ret, code = 0;
  SQueryMeterMsg *   pQueryMsg;
  SMeterSidExtInfo **pSids = NULL;
  int32_t            incNumber = 0;
  SSqlFunctionExpr * pExprs = NULL;
  SSqlGroupbyExpr *  pGroupbyExpr = NULL;
  SMeterObj **       pMeterObjList = NULL;

  pQueryMsg = (SQueryMeterMsg *)pMsg;
  if ((code = vnodeConvertQueryMeterMsg(pQueryMsg)) != TSDB_CODE_SUCCESS) {
    goto _query_over;
  }

  if (pQueryMsg->numOfSids <= 0) {
    dError("Invalid number of meters to query, numOfSids:%d", pQueryMsg->numOfSids);
    code = TSDB_CODE_INVALID_QUERY_MSG;
    goto _query_over;
  }

  if (pQueryMsg->vnode >= TSDB_MAX_VNODES || pQueryMsg->vnode < 0) {
    dTrace("qmsg:%p,vid:%d is out of range", pQueryMsg, pQueryMsg->vnode);
    code = TSDB_CODE_INVALID_TABLE_ID;
    goto _query_over;
  }

  SVnodeObj *pVnode = &vnodeList[pQueryMsg->vnode];

  if (pVnode->cfg.maxSessions == 0) {
    dError("qmsg:%p,vid:%d is not activated yet", pQueryMsg, pQueryMsg->vnode);
    vnodeSendVpeerCfgMsg(pQueryMsg->vnode);
    code = TSDB_CODE_NOT_ACTIVE_TABLE;
    goto _query_over;
  }

  if (!(pVnode->accessState & TSDB_VN_READ_ACCCESS)) {
    code = TSDB_CODE_NO_READ_ACCESS;
    goto _query_over;
  }

  if (pQueryMsg->pSidExtInfo == 0) {
    dTrace("qmsg:%p,SQueryMeterMsg wrong format", pQueryMsg);
    code = TSDB_CODE_INVALID_QUERY_MSG;
    goto _query_over;
  }

  if (pVnode->meterList == NULL) {
    dError("qmsg:%p,vid:%d has been closed", pQueryMsg, pQueryMsg->vnode);
    code = TSDB_CODE_NOT_ACTIVE_VNODE;
    goto _query_over;
  }

  pSids = (SMeterSidExtInfo **)pQueryMsg->pSidExtInfo;
  for (int32_t i = 0; i < pQueryMsg->numOfSids; ++i) {
    if (pSids[i]->sid >= pVnode->cfg.maxSessions || pSids[i]->sid < 0) {
      dTrace("qmsg:%p sid:%d is out of range, valid range:[%d,%d]", pQueryMsg, pSids[i]->sid, 0,
             pVnode->cfg.maxSessions);

      code = TSDB_CODE_INVALID_TABLE_ID;
      goto _query_over;
    }
  }

  // todo optimize for single table query process
  pMeterObjList = (SMeterObj **)calloc(pQueryMsg->numOfSids, sizeof(SMeterObj *));
  if (pMeterObjList == NULL) {
    code = TSDB_CODE_SERV_OUT_OF_MEMORY;
    goto _query_over;
  }

  //add query ref for all meters. if any meter failed to add ref, rollback whole operation and go to error
  pthread_mutex_lock(&pVnode->vmutex);
  code = vnodeIncQueryRefCount(pQueryMsg, pSids, pMeterObjList, &incNumber);
  assert(incNumber <= pQueryMsg->numOfSids);
  pthread_mutex_unlock(&pVnode->vmutex);

  if (code != TSDB_CODE_SUCCESS) {
    goto _query_over;
  }

  pExprs = vnodeCreateSqlFunctionExpr(pQueryMsg, &code);
  if (pExprs == NULL) {
    assert(code != TSDB_CODE_SUCCESS);
    goto _query_over;
  }

  pGroupbyExpr = vnodeCreateGroupbyExpr(pQueryMsg, &code);
  if ((pGroupbyExpr == NULL && pQueryMsg->numOfGroupCols != 0) || code != TSDB_CODE_SUCCESS) {
    goto _query_over;
  }

  if (pObj->qhandle) {
    dTrace("QInfo:%p %s free qhandle", pObj->qhandle, __FUNCTION__);
    vnodeFreeQInfo(pObj->qhandle, true);
    pObj->qhandle = NULL;
  }

  if (QUERY_IS_STABLE_QUERY(pQueryMsg->queryType)) {
    pObj->qhandle = vnodeQueryOnMultiMeters(pMeterObjList, pGroupbyExpr, pExprs, pQueryMsg, &code);
  } else {
    pObj->qhandle = vnodeQueryInTimeRange(pMeterObjList, pGroupbyExpr, pExprs, pQueryMsg, &code);
  }

_query_over:
  // if failed to add ref for all meters in this query, abort current query
  if (code != TSDB_CODE_SUCCESS) {
    vnodeDecQueryRefCount(pQueryMsg, pMeterObjList, incNumber);
  }

  tfree(pQueryMsg->pSqlFuncExprs);
  tfree(pMeterObjList);
  ret = vnodeSendQueryRspMsg(pObj, code, pObj->qhandle);

  free(pQueryMsg->pSidExtInfo);
  for(int32_t i = 0; i < pQueryMsg->numOfCols; ++i) {
    vnodeFreeColumnInfo(&pQueryMsg->colList[i]);
  }

  atomic_fetch_add_32(&vnodeSelectReqNum, 1);
  return ret;
}

void vnodeExecuteRetrieveReq(SSchedMsg *pSched) {
  char *     pMsg = pSched->msg;
  int        msgLen;
  SShellObj *pObj = (SShellObj *)pSched->ahandle;

  SRetrieveMeterMsg *pRetrieve;
  SRetrieveMeterRsp *pRsp;
  int                numOfRows = 0, rowSize = 0, size = 0;
  int16_t            timePrec = TSDB_TIME_PRECISION_MILLI;

  char *pStart;

  int code = 0;
  pRetrieve = (SRetrieveMeterMsg *)pMsg;
  pRetrieve->free = htons(pRetrieve->free);

  if ((pRetrieve->free & TSDB_QUERY_TYPE_FREE_RESOURCE) != TSDB_QUERY_TYPE_FREE_RESOURCE) {
    dTrace("retrieve msg, handle:%p, free:%d", pRetrieve->qhandle, pRetrieve->free);
  } else {
    dTrace("retrieve msg to free resource from client, handle:%p, free:%d", pRetrieve->qhandle, pRetrieve->free);
  }

  /*
   * in case of server restart, apps may hold qhandle created by server before restart,
   * which is actually invalid, therefore, signature check is required.
   */
  if (pRetrieve->qhandle == (uint64_t)pObj->qhandle) {
    // if free flag is set, client wants to clean the resources
    if ((pRetrieve->free & TSDB_QUERY_TYPE_FREE_RESOURCE) != TSDB_QUERY_TYPE_FREE_RESOURCE) {
      code = vnodeRetrieveQueryInfo((void *)(pRetrieve->qhandle), &numOfRows, &rowSize, &timePrec);
    }
  } else {
    dError("QInfo:%p, qhandle:%p is not matched with saved:%p", pObj->qhandle, pRetrieve->qhandle, pObj->qhandle);
    code = TSDB_CODE_INVALID_QHANDLE;
  }

  if (code == TSDB_CODE_SUCCESS) {
    size = vnodeGetResultSize((void *)(pRetrieve->qhandle), &numOfRows);
  }

  pStart = taosBuildRspMsgWithSize(pObj->thandle, TSDB_MSG_TYPE_RETRIEVE_RSP, size + 100);
  if (pStart == NULL) {
    taosSendSimpleRsp(pObj->thandle, TSDB_MSG_TYPE_RETRIEVE_RSP, TSDB_CODE_SERV_OUT_OF_MEMORY);
    goto _exit;
  }
  
  pMsg = pStart;

  *pMsg = code;
  pMsg++;

  pRsp = (SRetrieveMeterRsp *)pMsg;
  pRsp->numOfRows = htonl(numOfRows);
  pRsp->precision = htons(timePrec);

  if (code == TSDB_CODE_SUCCESS) {
    pRsp->offset = htobe64(vnodeGetOffsetVal(pRetrieve->qhandle));
    pRsp->useconds = htobe64(((SQInfo *)(pRetrieve->qhandle))->useconds);
  } else {
    pRsp->offset = 0;
    pRsp->useconds = 0;
  }

  pMsg = pRsp->data;

  if (numOfRows > 0 && code == TSDB_CODE_SUCCESS) {
    int32_t oldSize = size;
    vnodeSaveQueryResult((void *)(pRetrieve->qhandle), pRsp->data, &size);
    if (oldSize > size) {
      pRsp->compress = htons(1); // denote that the response msg is compressed
    }
  }

  pMsg += size;
  msgLen = pMsg - pStart;

  if (numOfRows == 0 && (pRetrieve->qhandle == (uint64_t)pObj->qhandle) && (code != TSDB_CODE_ACTION_IN_PROGRESS)) {
    dTrace("QInfo:%p %s free qhandle code:%d", pObj->qhandle, __FUNCTION__, code);
    vnodeFreeQInfoInQueue(pObj->qhandle);
    pObj->qhandle = NULL;
  }

  taosSendMsgToPeer(pObj->thandle, pStart, msgLen);

_exit:
  free(pSched->msg);

  return;
}

int vnodeProcessRetrieveRequest(char *pMsg, int msgLen, SShellObj *pObj) {
  SSchedMsg schedMsg;

  char *msg = malloc(msgLen);
  memcpy(msg, pMsg, msgLen);
  schedMsg.msg = msg;
  schedMsg.ahandle = pObj;
  schedMsg.fp = vnodeExecuteRetrieveReq;
  taosScheduleTask(queryQhandle, &schedMsg);

  return msgLen;
}

static int vnodeCheckSubmitBlockContext(SShellSubmitBlock *pBlocks, SVnodeObj *pVnode) {
  int32_t  sid = htonl(pBlocks->sid);
  uint64_t uid = htobe64(pBlocks->uid);

  if (sid >= pVnode->cfg.maxSessions || sid <= 0) {
    dError("sid:%d is out of range", sid);
    return TSDB_CODE_INVALID_TABLE_ID;
  }

  SMeterObj *pMeterObj = pVnode->meterList[sid];
  if (pMeterObj == NULL) {
    dError("vid:%d sid:%d, no active table", pVnode->vnode, sid);
    vnodeSendMeterCfgMsg(pVnode->vnode, sid);
    return TSDB_CODE_NOT_ACTIVE_TABLE;
  }

  if (pMeterObj->uid != uid) {
    dError("vid:%d sid:%d, meterId:%s, uid:%lld, uid in msg:%lld, uid mismatch", pVnode->vnode, sid, pMeterObj->meterId,
           pMeterObj->uid, uid);
    return TSDB_CODE_INVALID_SUBMIT_MSG;
  }

  return TSDB_CODE_SUCCESS;
}

int vnodeProcessShellSubmitRequest(char *pMsg, int msgLen, SShellObj *pObj) {
  int              code = 0, ret = 0;
  int32_t          i = 0;
  SShellSubmitMsg  shellSubmit = *(SShellSubmitMsg *)pMsg;
  SShellSubmitMsg *pSubmit = &shellSubmit;
  SShellSubmitBlock *pBlocks = NULL;

  pSubmit->vnode = htons(pSubmit->vnode);
  pSubmit->numOfSid = htonl(pSubmit->numOfSid);

  if (pSubmit->numOfSid <= 0) {
    dError("invalid num of meters:%d", pSubmit->numOfSid);
    code = TSDB_CODE_INVALID_QUERY_MSG;
    goto _submit_over;
  }

  if (pSubmit->vnode >= TSDB_MAX_VNODES || pSubmit->vnode < 0) {
    dTrace("vnode:%d is out of range", pSubmit->vnode);
    code = TSDB_CODE_INVALID_VNODE_ID;
    goto _submit_over;
  }

  SVnodeObj *pVnode = vnodeList + pSubmit->vnode;
  if (pVnode->cfg.maxSessions == 0 || pVnode->meterList == NULL) {
    dError("vid:%d is not activated for submit", pSubmit->vnode);
    vnodeSendVpeerCfgMsg(pSubmit->vnode);
    code = TSDB_CODE_NOT_ACTIVE_VNODE;
    goto _submit_over;
  }

  if (!(pVnode->accessState & TSDB_VN_WRITE_ACCCESS)) {
    code = TSDB_CODE_NO_WRITE_ACCESS;
    goto _submit_over;
  }

  if (tsAvailDataDirGB < tsMinimalDataDirGB) {
    dError("server disk space remain %.3f GB, need at least %.3f GB, stop writing", tsAvailDataDirGB, tsMinimalDataDirGB);
    code = TSDB_CODE_SERVER_NO_SPACE;
    goto _submit_over;
  }

  pObj->count = pSubmit->numOfSid;  // for import
  pObj->code = 0;                   // for import
  pObj->numOfTotalPoints = 0;       // for import

  int32_t numOfPoints = 0;
  int32_t numOfTotalPoints = 0;
  // We take current time here to avoid it in the for loop.
  TSKEY   now = taosGetTimestamp(pVnode->cfg.precision);

  pBlocks = (SShellSubmitBlock *)(pMsg + sizeof(SShellSubmitMsg));
  for (i = 0; i < pSubmit->numOfSid; ++i) {
    numOfPoints = 0;

    code = vnodeCheckSubmitBlockContext(pBlocks, pVnode);
    if (code != TSDB_CODE_SUCCESS) break;

    SMeterObj *pMeterObj = (SMeterObj *)(pVnode->meterList[htonl(pBlocks->sid)]);
    // dont include sid, vid
    int32_t subMsgLen = sizeof(pBlocks->numOfRows) + htons(pBlocks->numOfRows) * pMeterObj->bytesPerPoint;
    int32_t sversion = htonl(pBlocks->sversion);

    int32_t state = TSDB_METER_STATE_READY;
    state = vnodeSetMeterState(pMeterObj, (pSubmit->import ? TSDB_METER_STATE_IMPORTING : TSDB_METER_STATE_INSERT));

    if (state == TSDB_METER_STATE_READY) { // meter status is ready for insert/import
      if (pSubmit->import) {
        code = vnodeImportPoints(pMeterObj, (char *) &(pBlocks->numOfRows), subMsgLen, TSDB_DATA_SOURCE_SHELL, pObj,
                                 sversion, &numOfPoints, now);
        vnodeClearMeterState(pMeterObj, TSDB_METER_STATE_IMPORTING);
        pObj->numOfTotalPoints += numOfPoints;
        if (code == TSDB_CODE_SUCCESS) pObj->count--;
      } else {
        code = vnodeInsertPoints(pMeterObj, (char *) &(pBlocks->numOfRows), subMsgLen, TSDB_DATA_SOURCE_SHELL, NULL,
                                 sversion, &numOfPoints, now);
        vnodeClearMeterState(pMeterObj, TSDB_METER_STATE_INSERT);
        numOfTotalPoints += numOfPoints;
      }
      if (code != TSDB_CODE_SUCCESS) break;
    } else {
      if (vnodeIsMeterState(pMeterObj, TSDB_METER_STATE_DELETING)) {
        dTrace("vid:%d sid:%d id:%s, it is removed, state:%d", pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId,
               pMeterObj->state);
        code = TSDB_CODE_NOT_ACTIVE_TABLE;
        break;
      } else {// waiting for 300ms by default and try again
        dTrace("vid:%d sid:%d id:%s, try submit again since in state:%d", pMeterObj->vnode, pMeterObj->sid,
               pMeterObj->meterId, pMeterObj->state);

        code = TSDB_CODE_ACTION_IN_PROGRESS;
        break;
      }
    }

    pBlocks = (SShellSubmitBlock *)((char *)pBlocks + sizeof(SShellSubmitBlock) +
                                    htons(pBlocks->numOfRows) * pMeterObj->bytesPerPoint);
  }

_submit_over:
  ret = 0;
  if (pSubmit->import) {  // Import case
    if (code == TSDB_CODE_ACTION_IN_PROGRESS) {

      SBatchImportInfo *pImportInfo =
          (SBatchImportInfo *)calloc(1, sizeof(SBatchImportInfo) + msgLen - sizeof(SShellSubmitMsg));
      if (pImportInfo == NULL) {
        code = TSDB_CODE_SERV_OUT_OF_MEMORY;
        ret = vnodeSendShellSubmitRspMsg(pObj, code, pObj->numOfTotalPoints);
      } else { // Start a timer to process the next part of request
        pImportInfo->import = 1;
        pImportInfo->vnode = pSubmit->vnode;
        pImportInfo->numOfSid = pSubmit->numOfSid;
        pImportInfo->ssid = i;
        pImportInfo->pObj = pObj;
        pImportInfo->offset = ((char *)pBlocks) - (pMsg + sizeof(SShellSubmitMsg));
        assert(pImportInfo->offset >= 0);
        memcpy((void *)(pImportInfo->blks), (void *)(pMsg + sizeof(SShellSubmitMsg)), msgLen - sizeof(SShellSubmitMsg));
        taosTmrStart(vnodeProcessBatchImportTimer, 10, (void *)pImportInfo, vnodeTmrCtrl);
      }
    } else {
      if (code == TSDB_CODE_SUCCESS) assert(pObj->count == 0);
      ret = vnodeSendShellSubmitRspMsg(pObj, code, pObj->numOfTotalPoints);
    }
  } else {  // Insert case
    ret = vnodeSendShellSubmitRspMsg(pObj, code, numOfTotalPoints);
  }

  atomic_fetch_add_32(&vnodeInsertReqNum, 1);
  return ret;
}

static void vnodeProcessBatchImportTimer(void *param, void *tmrId) {
  SBatchImportInfo *pImportInfo = (SBatchImportInfo *)param;
  assert(pImportInfo != NULL && pImportInfo->import);

  int32_t i = 0, numOfPoints = 0, numOfTotalPoints = 0;
  int32_t code = TSDB_CODE_SUCCESS;

  SShellObj *        pShell = pImportInfo->pObj;
  SVnodeObj *        pVnode = &vnodeList[pImportInfo->vnode];
  SShellSubmitBlock *pBlocks = (SShellSubmitBlock *)(pImportInfo->blks + pImportInfo->offset);
  TSKEY   now = taosGetTimestamp(pVnode->cfg.precision);

  for (i = pImportInfo->ssid; i < pImportInfo->numOfSid; i++) {
    numOfPoints = 0;

    code = vnodeCheckSubmitBlockContext(pBlocks, pVnode);
    if (code != TSDB_CODE_SUCCESS) break;

    SMeterObj *pMeterObj = (SMeterObj *)(pVnode->meterList[htonl(pBlocks->sid)]);
    // dont include sid, vid
    int32_t subMsgLen = sizeof(pBlocks->numOfRows) + htons(pBlocks->numOfRows) * pMeterObj->bytesPerPoint;
    int32_t sversion = htonl(pBlocks->sversion);

    int32_t state = TSDB_METER_STATE_READY;
    state = vnodeSetMeterState(pMeterObj, TSDB_METER_STATE_IMPORTING);

    if (state == TSDB_METER_STATE_READY) {  // meter status is ready for insert/import
      code = vnodeImportPoints(pMeterObj, (char *)&(pBlocks->numOfRows), subMsgLen, TSDB_DATA_SOURCE_SHELL, pShell,
                               sversion, &numOfPoints, now);
      vnodeClearMeterState(pMeterObj, TSDB_METER_STATE_IMPORTING);
      pShell->numOfTotalPoints += numOfPoints;
      if (code != TSDB_CODE_SUCCESS) break;
      pShell->count--;
    } else {
      if (vnodeIsMeterState(pMeterObj, TSDB_METER_STATE_DELETING)) {
        dTrace("vid:%d sid:%d id:%s, it is removed, state:%d", pMeterObj->vnode, pMeterObj->sid, pMeterObj->meterId,
               pMeterObj->state);
        code = TSDB_CODE_NOT_ACTIVE_TABLE;
        break;
      } else {  // waiting for 300ms by default and try again
        dTrace("vid:%d sid:%d id:%s, try submit again since in state:%d", pMeterObj->vnode, pMeterObj->sid,
               pMeterObj->meterId, pMeterObj->state);

        code = TSDB_CODE_ACTION_IN_PROGRESS;
        break;
      }
    }

    pBlocks = (SShellSubmitBlock *)((char *)pBlocks + sizeof(SShellSubmitBlock) +
                                    htons(pBlocks->numOfRows) * pMeterObj->bytesPerPoint);
  }

  int ret = 0;
  if (code == TSDB_CODE_ACTION_IN_PROGRESS) {
    pImportInfo->ssid = i;
    pImportInfo->offset = ((char *)pBlocks) - pImportInfo->blks;
    taosTmrStart(vnodeProcessBatchImportTimer, 10, (void *)pImportInfo, vnodeTmrCtrl);
  } else {
    if (code == TSDB_CODE_SUCCESS) assert(pShell->count == 0);
    tfree(param);
    ret = vnodeSendShellSubmitRspMsg(pShell, code, pShell->numOfTotalPoints);
  }
}
