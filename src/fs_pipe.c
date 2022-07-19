/* Copyright (c) 2021 Connected Way, LLC. All rights reserved.
 * Use of this source code is governed by a Creative Commons 
 * Attribution-NoDerivatives 4.0 International license that can be
 * found in the LICENSE file.
 */
#include "ofc/types.h"
#include "ofc/handle.h"
#include "ofc/libc.h"
#include "ofc/path.h"
#include "ofc/waitq.h"
#include "ofc/thread.h"
#include "ofc/lock.h"
#include "ofc/heap.h"

#include "ofc/fs.h"
#include "ofc/fstype.h"

/**
 * \defgroup pipe Pipe File Interface
 */

/** \{ */

typedef struct
{
  OFC_UINT len ;
  OFC_INT offset ;
  OFC_CHAR buffer[1] ;
} OFC_FS_PIPE_DATA ;

struct _OFC_FS_PIPE_HALF;

typedef struct _OFC_FS_PIPE_FILE
{
  /* Since this is queued in shared memory, link must be first */
  struct _OFC_FS_PIPE_FILE *next ;
  OFC_TCHAR *name;
  struct _OFC_FS_PIPE_HALF *server ;
  struct _OFC_FS_PIPE_HALF *client ;
} OFC_FS_PIPE_FILE ;

typedef struct _OFC_FS_PIPE_HALF
{
  OFC_HANDLE hPipe ;
  OFC_HANDLE hWaitQ;
  OFC_FS_PIPE_FILE *pipe_file;
  struct _OFC_FS_PIPE_HALF *sibling ;
} OFC_FS_PIPE_HALF ;

typedef struct
{
  OFC_LOCK lock ;
  OFC_FS_PIPE_FILE *first ;
  OFC_FS_PIPE_FILE *last ;
} OFC_PIPES ;

OFC_PIPES pipes;

OFC_VOID ofc_pipe_lock (OFC_VOID)
{
  ofc_lock(pipes.lock);
}

OFC_VOID ofc_pipe_unlock (OFC_VOID)
{
  ofc_unlock(pipes.lock);
}

static OFC_VOID pipe_unlink_internal (OFC_FS_PIPE_FILE *pipe_file)
{
  OFC_FS_PIPE_FILE *curr;
  OFC_FS_PIPE_FILE *prev;
  OFC_BOOL found ;

  prev = OFC_NULL;
  found = OFC_FALSE ;

  for (curr = pipes.first ;
       curr != OFC_NULL && !found ;)
    {
      if (curr == pipe_file)
	found = OFC_TRUE ;
      else
	{
	  prev = curr ;
	  curr = curr->next;
	}
    }

  if (found)
    {
      if (prev == OFC_NULL)
	pipes.first = pipe_file->next ;
      else
	{
	  prev->next = pipe_file->next;
	}
      if (pipe_file->next == OFC_NULL)
	pipes.last = OFC_NULL ;
    }
}

OFC_VOID pipe_enqueue_internal (OFC_FS_PIPE_FILE *pipe_file)
{
  OFC_FS_PIPE_FILE *prev ;

  pipe_file->next = OFC_NULL;

  if (pipes.last == OFC_NULL)
    pipes.first = pipe_file ;
  else
    {
      prev = pipes.last ;
      prev->next = pipe_file;
    }

  pipes.last = pipe_file;
}

