/* Copyright (c) 2021 Connected Way, LLC. All rights reserved.
 * Use of this source code is governed by a Creative Commons 
 * Attribution-NoDerivatives 4.0 International license that can be
 * found in the LICENSE file.
 */
#if !defined(__OFC_FSPIPE_H__)
#define __OFC_FSPIPE_H__

#include "ofc/types.h"
#include "ofc/file.h"

/**
 * \defgroup BlueFSPipe Pipe File System Dependent Support
 * \ingroup BlueFS
 */

/** \{ */

#if defined(__cplusplus)
extern "C"
{
#endif
  OFC_VOID OfcFSPipeStartup (OFC_VOID) ;
  OFC_VOID OfcFSPipeShutdown (OFC_VOID);
#if defined(__cplusplus)
}
#endif

#endif
/** \} */
