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
/* PBS_status.c

 The function that underlies all the status requests
*/

#include <pbs_config.h>   /* the master config generated by configure */

#include <string.h>
#include <stdio.h>
#include "libpbs.h"
#include "server_limits.h"


static struct batch_status *alloc_bs();

struct batch_status *PBSD_status(

  int           c,           /* I - socket descriptor */
  int           function,    /* I - ??? */
  int          *local_errno, /* O */
  char         *id,          /* I - object id (optional) */
  struct attrl *attrib,      /* I */
  char         *extend)      /* I/O */

  {
  int rc;

  /* send the status request */

  if (id == NULL)
    id =(char *)""; /* set to null string for encoding */

  rc = PBSD_status_put(c, function, id, attrib, extend);

  if (rc != 0)
    {
    *local_errno = PBSE_PROTOCOL;

    if (extend != NULL)
      strcpy(extend, "timeout");

    return((struct batch_status *)NULL);
    }

  /* get the status reply */

  *local_errno = 0;

  return(PBSD_status_get(local_errno, c));
  }  /* END PBSD_status() */




struct batch_status *PBSD_status_get(

  int *local_errno, /* O */
  int  c)           /* I */

  {
  struct brp_cmdstat  *stp; /* pointer to a returned status record */
  struct batch_status *bsp = NULL;
  struct batch_status *rbsp = (struct batch_status *)NULL;
  struct batch_reply  *reply;
  int i;
  
  if ((c < 0) || 
      (c >= PBS_NET_MAX_CONNECTIONS))
    {
    return(NULL);
    }

  pthread_mutex_lock(connection[c].ch_mutex);

  *local_errno = 0;

  /* read reply from stream into presentation element */
  reply = PBSD_rdrpy(local_errno, c);

  if (reply == NULL)
    {
    *local_errno = PBSE_PROTOCOL;
    }
  else if ((reply->brp_choice != BATCH_REPLY_CHOICE_NULL) &&
           (reply->brp_choice != BATCH_REPLY_CHOICE_Text) &&
           (reply->brp_choice != BATCH_REPLY_CHOICE_Status))
    {
    *local_errno = PBSE_PROTOCOL;
    }
  else if (connection[c].ch_errno != 0)
    {
    char tmpLine[1024];

    if (*local_errno == 0)
      *local_errno = PBSE_PROTOCOL;

    sprintf(tmpLine, "PBS API connection failed with pbserrno=%d\n",
            connection[c].ch_errno);
    }
  else
    {
    /* query is successful */

    /* have zero or more attrl structs to decode here */

    stp = reply->brp_un.brp_statc;

    i = 0;

    *local_errno = 0;

    while (stp != NULL)
      {
      if (i++ == 0)
        {
        bsp = alloc_bs();

        rbsp = bsp;

        if (bsp == (struct batch_status *)NULL)
          {
          *local_errno = PBSE_SYSTEM;

          break;
          }
        }
      else
        {
        bsp->next = alloc_bs();

        bsp = bsp->next;

        if (bsp == (struct batch_status *)NULL)
          {
          *local_errno = PBSE_SYSTEM;

          break;
          }
        }

      bsp->name = strdup(stp->brp_objname);

      bsp->attribs = stp->brp_attrl;

      if (stp->brp_attrl != NULL)
        stp->brp_attrl = NULL;

      bsp->next = (struct batch_status *)NULL;

      stp = stp->brp_stlink;
      }  /* END while (stp != NULL) */

    if (*local_errno != 0)
      {
      /* destroy corrupt results */

      pbs_statfree(rbsp);

      rbsp = (struct batch_status *)NULL;
      }
    }    /* END else */

  pthread_mutex_unlock(connection[c].ch_mutex);

  PBSD_FreeReply(reply);

  return(rbsp);
  }  /* END PBSD_status_get() */




/* Allocate a batch status reply structure */

static struct batch_status *
      alloc_bs(void)

  {

  struct batch_status *bsp;

  /* allocate memory */

  bsp = MH(struct batch_status);

  if (bsp != NULL)
    {
    bsp->next = (struct batch_status *)NULL;
    bsp->name = (char *)NULL;
    bsp->attribs = (struct attrl *)NULL;
    bsp->text = (char *)NULL;
    }

  return(bsp);
  }

/* END PBSD_status.c */