static OFC_HANDLE OfcFSPipeCreateFile (OFC_LPCTSTR lpFileName,
					 OFC_DWORD dwDesiredAccess,
					 OFC_DWORD dwShareMode,
					 OFC_LPSECURITY_ATTRIBUTES 
					 lpSecAttributes,
					 OFC_DWORD dwCreationDisposition,
					 OFC_DWORD dwFlagsAndAttributes,
					 OFC_HANDLE hTemplateFile)
{
  OFC_HANDLE ret ;
  OFC_FS_PIPE_FILE *pipe_file ;
  OFC_FS_PIPE_HALF *client ;
  OFC_FS_PIPE_HALF *server ;
  OFC_BOOL found ;

  ret = OFC_HANDLE_NULL ;

  /*
   * If it's create always, we create the pipe (even if there's other pipes
   * with the same name) and add it to the pipe list.  We will then
   * wait for a connection before we return
   */
  if (dwCreationDisposition == OFC_CREATE_ALWAYS ||
      dwCreationDisposition == OFC_CREATE_NEW)
    {
      pipe_file = ofc_malloc(sizeof (OFC_FS_PIPE_FILE)) ;
      if (pipe_file != OFC_NULL)
	{
	  /*
	   * We're creating the server
	   */
	  pipe_file->name = ofc_tstrdup(lpFileName) ;

	  server = ofc_malloc(sizeof (OFC_FS_PIPE_HALF)) ;
	  if (server != OFC_NULL)
	    {
	      server->hWaitQ = ofc_waitq_create();

	      server->hPipe = ofc_handle_create (OFC_HANDLE_PIPE, server) ;
	      server->pipe_file = pipe_file ;
	      server->sibling = OFC_NULL;

	      pipe_file->server = server ;
	      pipe_file->client = OFC_NULL ;

	      ofc_pipe_lock () ;
	      pipe_enqueue_internal (pipe_file) ;
	      ofc_pipe_unlock() ;

	      while (server->sibling == OFC_NULL)
		{
		  ofc_waitq_block(server->hWaitQ);
		}

	      ret = server->hPipe ;
	    }
	  else
	    {
	      ofc_free(pipe_file->name) ;
	      ofc_free(pipe_file) ;
	      ofc_thread_set_variable
		(OfcLastError, 
		 (OFC_DWORD_PTR) OFC_ERROR_NOT_ENOUGH_MEMORY) ;
	    }
	}
    }
  else
    {
      /*
       * We are opening the pipe from the client side.  We look for a pipe
       * that has no client
       */
      ofc_pipe_lock () ;
      found = OFC_FALSE ;

      for (pipe_file = pipes.first ;
	   pipe_file != OFC_NULL && !found ;)
	{
	  if (ofc_tstrcmp (pipe_file->name, 
			   lpFileName) == 0 &&
	      pipe_file->client == OFC_NULL)
	    found = OFC_TRUE ;
	  else
	    pipe_file = pipe_file->next ;
	}

      if (!found)
	{
	  ofc_thread_set_variable (OfcLastError, (OFC_DWORD_PTR) 
				 OFC_ERROR_FILE_NOT_FOUND) ;
	}
      else
	{
	  client = ofc_malloc(sizeof (OFC_FS_PIPE_HALF)) ;
	  if (client != OFC_NULL)
	    {
	      pipe_file->client = client ;
	      server = pipe_file->server ;

	      client->hWaitQ = ofc_waitq_create();
	      client->hPipe = ofc_handle_create (OFC_HANDLE_PIPE, client) ;
	      client->pipe_file = pipe_file ;
	      client->sibling = pipe_file->server ;
	      server->sibling = client ;
	      /*
	       * Set the event
	       */
	      ofc_waitq_wake(server->hWaitQ);

	      ret = client->hPipe ;
	    }
	  else
	    {
	      ofc_thread_set_variable 
		(OfcLastError, 
		 (OFC_DWORD_PTR) OFC_ERROR_NOT_ENOUGH_MEMORY) ;
	    }
	}
      ofc_pipe_unlock () ;
    }
  return (ret) ;
}

static OFC_BOOL 
OfcFSPipeCreateDirectory (OFC_LPCTSTR lpPathName,
			   OFC_LPSECURITY_ATTRIBUTES lpSecurityAttr) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

static OFC_BOOL OfcFSPipeWriteFile (OFC_HANDLE hFile,
				      OFC_LPCVOID lpBuffer,
				      OFC_DWORD nNumberOfBytesToWrite,
				      OFC_LPDWORD lpNumberOfBytesWritten,
				      OFC_HANDLE hOverlapped)
{
  OFC_BOOL ret ;
  OFC_FS_PIPE_DATA *data ;
  OFC_FS_PIPE_HALF *half ;
  OFC_FS_PIPE_HALF *sibling ;

  ret = OFC_FALSE ;

  half = ofc_handle_lock (hFile) ;
  if (half != OFC_NULL)
    {
      ofc_pipe_lock() ;
      if (half->sibling != OFC_NULL)
	{
	  sibling = half->sibling ;
	  data = ofc_malloc (sizeof (OFC_FS_PIPE_DATA) +
			     nNumberOfBytesToWrite - 1) ;
	  data->len = nNumberOfBytesToWrite ;
	  data->offset = 0 ;
	  ofc_memcpy (data->buffer, lpBuffer, nNumberOfBytesToWrite) ;

	  ofc_waitq_enqueue(sibling->hWaitQ, data);

	  if (lpNumberOfBytesWritten != OFC_NULL)
	    *lpNumberOfBytesWritten = nNumberOfBytesToWrite ;
	  ret = OFC_TRUE ;
	}
      else
	{
	  ofc_thread_set_variable (OfcLastError, 
				 (OFC_DWORD_PTR) OFC_ERROR_BROKEN_PIPE) ;
	}
      ofc_pipe_unlock() ;
      ofc_handle_unlock (hFile) ;
    }

  return (ret) ;
}

