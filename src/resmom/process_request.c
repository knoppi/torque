/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/

#include <pbs_config.h>   /* the master config generated by configure */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <memory.h>
#include <time.h>
#include <pwd.h>
#include <sys/param.h>
#if HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif
#if HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef ENABLE_PTHREADS
#include <pthread.h>
#endif

#include "libpbs.h"
#include "pbs_error.h"
#include "server_limits.h"
#include "pbs_nodes.h"
#include "list_link.h"
#include "attribute.h"
#include "pbs_job.h"
#include "server.h"
#include "credential.h"
#include "batch_request.h"
#include "net_connect.h"
#include "log.h"
#include "svrfunc.h"
#include "pbs_proto.h"
#include "csv.h"
#include "u_tree.h"
#include "threadpool.h"
#include "dis.h"

/*
 * mom_process_request - this function gets, checks, and invokes the proper
 * function to deal with a batch request received over the network.
 *
 * All data encoding/decoding dependencies are moved to a lower level
 * routine.  That routine must convert
 * the data received into the internal server structures regardless of
 * the data structures used by the encode/decode routines.  This provides
 * the "protocol" and "protocol generation tool" freedom to the bulk
 * of the server.
 */

/* global data items */

tlist_head svr_requests;

extern struct    connection svr_conn[];

extern struct    credential conn_credent[PBS_NET_MAX_CONNECTIONS];

extern struct server server;
extern char      server_host[];
extern tlist_head svr_newjobs;

extern time_t    time_now;
extern char  *msg_err_noqueue;
extern char  *msg_err_malloc;
extern char  *msg_request;

extern AvlTree okclients;

extern int LOGLEVEL;

/* private functions local to this file */

#ifdef MUNGE_AUTH
static const int munge_on = 1;
#else
static const int munge_on = 0;
#endif 

static void close_client(int sfds);
static void freebr_manage(struct rq_manage *);
static void freebr_cpyfile(struct rq_cpyfile *);
static void close_quejob(int sfds);

/* END private prototypes */

/* request processing prototypes */
void req_quejob(struct batch_request *preq);
void req_jobcredential(struct batch_request *preq);
void req_jobscript(struct batch_request *preq);
void req_rdytocommit(struct batch_request *preq);
void req_commit(struct batch_request *preq);
void req_holdjob(struct batch_request *preq);
void req_deletejob(struct batch_request *preq);
void req_rerunjob(struct batch_request *preq);
void req_modifyjob(struct batch_request *preq);

void req_shutdown(struct batch_request *preq);
void req_signaljob(struct batch_request *preq);
void req_mvjobfile(struct batch_request *preq);
void req_checkpointjob(struct batch_request *preq);
void req_messagejob(struct batch_request *preq);

/* END request processing prototypes */


/*
 * mom_process_request - process an request from the network:
 *
 * - Call function to read in the request and decode it.
 * - Validate requesting host and user.
 * - Call function to process request based on type.
 *    That function MUST free the request by calling free_br()
 *
 * NOTE: the caller functions hold the mutex for the connection
 */