static OFC_BOOL OfcFSPipeReadFile (OFC_HANDLE hFile,
				     OFC_LPVOID lpBuffer,
				     OFC_DWORD nNumberOfBytesToRead,
				     OFC_LPDWORD lpNumberOfBytesRead,
				     OFC_HANDLE hOverlapped)
{
  OFC_BOOL ret ;
  OFC_FS_PIPE_HALF *half ;
  OFC_FS_PIPE_DATA *data ;
  OFC_INT nBytes ;

  ret = OFC_FALSE ;

  half = ofc_handle_lock (hFile) ;
  if (half != OFC_NULL)
    {
      ofc_pipe_lock() ;

      for (data = ofc_waitq_first(half->hWaitQ) ;
	   data == OFC_NULL && half->sibling != OFC_NULL ;
	   data = ofc_waitq_first(half->hWaitQ))
	{
	  ofc_pipe_unlock() ;
	  ofc_waitq_block(half->hWaitQ);
	  ofc_pipe_lock() ;
	}

      if (data == OFC_NULL)
	{
	  ofc_thread_set_variable (OfcLastError, 
				 (OFC_DWORD_PTR) OFC_ERROR_BROKEN_PIPE) ;
	}
      else
	{
	  nBytes = OFC_MIN(nNumberOfBytesToRead, data->len) ;
	  ofc_memcpy (lpBuffer, data->buffer + data->offset, nBytes) ;
	  if (lpNumberOfBytesRead != OFC_NULL)
	    *lpNumberOfBytesRead = nBytes ;
	  data->len -= nBytes ;
	  data->offset += nBytes ;
	  if (data->len == 0)
	    {
	      ofc_waitq_dequeue(half->hWaitQ);
	      ofc_free (data) ;
	    }
	  ret = OFC_TRUE ;
	}
      ofc_handle_unlock (hFile) ;
      ofc_pipe_unlock() ;
    }
  return (ret) ;
}

static OFC_BOOL OfcFSPipeCloseHandle (OFC_HANDLE hFile)
{
  OFC_BOOL ret ;
  OFC_FS_PIPE_FILE *pipe_file ;
  OFC_FS_PIPE_HALF *half ;
  OFC_FS_PIPE_HALF *sibling ;
  OFC_FS_PIPE_DATA *data ;

  ret = OFC_FALSE ;

  half = ofc_handle_lock (hFile) ;
  if (half != OFC_NULL)
    {
      ofc_pipe_lock () ;

      for (data = ofc_waitq_dequeue (half->hWaitQ) ;
	   data != OFC_NULL ;
	   data = ofc_waitq_dequeue (half->hWaitQ))
	{
	  ofc_free (data) ;
	}
      ofc_waitq_destroy(half->hWaitQ);
      half->hWaitQ = OFC_HANDLE_NULL;
      half->hPipe = OFC_HANDLE_NULL;

      if (half->sibling != OFC_NULL)
	{
	  sibling = half->sibling ;
	  sibling->sibling = OFC_NULL ;
	  ofc_waitq_wake(sibling->hWaitQ);
	}
      else
	{
	  pipe_file = half->pipe_file ;
	  if (pipe_file != OFC_NULL)
	    {
	      pipe_unlink_internal (pipe_file) ;
	      ofc_free (pipe_file->name) ;
	      ofc_free (pipe_file) ;
	    }
	}
      
      ofc_free (half) ;

      ofc_pipe_unlock () ;

      ofc_handle_destroy (hFile) ;
      ofc_handle_unlock (hFile) ;
      
      ret = OFC_TRUE ;
    }
  return (ret) ;
}

OFC_BOOL OfcFSPipeDeleteFile (OFC_LPCTSTR lpFileName) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_TRUE) ;
}

OFC_BOOL OfcFSPipeRemoveDirectory (OFC_LPCTSTR lpPathName) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_TRUE) ;
}

OFC_HANDLE OfcFSPipeFindFirstFile (OFC_LPCTSTR lpFileName,
				     OFC_LPWIN32_FIND_DATAW lpFindFileData,
				     OFC_BOOL *more) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_HANDLE_NULL) ;
}

OFC_BOOL OfcFSPipeFindNextFile (OFC_HANDLE hFindFile,
				  OFC_LPWIN32_FIND_DATAW lpFindFileData,
				  OFC_BOOL *more) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeFindClose (OFC_HANDLE hFindFile) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeFlushFileBuffers (OFC_HANDLE hFile) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_TRUE) ;
}

OFC_BOOL OfcFSPipeGetFileAttributesEx (OFC_LPCTSTR lpFileName,
					 OFC_GET_FILEEX_INFO_LEVELS 
					 fInfoLevelId,
					 OFC_LPVOID lpFileInformation) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeGetFileInformationByHandleEx 
(OFC_HANDLE hFile,
 OFC_FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
 OFC_LPVOID lpFileInformation,
 OFC_DWORD dwBufferSize) 
{
  OFC_BOOL ret ;
  OFC_FS_PIPE_HALF *half ;

  ret = OFC_FALSE ;

  half = ofc_handle_lock (hFile) ;
  if (half != OFC_NULL)
    {
      switch (FileInformationClass)
	{
	case OfcFileStandardInfo:
	  {
	    OFC_FILE_STANDARD_INFO *info ;

	    info = lpFileInformation ;
#if defined(OFC_64BIT_INTEGER)
	    info->AllocationSize = 0 ;
	    info->EndOfFile = 0 ;
#else
	    info->AllocationSize.low = 0 ;
	    info->AllocationSize.high = 0 ;
	    info->EndOfFile.low = 0 ;
	    info->EndOfFile.high = 0 ;
#endif
	    info->NumberOfLinks = 0 ;
	    info->DeletePending = OFC_FALSE ;
	    info->Directory = OFC_FALSE ;
	    ret = OFC_TRUE ;
	  }
	  break ;
	case OfcFileBasicInfo:
	  {
	    OFC_FILE_BASIC_INFO *info ;

	    info = lpFileInformation ;
#if defined(OFC_64BIT_INTEGER)
	    info->CreationTime = 0 ;
	    info->LastAccessTime = 0 ;
	    info->LastWriteTime = 0 ;
	    info->ChangeTime = 0 ;
#else
	    info->CreationTime.low = 0 ;
	    info->CreationTime.high = 0 ;
	    info->LastAccessTime.low = 0 ;
	    info->LastAccessTime.high = 0 ;
	    info->LastWriteTime.low = 0 ;
	    info->LastWriteTime.high = 0 ;
	    info->ChangeTime.low = 0 ;
	    info->ChangeTime.high = 0 ;
#endif
	    info->FileAttributes = OFC_FILE_ATTRIBUTE_NORMAL ;
	    ret = OFC_TRUE ;
	  }
	  break ;

        case OfcFileEaInfo:
          if (dwBufferSize >= sizeof(OFC_FILE_EA_INFO))
            {
              OFC_FILE_EA_INFO *lpFileEaInfo =
                (OFC_FILE_EA_INFO *) lpFileInformation;
              lpFileEaInfo->EaSize = 0;
              ret = OFC_TRUE;
            }
          break;

	default:
	  ofc_thread_set_variable (OfcLastError, 
				 (OFC_DWORD_PTR) 
				 OFC_ERROR_CALL_NOT_IMPLEMENTED) ;
	  break ;
	}
      ofc_handle_unlock (hFile) ;
    }
  return (ret) ;
}

OFC_BOOL OfcFSPipeMoveFile (OFC_LPCTSTR lpExistingFileName,
			      OFC_LPCTSTR lpNewFileName) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

static OFC_HANDLE OfcFSPipeCreateOverlapped (OFC_VOID)
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_HANDLE_NULL) ;
}

static OFC_VOID OfcFSPipeDestroyOverlapped (OFC_HANDLE hOverlapped)
{
}

static OFC_VOID OfcFSPipeSetOverlappedOffset (OFC_HANDLE hOverlapped,
						OFC_OFFT offset)
{
}

OFC_BOOL OfcFSPipeGetOverlappedResult (OFC_HANDLE hFile,
					 OFC_HANDLE hOverlapped,
					 OFC_LPDWORD 
					 lpNumberOfBytesTransferred,
					 OFC_BOOL bWait) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeSetEndOfFile (OFC_HANDLE hFile) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeSetFileAttributes (OFC_LPCTSTR lpFileName,
				       OFC_DWORD dwFileAttributes)
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_BOOL OfcFSPipeSetFileInformationByHandle (OFC_HANDLE hFile,
						OFC_FILE_INFO_BY_HANDLE_CLASS
						FileInformationClass,
						OFC_LPVOID lpFileInformation,
						OFC_DWORD dwBufferSize) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