void mom_process_request(

  int sfds) /* file descriptor (socket) to get request */

  {
  char                 *id = "mom_process_request";

  int                   rc;

  struct batch_request *request = NULL;

  time_now = time(NULL);

  request = alloc_br(0);

  request->rq_conn = sfds;

  /*
   * Read in the request and decode it to the internal request structure.
   */

  DIS_tcp_setup(sfds);
  rc = dis_request_read(sfds, request);

  if (rc == -1)
    {
    /* FAILURE */

    /* premature end of file */

    close_client(sfds);

    free_br(request);

    return;
    }

  if ((rc == PBSE_SYSTEM) || (rc == PBSE_INTERNAL))
    {
    /* FAILURE */

    /* read error, likely cannot send reply so just disconnect */

    /* ??? not sure about this ??? */

    close_client(sfds);

    free_br(request);

    return;
    }

  if (rc > 0)
    {
    /* FAILURE */

    /*
     * request didn't decode, either garbage or unknown
     * request type, in either case, return reject-reply
     */

    req_reject(rc, 0, request, NULL, "cannot decode message");

    close_client(sfds);

    return;
    }

  if (get_connecthost(sfds, request->rq_host, PBS_MAXHOSTNAME) != 0)
    {
    char tmpLine[MAXLINE];

    sprintf(log_buffer, "%s: %lu",
            pbse_to_txt(PBSE_BADHOST),
            get_connectaddr(sfds,FALSE));

    log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_REQUEST, "", log_buffer);

    snprintf(tmpLine, sizeof(tmpLine), "cannot determine hostname for connection from %lu",
             get_connectaddr(sfds,FALSE));

    req_reject(PBSE_BADHOST, 0, request, NULL, tmpLine);

    return;
    }

  if (LOGLEVEL >= 1)
    {
    sprintf(
      log_buffer,
      msg_request,
      reqtype_to_txt(request->rq_type),
      request->rq_user,
      request->rq_host,
      sfds);

    log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_REQUEST, "", log_buffer);
    }

  /* is the request from a host acceptable to the server */

    {
    /*extern tree *okclients; */

    extern void mom_server_update_receive_time_by_ip(u_long ipaddr, const char *cmd);

    /* check connecting host against allowed list of ok clients */

    if (LOGLEVEL >= 6)
      {
      sprintf(log_buffer, "request type %s from host %s received",
        reqtype_to_txt(request->rq_type),
        request->rq_host);

      log_record(
        PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        id,
        log_buffer);
      }

    if (!AVL_is_in_tree_no_port_compare(svr_conn[sfds].cn_addr, 0, okclients))
      {
      sprintf(log_buffer, "request type %s from host %s rejected (host not authorized)",
        reqtype_to_txt(request->rq_type),
        request->rq_host);

      log_record(
        PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        id,
        log_buffer);

      req_reject(PBSE_BADHOST, 0, request, NULL, "request not authorized");

      close_client(sfds);

      return;
      }

    if (LOGLEVEL >= 3)
      {
      sprintf(log_buffer, "request type %s from host %s allowed",
        reqtype_to_txt(request->rq_type),
        request->rq_host);

      log_record(
        PBSEVENT_JOB,
        PBS_EVENTCLASS_JOB,
        id,
        log_buffer);
      }

    mom_server_update_receive_time_by_ip(svr_conn[sfds].cn_addr, reqtype_to_txt(request->rq_type));
    }    /* END BLOCK */

  request->rq_fromsvr = 1;

  request->rq_perm =
    ATR_DFLAG_USRD | ATR_DFLAG_USWR |
    ATR_DFLAG_OPRD | ATR_DFLAG_OPWR |
    ATR_DFLAG_MGRD | ATR_DFLAG_MGWR |
    ATR_DFLAG_SvWR | ATR_DFLAG_MOM;

  /*
   * dispatch the request to the correct processing function.
   * The processing function must call reply_send() to free
   * the request struture.
   */

  dispatch_request(sfds, request);

  return;
  }  /* END mom_process_request() */


/*
 * dispatch_request - Determine the request type and invoke the corresponding
 * function.  The function will perform the request action and return the
 * reply.  The function MUST also reply and free the request by calling
 * reply_send().
 *
 * @see mom_process_request() - parent
 */

void dispatch_request(

  int          sfds,    /* I */
  struct batch_request *request) /* I */

  {
  char *id = "dispatch_request";

  if (LOGLEVEL >= 5)
    {
    sprintf(log_buffer,"dispatching request %s on sd=%d",
      reqtype_to_txt(request->rq_type),
      sfds);

    log_record(
      PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      id,
      log_buffer);
    }

  switch (request->rq_type)
    {
    case PBS_BATCH_QueueJob:

      net_add_close_func(sfds, close_quejob);

      req_quejob(request);
      
      break;

    case PBS_BATCH_JobCred:

      req_jobcredential(request);

      break;

    case PBS_BATCH_jobscript:

      req_jobscript(request);
      
      break;

    case PBS_BATCH_RdytoCommit:

      req_rdytocommit(request);
      
      break;

    case PBS_BATCH_Commit:

      req_commit(request);

      net_add_close_func(sfds, (void (*)())0);

      break;

    case PBS_BATCH_DeleteJob:

      req_deletejob(request);
      
      break;

    case PBS_BATCH_HoldJob:
      
      req_holdjob(request);

      break;

    case PBS_BATCH_CheckpointJob:

      req_checkpointjob(request);

      break;

    case PBS_BATCH_MessJob:

      req_messagejob(request);

      break;

    case PBS_BATCH_AsyModifyJob:

    case PBS_BATCH_ModifyJob:

      req_modifyjob(request);

      break;

    case PBS_BATCH_Rerun:

      req_rerunjob(request);

      break;

    case PBS_BATCH_Shutdown:

      req_shutdown(request);

      break;

    case PBS_BATCH_SignalJob:

    case PBS_BATCH_AsySignalJob:

      req_signaljob(request);

      break;

    case PBS_BATCH_MvJobFile:

      req_mvjobfile(request);

      break;

    case PBS_BATCH_StatusJob:

      req_stat_job(request);

      break;

    case PBS_BATCH_ReturnFiles:

      req_returnfiles(request);

      break;

    case PBS_BATCH_CopyFiles:

      req_cpyfile(request);

      break;

    case PBS_BATCH_DelFiles:

      req_delfile(request);

      break;

    default:

      req_reject(PBSE_UNKREQ, 0, request, NULL, NULL);

      close_client(sfds);

      break;
    }  /* END switch (request->rq_type) */

  return;
  }  /* END dispatch_request() */





/*
 * close_client - close a connection to a client, also "inactivate"
 *    any outstanding batch requests on that connection.
 */