OFC_DWORD OfcFSPipeSetFilePointer (OFC_HANDLE hFile,
				     OFC_LONG lDistanceToMove,
				     OFC_PLONG lpDistanceToMoveHigh,
				     OFC_DWORD dwMoveMethod) 
{
  return (OFC_TRUE) ;
}

static OFC_BOOL 
OfcFSPipeTransactNamedPipe (OFC_HANDLE hFile,
			     OFC_LPVOID lpInBuffer,
			     OFC_DWORD nInBufferSize,
			     OFC_LPVOID lpOutBuffer,
			     OFC_DWORD nOutBufferSize,
			     OFC_LPDWORD lpBytesRead,
			     OFC_HANDLE hOverlapped)
{
  OFC_BOOL ret ;
  OFC_FS_PIPE_DATA *data ;
  OFC_FS_PIPE_HALF *half ;
  OFC_FS_PIPE_HALF *sibling ;
  OFC_INT nBytes ;

  ret = OFC_FALSE ;

  half = ofc_handle_lock (hFile) ;
  if (half != OFC_NULL)
    {
      if (half->sibling != OFC_NULL)
	{
	  sibling = half->sibling ;
	  data = ofc_malloc (sizeof (OFC_FS_PIPE_DATA) +
			     nInBufferSize - 1) ;
	  data->len = nInBufferSize ;
	  data->offset = 0 ;
	  ofc_memcpy (data->buffer, lpInBuffer, nInBufferSize) ;

	  ofc_waitq_enqueue(sibling->hWaitQ, data);

	  for (data = ofc_waitq_dequeue (half->hWaitQ) ;
	       data == OFC_NULL ;
	       data = ofc_waitq_dequeue (half->hWaitQ))
	    {
	      ofc_waitq_block(half->hWaitQ);
	    }

	  nBytes = OFC_MIN(nOutBufferSize, data->len) ;
	  ofc_memcpy (lpOutBuffer, data->buffer, nBytes) ;
	  *lpBytesRead = nBytes ;
	  ofc_free (data) ;
	  ret = OFC_TRUE ;
	}
      else
	{
	  ofc_thread_set_variable (OfcLastError, 
				 (OFC_DWORD_PTR) OFC_ERROR_BROKEN_PIPE) ;
	}
      ofc_handle_unlock (hFile) ;
    }

  return (ret) ;
}

static OFC_BOOL 
OfcFSPipeGetDiskFreeSpace (OFC_LPCTSTR lpRootPathName,
			    OFC_LPDWORD lpSectorsPerCluster,
			    OFC_LPDWORD lpBytesPerSector,
			    OFC_LPDWORD lpNumberOfFreeClusters,
			    OFC_LPDWORD lpTotalNumberOfClusters) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

static OFC_BOOL 
OfcFSPipeGetVolumeInformation (OFC_LPCTSTR lpRootPathName,
				OFC_LPTSTR lpVolumeNameBuffer,
				OFC_DWORD nVolumeNameSize,
				OFC_LPDWORD lpVolumeSerialNumber,
				OFC_LPDWORD lpMaximumComponentLength,
				OFC_LPDWORD lpFileSystemFlags,
				OFC_LPTSTR lpFileSystemName,
				OFC_DWORD nFileSystemName) 
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_CALL_NOT_IMPLEMENTED) ;

  return (OFC_FALSE) ;
}

/**
 * Unlock a region in a file
 * 
 * \param hFile
 * File Handle to unlock 
 *
 * \param length_low
 * the low order 32 bits of the length of the region
 *
 * \param length_high
 * the high order 32 bits of the length of the region
 *
 * \param hOverlapped
 * The overlapped structure which specifies the offset
 *
 * \returns
 * OFC_TRUE if successful, OFC_FALSE otherwise
 */
static OFC_BOOL OfcFSPipeUnlockFileEx (OFC_HANDLE hFile, 
					 OFC_UINT32 length_low, 
					 OFC_UINT32 length_high,
					 OFC_HANDLE hOverlapped)
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_OPLOCK_NOT_GRANTED) ;

  return (OFC_FALSE) ;
}

/**
 * Lock a region of a file
 * 
 * \param hFile
 * Handle to file to unlock region in 
 *
 * \param flags
 * Flags for lock
 *
 * \param length_low
 * Low order 32 bits of length of region
 *
 * \param length_high
 * High order 32 bits of length of region
 *
 * \param hOverlapped
 * Pointer to overlapped structure containing offset of region
 *
 * \returns
 * OFC_TRUE if successful, OFC_FALSE otherwise
 */
static OFC_BOOL OfcFSPipeLockFileEx (OFC_HANDLE hFile, OFC_DWORD flags,
				       OFC_DWORD length_low, 
				       OFC_DWORD length_high,
				       OFC_HANDLE hOverlapped)
{
  ofc_thread_set_variable (OfcLastError, 
			 (OFC_DWORD_PTR) OFC_ERROR_OPLOCK_NOT_GRANTED) ;

  return (OFC_FALSE) ;
}

static OFC_FILE_FSINFO OfcFSPipeInfo =
  {
    &OfcFSPipeCreateFile,
    &OfcFSPipeDeleteFile,
    &OfcFSPipeFindFirstFile,
    &OfcFSPipeFindNextFile,
    &OfcFSPipeFindClose,
    &OfcFSPipeFlushFileBuffers,
    &OfcFSPipeGetFileAttributesEx,
    &OfcFSPipeGetFileInformationByHandleEx,
    &OfcFSPipeMoveFile,
    &OfcFSPipeGetOverlappedResult,
    &OfcFSPipeCreateOverlapped,
    &OfcFSPipeDestroyOverlapped,
    &OfcFSPipeSetOverlappedOffset,
    &OfcFSPipeSetEndOfFile,
    &OfcFSPipeSetFileAttributes,
    &OfcFSPipeSetFileInformationByHandle,
    &OfcFSPipeSetFilePointer,
    &OfcFSPipeWriteFile,
    &OfcFSPipeReadFile,
    &OfcFSPipeCloseHandle,
    &OfcFSPipeTransactNamedPipe,
    &OfcFSPipeGetDiskFreeSpace,
    &OfcFSPipeGetVolumeInformation,
    &OfcFSPipeCreateDirectory,
    &OfcFSPipeRemoveDirectory,
    &OfcFSPipeUnlockFileEx,
    &OfcFSPipeLockFileEx,
    OFC_NULL,
    OFC_NULL
  } ;

OFC_VOID OfcFSPipeStartup (OFC_VOID)
{
  OFC_PATH *path ;

  pipes.lock = ofc_lock_init();

  pipes.first = OFC_NULL ;
  pipes.last = OFC_NULL ;

  ofc_fs_register (OFC_FST_PIPE, &OfcFSPipeInfo) ;
  /*
   * Create a path for the IPC service
   */
  path = ofc_path_createW(TSTR("/")) ;
  if (path == OFC_NULL)
    ofc_printf ("Couldn't Create IPC Path\n") ;
  else
    ofc_path_add_mapW(TSTR("IPC"), TSTR("IPC Path"), path, OFC_FST_PIPE,
		     OFC_TRUE) ;
}

OFC_VOID OfcFSPipeShutdown (OFC_VOID)
{
  OFC_FS_PIPE_FILE *pipe_file ;

  ofc_lock(pipes.lock);
  for (pipe_file = pipes.first ;
       pipe_file != OFC_NULL;
       pipe_file = pipes.first)
    {
      pipes.first = pipe_file->next;
      ofc_unlock(pipes.lock);

      if (pipe_file->client != OFC_NULL)
	{
	  OFC_FS_PIPE_HALF *client ;
	  client = pipe_file->client ;
	  if (client->hPipe != OFC_HANDLE_NULL)
	    {
	      OfcFSPipeCloseHandle(client->hPipe);
	      client->hPipe = OFC_HANDLE_NULL;
	    }
	}
      if (pipe_file->server != OFC_NULL)
	{
	  OFC_FS_PIPE_HALF *server ;
	  server = pipe_file->server ;
	  if (server->hPipe != OFC_HANDLE_NULL)
	    {
	      OfcFSPipeCloseHandle(server->hPipe);
	      server->hPipe = OFC_HANDLE_NULL;
	    }
	}
      ofc_lock(pipes.lock);
    }
  ofc_unlock (pipes.lock);
  ofc_lock_destroy(pipes.lock);

  ofc_path_delete_mapW (TSTR("IPC"));
}

/** \} */