static void close_client(

  int sfds)  /* connection socket */

  {

  struct batch_request *preq;

  close_conn(sfds); /* close the connection */

  preq = (struct batch_request *)GET_NEXT(svr_requests);

  while (preq != NULL)
    {
    /* list of outstanding requests */

    if (preq->rq_conn == sfds)
      preq->rq_conn = -1;

    if (preq->rq_orgconn == sfds)
      preq->rq_orgconn = -1;

    preq = (struct batch_request *)GET_NEXT(preq->rq_link);
    }

  return;
  }  /* END close_client() */




/*
 * alloc_br - allocate and clear a batch_request structure
 */

struct batch_request *alloc_br(

        int type)

  {

  struct batch_request *req = NULL;

  req = (struct batch_request *)malloc(sizeof(struct batch_request));

  if (req == NULL)
    {
    log_err(errno, "alloc_br", msg_err_malloc);

    return(NULL);
    }

  memset((void *)req, (int)0, sizeof(struct batch_request));

  req->rq_type = type;

  req->rq_conn = -1;  /* indicate not connected */
  req->rq_orgconn = -1;  /* indicate not connected */
  req->rq_time = time_now;
  req->rq_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
  req->rq_noreply = FALSE;  /* indicate reply is needed */

  CLEAR_LINK(req->rq_link);

  append_link(&svr_requests, &req->rq_link, req);

  return(req);
  }





/*
 * close_quejob - locate and deal with the new job that was being recevied
 *    when the net connection closed.
 */

static void close_quejob(

  int sfds)

  {
  job *pjob;

  job *npjob;

  pjob = (job *)GET_NEXT(svr_newjobs);

  while (pjob != NULL)
    {
    npjob = GET_NEXT(pjob->ji_alljobs);

    if (pjob->ji_qs.ji_un.ji_newt.ji_fromsock == sfds)
      {
      if (pjob->ji_qs.ji_substate != JOB_SUBSTATE_TRANSICM)
        {
        /* delete the job */
        delete_link(&pjob->ji_alljobs);

        job_purge(pjob);

        pjob = NULL;
        }
      
      break;
      }  /* END if (..) */

    pjob = npjob;
    }

  return;
  }  /* END close_quejob() */





/*
 * free_br - free space allocated to a batch_request structure
 * including any sub-structures
 */

void free_br(

  struct batch_request *preq)

  {
  delete_link(&preq->rq_link);

  reply_free(&preq->rq_reply);

  if (preq->rq_extend) 
      free(preq->rq_extend);

  switch (preq->rq_type)
    {
    case PBS_BATCH_QueueJob:

      free_attrlist(&preq->rq_ind.rq_queuejob.rq_attr);

      break;

    case PBS_BATCH_JobCred:

      if (preq->rq_ind.rq_jobcred.rq_data)
        free(preq->rq_ind.rq_jobcred.rq_data);

      break;

    case PBS_BATCH_MvJobFile:

    case PBS_BATCH_jobscript:

      if (preq->rq_ind.rq_jobfile.rq_data)
        free(preq->rq_ind.rq_jobfile.rq_data);

      break;

    case PBS_BATCH_HoldJob:

      freebr_manage(&preq->rq_ind.rq_hold.rq_orig);

      break;

    case PBS_BATCH_CheckpointJob:

      freebr_manage(&preq->rq_ind.rq_manager);

      break;

    case PBS_BATCH_MessJob:

      if (preq->rq_ind.rq_message.rq_text)
        free(preq->rq_ind.rq_message.rq_text);

      break;

    case PBS_BATCH_ModifyJob:

    case PBS_BATCH_AsyModifyJob:

      freebr_manage(&preq->rq_ind.rq_modify);

      break;

    case PBS_BATCH_StatusJob:

    case PBS_BATCH_StatusQue:

    case PBS_BATCH_StatusNode:

    case PBS_BATCH_StatusSvr:
      /* DIAGTODO: handle PBS_BATCH_StatusDiag */

      free_attrlist(&preq->rq_ind.rq_status.rq_attr);

      break;

    case PBS_BATCH_JobObit:

      free_attrlist(&preq->rq_ind.rq_jobobit.rq_attr);

      break;

    case PBS_BATCH_CopyFiles:

    case PBS_BATCH_DelFiles:

      freebr_cpyfile(&preq->rq_ind.rq_cpyfile);

      break;

    default:

      /* NO-OP */

      break;
    }  /* END switch (preq->rq_type) */

  free(preq);

  return;
  }  /* END free_br() */





static void freebr_manage(

  struct rq_manage *pmgr)

  {
  free_attrlist(&pmgr->rq_attr);

  return;
  }  /* END freebr_manage() */




static void freebr_cpyfile(

  struct rq_cpyfile *pcf)

  {

  struct rqfpair *ppair;

  while ((ppair = (struct rqfpair *)GET_NEXT(pcf->rq_pair)) != NULL)
    {
    delete_link(&ppair->fp_link);

    if (ppair->fp_local != NULL)
      free(ppair->fp_local);

    if (ppair->fp_rmt != NULL)
      free(ppair->fp_rmt);

    free(ppair);
    }

  return;
  }  /* END freebr_cpyfile() */





/* END process_requests.c */


