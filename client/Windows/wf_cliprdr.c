/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Windows Clipboard Redirection
 *
 * Copyright 2012 Jason Champion
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#define CINTERFACE
#define COBJMACROS

#include <ole2.h>
#include <shlobj.h>
#include <windows.h>
#include <winuser.h>

#include <winpr/assert.h>
#include <winpr/library.h>

#include <winpr/crt.h>
#include <winpr/tchar.h>
#include <winpr/stream.h>

#include <freerdp/log.h>
#include <freerdp/client/cliprdr.h>

#include <strsafe.h>

#include "wf_cliprdr.h"

#define TAG CLIENT_TAG("windows")

#ifdef WITH_DEBUG_CLIPRDR
#define DEBUG_CLIPRDR(...) WLog_DBG(TAG, __VA_ARGS__)
#else
#define DEBUG_CLIPRDR(...) \
	do                     \
	{                      \
	} while (0)
#endif

typedef BOOL(WINAPI* fnAddClipboardFormatListener)(HWND hwnd);
typedef BOOL(WINAPI* fnRemoveClipboardFormatListener)(HWND hwnd);
typedef BOOL(WINAPI* fnGetUpdatedClipboardFormats)(PUINT lpuiFormats, UINT cFormats,
                                                   PUINT pcFormatsOut);

typedef struct
{
	UINT32 remote_format_id;
	UINT32 local_format_id;
	WCHAR* name;
} formatMapping;

typedef struct
{
	IEnumFORMATETC iEnumFORMATETC;

	LONG m_lRefCount;
	LONG m_nIndex;
	LONG m_nNumFormats;
	FORMATETC* m_pFormatEtc;
} CliprdrEnumFORMATETC;

typedef struct
{
	IStream iStream;

	LONG m_lRefCount;
	ULONG m_lIndex;
	ULARGE_INTEGER m_lSize;
	ULARGE_INTEGER m_lOffset;
	FILEDESCRIPTORW m_Dsc;
	void* m_pData;
} CliprdrStream;

typedef struct
{
	IDataObject iDataObject;

	LONG m_lRefCount;
	FORMATETC* m_pFormatEtc;
	STGMEDIUM* m_pStgMedium;
	ULONG m_nNumFormats;
	ULONG m_nStreams;
	IStream** m_pStream;
	void* m_pData;
} CliprdrDataObject;

typedef struct
{
	wfContext* wfc;
	rdpChannels* channels;
	CliprdrClientContext* context;

	BOOL sync;
	UINT32 capabilities;

	size_t map_size;
	size_t map_capacity;
	formatMapping* format_mappings;

	UINT32 requestedFormatId;

	HWND hwnd;
	HANDLE hmem;
	HANDLE thread;
	HANDLE response_data_event;

	LPDATAOBJECT data_obj;
	ULONG req_fsize;
	char* req_fdata;
	HANDLE req_fevent;

	size_t nFiles;
	size_t file_array_size;
	WCHAR** file_names;
	FILEDESCRIPTORW** fileDescriptor;

	BOOL legacyApi;
	HMODULE hUser32;
	HWND hWndNextViewer;
	fnAddClipboardFormatListener AddClipboardFormatListener;
	fnRemoveClipboardFormatListener RemoveClipboardFormatListener;
	fnGetUpdatedClipboardFormats GetUpdatedClipboardFormats;
} wfClipboard;

#define WM_CLIPRDR_MESSAGE (WM_USER + 156)
#define OLE_SETCLIPBOARD 1

static BOOL wf_create_file_obj(wfClipboard* cliprdrrdr, IDataObject** ppDataObject);
static void wf_destroy_file_obj(IDataObject* instance);
static UINT32 get_remote_format_id(wfClipboard* clipboard, UINT32 local_format);
static UINT cliprdr_send_data_request(wfClipboard* clipboard, UINT32 format);
static UINT cliprdr_send_lock(wfClipboard* clipboard);
static UINT cliprdr_send_unlock(wfClipboard* clipboard);
static UINT cliprdr_send_request_filecontents(wfClipboard* clipboard, const void* streamid,
                                              ULONG index, UINT32 flag, UINT64 position,
                                              ULONG request);

static void CliprdrDataObject_Delete(CliprdrDataObject* instance);

static CliprdrEnumFORMATETC* CliprdrEnumFORMATETC_New(ULONG nFormats, FORMATETC* pFormatEtc);
static void CliprdrEnumFORMATETC_Delete(CliprdrEnumFORMATETC* instance);

static void CliprdrStream_Delete(CliprdrStream* instance);

static BOOL try_open_clipboard(HWND hwnd)
{
	for (size_t x = 0; x < 10; x++)
	{
		if (OpenClipboard(hwnd))
			return TRUE;
		Sleep(10);
	}
	return FALSE;
}

/**
 * IStream
 */

static HRESULT STDMETHODCALLTYPE CliprdrStream_QueryInterface(IStream* This, REFIID riid,
                                                              void** ppvObject)
{
	if (IsEqualIID(riid, &IID_IStream) || IsEqualIID(riid, &IID_IUnknown))
	{
		IStream_AddRef(This);
		*ppvObject = This;
		return S_OK;
	}
	else
	{
		*ppvObject = 0;
		return E_NOINTERFACE;
	}
}

static ULONG STDMETHODCALLTYPE CliprdrStream_AddRef(IStream* This)
{
	CliprdrStream* instance = (CliprdrStream*)This;

	if (!instance)
		return 0;

	return InterlockedIncrement(&instance->m_lRefCount);
}

static ULONG STDMETHODCALLTYPE CliprdrStream_Release(IStream* This)
{
	LONG count;
	CliprdrStream* instance = (CliprdrStream*)This;

	if (!instance)
		return 0;

	count = InterlockedDecrement(&instance->m_lRefCount);

	if (count == 0)
	{
		CliprdrStream_Delete(instance);
		return 0;
	}
	else
	{
		return count;
	}
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Read(IStream* This, void* pv, ULONG cb,
                                                    ULONG* pcbRead)
{
	int ret;
	CliprdrStream* instance = (CliprdrStream*)This;
	wfClipboard* clipboard;

	if (!pv || !pcbRead || !instance)
		return E_INVALIDARG;

	clipboard = (wfClipboard*)instance->m_pData;
	*pcbRead = 0;

	if (instance->m_lOffset.QuadPart >= instance->m_lSize.QuadPart)
		return S_FALSE;

	ret = cliprdr_send_request_filecontents(clipboard, (void*)This, instance->m_lIndex,
	                                        FILECONTENTS_RANGE, instance->m_lOffset.QuadPart, cb);

	if (ret < 0)
		return E_FAIL;

	if (clipboard->req_fdata)
	{
		CopyMemory(pv, clipboard->req_fdata, clipboard->req_fsize);
		free(clipboard->req_fdata);
	}

	*pcbRead = clipboard->req_fsize;
	instance->m_lOffset.QuadPart += clipboard->req_fsize;

	if (clipboard->req_fsize < cb)
		return S_FALSE;

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Write(IStream* This, const void* pv, ULONG cb,
                                                     ULONG* pcbWritten)
{
	(void)This;
	(void)pv;
	(void)cb;
	(void)pcbWritten;
	return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Seek(IStream* This, LARGE_INTEGER dlibMove,
                                                    DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition)
{
	ULONGLONG newoffset;
	CliprdrStream* instance = (CliprdrStream*)This;

	if (!instance)
		return E_INVALIDARG;

	newoffset = instance->m_lOffset.QuadPart;

	switch (dwOrigin)
	{
		case STREAM_SEEK_SET:
			newoffset = dlibMove.QuadPart;
			break;

		case STREAM_SEEK_CUR:
			newoffset += dlibMove.QuadPart;
			break;

		case STREAM_SEEK_END:
			newoffset = instance->m_lSize.QuadPart + dlibMove.QuadPart;
			break;

		default:
			return E_INVALIDARG;
	}

	if (newoffset < 0 || newoffset >= instance->m_lSize.QuadPart)
		return E_FAIL;

	instance->m_lOffset.QuadPart = newoffset;

	if (plibNewPosition)
		plibNewPosition->QuadPart = instance->m_lOffset.QuadPart;

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_SetSize(IStream* This, ULARGE_INTEGER libNewSize)
{
	(void)This;
	(void)libNewSize;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_CopyTo(IStream* This, IStream* pstm,
                                                      ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
                                                      ULARGE_INTEGER* pcbWritten)
{
	(void)This;
	(void)pstm;
	(void)cb;
	(void)pcbRead;
	(void)pcbWritten;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Commit(IStream* This, DWORD grfCommitFlags)
{
	(void)This;
	(void)grfCommitFlags;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Revert(IStream* This)
{
	(void)This;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_LockRegion(IStream* This, ULARGE_INTEGER libOffset,
                                                          ULARGE_INTEGER cb, DWORD dwLockType)
{
	(void)This;
	(void)libOffset;
	(void)cb;
	(void)dwLockType;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_UnlockRegion(IStream* This, ULARGE_INTEGER libOffset,
                                                            ULARGE_INTEGER cb, DWORD dwLockType)
{
	(void)This;
	(void)libOffset;
	(void)cb;
	(void)dwLockType;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Stat(IStream* This, STATSTG* pstatstg,
                                                    DWORD grfStatFlag)
{
	CliprdrStream* instance = (CliprdrStream*)This;

	if (!instance)
		return E_INVALIDARG;

	if (pstatstg == NULL)
		return STG_E_INVALIDPOINTER;

	ZeroMemory(pstatstg, sizeof(STATSTG));

	switch (grfStatFlag)
	{
		case STATFLAG_DEFAULT:
			return STG_E_INSUFFICIENTMEMORY;

		case STATFLAG_NONAME:
			pstatstg->cbSize.QuadPart = instance->m_lSize.QuadPart;
			pstatstg->grfLocksSupported = LOCK_EXCLUSIVE;
			pstatstg->grfMode = GENERIC_READ;
			pstatstg->grfStateBits = 0;
			pstatstg->type = STGTY_STREAM;
			break;

		case STATFLAG_NOOPEN:
			return STG_E_INVALIDFLAG;

		default:
			return STG_E_INVALIDFLAG;
	}

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrStream_Clone(IStream* This, IStream** ppstm)
{
	(void)This;
	(void)ppstm;
	return E_NOTIMPL;
}

static CliprdrStream* CliprdrStream_New(ULONG index, void* pData, const FILEDESCRIPTORW* dsc)
{
	IStream* iStream;
	BOOL success = FALSE;
	BOOL isDir = FALSE;
	CliprdrStream* instance;
	wfClipboard* clipboard = (wfClipboard*)pData;
	instance = (CliprdrStream*)calloc(1, sizeof(CliprdrStream));

	if (instance)
	{
		instance->m_Dsc = *dsc;
		iStream = &instance->iStream;
		iStream->lpVtbl = (IStreamVtbl*)calloc(1, sizeof(IStreamVtbl));

		if (iStream->lpVtbl)
		{
			iStream->lpVtbl->QueryInterface = CliprdrStream_QueryInterface;
			iStream->lpVtbl->AddRef = CliprdrStream_AddRef;
			iStream->lpVtbl->Release = CliprdrStream_Release;
			iStream->lpVtbl->Read = CliprdrStream_Read;
			iStream->lpVtbl->Write = CliprdrStream_Write;
			iStream->lpVtbl->Seek = CliprdrStream_Seek;
			iStream->lpVtbl->SetSize = CliprdrStream_SetSize;
			iStream->lpVtbl->CopyTo = CliprdrStream_CopyTo;
			iStream->lpVtbl->Commit = CliprdrStream_Commit;
			iStream->lpVtbl->Revert = CliprdrStream_Revert;
			iStream->lpVtbl->LockRegion = CliprdrStream_LockRegion;
			iStream->lpVtbl->UnlockRegion = CliprdrStream_UnlockRegion;
			iStream->lpVtbl->Stat = CliprdrStream_Stat;
			iStream->lpVtbl->Clone = CliprdrStream_Clone;
			instance->m_lRefCount = 1;
			instance->m_lIndex = index;
			instance->m_pData = pData;
			instance->m_lOffset.QuadPart = 0;

			if (instance->m_Dsc.dwFlags & FD_ATTRIBUTES)
			{
				if (instance->m_Dsc.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					isDir = TRUE;
			}

			if (((instance->m_Dsc.dwFlags & FD_FILESIZE) == 0) && !isDir)
			{
				/* get content size of this stream */
				if (cliprdr_send_request_filecontents(clipboard, (void*)instance,
				                                      instance->m_lIndex, FILECONTENTS_SIZE, 0,
				                                      8) == CHANNEL_RC_OK)
				{
					success = TRUE;
				}

				instance->m_lSize.QuadPart = *((LONGLONG*)clipboard->req_fdata);
				free(clipboard->req_fdata);
			}
			else
			{
				instance->m_lSize.QuadPart =
				    ((UINT64)instance->m_Dsc.nFileSizeHigh << 32) | instance->m_Dsc.nFileSizeLow;
				success = TRUE;
			}
		}
	}

	if (!success)
	{
		CliprdrStream_Delete(instance);
		instance = NULL;
	}

	return instance;
}

void CliprdrStream_Delete(CliprdrStream* instance)
{
	if (instance)
	{
		free(instance->iStream.lpVtbl);
		free(instance);
	}
}

/**
 * IDataObject
 */

static LONG cliprdr_lookup_format(CliprdrDataObject* instance, FORMATETC* pFormatEtc)
{
	if (!instance || !pFormatEtc)
		return -1;

	for (ULONG i = 0; i < instance->m_nNumFormats; i++)
	{
		if ((pFormatEtc->tymed & instance->m_pFormatEtc[i].tymed) &&
		    pFormatEtc->cfFormat == instance->m_pFormatEtc[i].cfFormat &&
		    pFormatEtc->dwAspect & instance->m_pFormatEtc[i].dwAspect)
		{
			return (LONG)i;
		}
	}

	return -1;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_QueryInterface(IDataObject* This, REFIID riid,
                                                                  void** ppvObject)
{
	(void)This;

	if (!ppvObject)
		return E_INVALIDARG;

	if (IsEqualIID(riid, &IID_IDataObject) || IsEqualIID(riid, &IID_IUnknown))
	{
		IDataObject_AddRef(This);
		*ppvObject = This;
		return S_OK;
	}
	else
	{
		*ppvObject = 0;
		return E_NOINTERFACE;
	}
}

static ULONG STDMETHODCALLTYPE CliprdrDataObject_AddRef(IDataObject* This)
{
	CliprdrDataObject* instance = (CliprdrDataObject*)This;

	if (!instance)
		return E_INVALIDARG;

	return InterlockedIncrement(&instance->m_lRefCount);
}

static ULONG STDMETHODCALLTYPE CliprdrDataObject_Release(IDataObject* This)
{
	LONG count;
	CliprdrDataObject* instance = (CliprdrDataObject*)This;

	if (!instance)
		return E_INVALIDARG;

	count = InterlockedDecrement(&instance->m_lRefCount);

	if (count == 0)
	{
		CliprdrDataObject_Delete(instance);
		return 0;
	}
	else
		return count;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_GetData(IDataObject* This, FORMATETC* pFormatEtc,
                                                           STGMEDIUM* pMedium)
{
	LONG idx;
	CliprdrDataObject* instance = (CliprdrDataObject*)This;
	wfClipboard* clipboard;

	if (!pFormatEtc || !pMedium || !instance)
		return E_INVALIDARG;

	clipboard = (wfClipboard*)instance->m_pData;

	if (!clipboard)
		return E_INVALIDARG;

	if ((idx = cliprdr_lookup_format(instance, pFormatEtc)) == -1)
		return DV_E_FORMATETC;

	pMedium->tymed = instance->m_pFormatEtc[idx].tymed;
	pMedium->pUnkForRelease = 0;

	if (instance->m_pFormatEtc[idx].cfFormat == RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW))
	{
		FILEGROUPDESCRIPTOR* dsc;
		DWORD remote = get_remote_format_id(clipboard, instance->m_pFormatEtc[idx].cfFormat);

		if (cliprdr_send_data_request(clipboard, remote) != 0)
			return E_UNEXPECTED;

		pMedium->u.hGlobal = clipboard->hmem;
		/* points to a FILEGROUPDESCRIPTOR structure */
		/* GlobalLock returns a pointer to the first byte of the memory block,
		 * in which is a FILEGROUPDESCRIPTOR structure, whose first UINT member
		 * is the number of FILEDESCRIPTOR's */
		dsc = (FILEGROUPDESCRIPTOR*)GlobalLock(clipboard->hmem);
		instance->m_nStreams = dsc->cItems;
		GlobalUnlock(clipboard->hmem);

		if (instance->m_nStreams > 0)
		{
			if (!instance->m_pStream)
			{
				instance->m_pStream = (LPSTREAM*)calloc(instance->m_nStreams, sizeof(LPSTREAM));

				if (instance->m_pStream)
				{
					for (ULONG i = 0; i < instance->m_nStreams; i++)
					{
						instance->m_pStream[i] =
						    (IStream*)CliprdrStream_New(i, clipboard, &dsc->fgd[i]);

						if (!instance->m_pStream[i])
							return E_OUTOFMEMORY;
					}
				}
			}
		}

		if (!instance->m_pStream)
		{
			if (clipboard->hmem)
			{
				GlobalFree(clipboard->hmem);
				clipboard->hmem = NULL;
			}

			pMedium->u.hGlobal = NULL;
			return E_OUTOFMEMORY;
		}
	}
	else if (instance->m_pFormatEtc[idx].cfFormat == RegisterClipboardFormat(CFSTR_FILECONTENTS))
	{
		if ((pFormatEtc->lindex >= 0) && ((ULONG)pFormatEtc->lindex < instance->m_nStreams))
		{
			pMedium->u.pstm = instance->m_pStream[pFormatEtc->lindex];
			IDataObject_AddRef(instance->m_pStream[pFormatEtc->lindex]);
		}
		else
			return E_INVALIDARG;
	}
	else
		return E_UNEXPECTED;

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_GetDataHere(IDataObject* This,
                                                               FORMATETC* pformatetc,
                                                               STGMEDIUM* pmedium)
{
	(void)This;
	(void)pformatetc;
	(void)pmedium;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_QueryGetData(IDataObject* This,
                                                                FORMATETC* pformatetc)
{
	CliprdrDataObject* instance = (CliprdrDataObject*)This;

	if (!pformatetc)
		return E_INVALIDARG;

	if (cliprdr_lookup_format(instance, pformatetc) == -1)
		return DV_E_FORMATETC;

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_GetCanonicalFormatEtc(IDataObject* This,
                                                                         FORMATETC* pformatectIn,
                                                                         FORMATETC* pformatetcOut)
{
	(void)This;
	(void)pformatectIn;

	if (!pformatetcOut)
		return E_INVALIDARG;

	pformatetcOut->ptd = NULL;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_SetData(IDataObject* This, FORMATETC* pformatetc,
                                                           STGMEDIUM* pmedium, BOOL fRelease)
{
	(void)This;
	(void)pformatetc;
	(void)pmedium;
	(void)fRelease;
	return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_EnumFormatEtc(IDataObject* This,
                                                                 DWORD dwDirection,
                                                                 IEnumFORMATETC** ppenumFormatEtc)
{
	CliprdrDataObject* instance = (CliprdrDataObject*)This;

	if (!instance || !ppenumFormatEtc)
		return E_INVALIDARG;

	if (dwDirection == DATADIR_GET)
	{
		*ppenumFormatEtc = (IEnumFORMATETC*)CliprdrEnumFORMATETC_New(instance->m_nNumFormats,
		                                                             instance->m_pFormatEtc);
		return (*ppenumFormatEtc) ? S_OK : E_OUTOFMEMORY;
	}
	else
	{
		return E_NOTIMPL;
	}
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_DAdvise(IDataObject* This, FORMATETC* pformatetc,
                                                           DWORD advf, IAdviseSink* pAdvSink,
                                                           DWORD* pdwConnection)
{
	(void)This;
	(void)pformatetc;
	(void)advf;
	(void)pAdvSink;
	(void)pdwConnection;
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_DUnadvise(IDataObject* This, DWORD dwConnection)
{
	(void)This;
	(void)dwConnection;
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE CliprdrDataObject_EnumDAdvise(IDataObject* This,
                                                               IEnumSTATDATA** ppenumAdvise)
{
	(void)This;
	(void)ppenumAdvise;
	return OLE_E_ADVISENOTSUPPORTED;
}

static CliprdrDataObject* CliprdrDataObject_New(FORMATETC* fmtetc, STGMEDIUM* stgmed, ULONG count,
                                                void* data)
{
	CliprdrDataObject* instance;
	IDataObject* iDataObject;
	instance = (CliprdrDataObject*)calloc(1, sizeof(CliprdrDataObject));

	if (!instance)
		goto error;

	iDataObject = &instance->iDataObject;
	iDataObject->lpVtbl = (IDataObjectVtbl*)calloc(1, sizeof(IDataObjectVtbl));

	if (!iDataObject->lpVtbl)
		goto error;

	iDataObject->lpVtbl->QueryInterface = CliprdrDataObject_QueryInterface;
	iDataObject->lpVtbl->AddRef = CliprdrDataObject_AddRef;
	iDataObject->lpVtbl->Release = CliprdrDataObject_Release;
	iDataObject->lpVtbl->GetData = CliprdrDataObject_GetData;
	iDataObject->lpVtbl->GetDataHere = CliprdrDataObject_GetDataHere;
	iDataObject->lpVtbl->QueryGetData = CliprdrDataObject_QueryGetData;
	iDataObject->lpVtbl->GetCanonicalFormatEtc = CliprdrDataObject_GetCanonicalFormatEtc;
	iDataObject->lpVtbl->SetData = CliprdrDataObject_SetData;
	iDataObject->lpVtbl->EnumFormatEtc = CliprdrDataObject_EnumFormatEtc;
	iDataObject->lpVtbl->DAdvise = CliprdrDataObject_DAdvise;
	iDataObject->lpVtbl->DUnadvise = CliprdrDataObject_DUnadvise;
	iDataObject->lpVtbl->EnumDAdvise = CliprdrDataObject_EnumDAdvise;
	instance->m_lRefCount = 1;
	instance->m_nNumFormats = count;
	instance->m_pData = data;
	instance->m_nStreams = 0;
	instance->m_pStream = NULL;

	if (count > 0)
	{
		instance->m_pFormatEtc = (FORMATETC*)calloc(count, sizeof(FORMATETC));

		if (!instance->m_pFormatEtc)
			goto error;

		instance->m_pStgMedium = (STGMEDIUM*)calloc(count, sizeof(STGMEDIUM));

		if (!instance->m_pStgMedium)
			goto error;

		for (ULONG i = 0; i < count; i++)
		{
			instance->m_pFormatEtc[i] = fmtetc[i];
			instance->m_pStgMedium[i] = stgmed[i];
		}
	}

	return instance;
error:
	CliprdrDataObject_Delete(instance);
	return NULL;
}

void CliprdrDataObject_Delete(CliprdrDataObject* instance)
{
	if (instance)
	{
		free(instance->iDataObject.lpVtbl);
		free(instance->m_pFormatEtc);
		free(instance->m_pStgMedium);

		if (instance->m_pStream)
		{
			for (ULONG i = 0; i < instance->m_nStreams; i++)
				CliprdrStream_Release(instance->m_pStream[i]);

			free(instance->m_pStream);
		}

		free(instance);
	}
}

static BOOL wf_create_file_obj(wfClipboard* clipboard, IDataObject** ppDataObject)
{
	FORMATETC fmtetc[2] = { 0 };
	STGMEDIUM stgmeds[2] = { 0 };

	if (!ppDataObject)
		return FALSE;

	fmtetc[0].cfFormat = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
	fmtetc[0].dwAspect = DVASPECT_CONTENT;

	fmtetc[0].tymed = TYMED_HGLOBAL;
	stgmeds[0].tymed = TYMED_HGLOBAL;

	fmtetc[1].cfFormat = RegisterClipboardFormat(CFSTR_FILECONTENTS);
	fmtetc[1].dwAspect = DVASPECT_CONTENT;

	fmtetc[1].tymed = TYMED_ISTREAM;
	stgmeds[1].tymed = TYMED_ISTREAM;

	*ppDataObject = (IDataObject*)CliprdrDataObject_New(fmtetc, stgmeds, 2, clipboard);
	return (*ppDataObject) ? TRUE : FALSE;
}

static void wf_destroy_file_obj(IDataObject* instance)
{
	if (instance)
		IDataObject_Release(instance);
}

/**
 * IEnumFORMATETC
 */

static void cliprdr_format_deep_copy(FORMATETC* dest, FORMATETC* source)
{
	*dest = *source;

	if (source->ptd)
	{
		dest->ptd = (DVTARGETDEVICE*)CoTaskMemAlloc(sizeof(DVTARGETDEVICE));

		if (dest->ptd)
			*(dest->ptd) = *(source->ptd);
	}
}

static HRESULT STDMETHODCALLTYPE CliprdrEnumFORMATETC_QueryInterface(IEnumFORMATETC* This,
                                                                     REFIID riid, void** ppvObject)
{
	(void)This;

	if (IsEqualIID(riid, &IID_IEnumFORMATETC) || IsEqualIID(riid, &IID_IUnknown))
	{
		IEnumFORMATETC_AddRef(This);
		*ppvObject = This;
		return S_OK;
	}
	else
	{
		*ppvObject = 0;
		return E_NOINTERFACE;
	}
}

static ULONG STDMETHODCALLTYPE CliprdrEnumFORMATETC_AddRef(IEnumFORMATETC* This)
{
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance)
		return 0;

	return InterlockedIncrement(&instance->m_lRefCount);
}

static ULONG STDMETHODCALLTYPE CliprdrEnumFORMATETC_Release(IEnumFORMATETC* This)
{
	LONG count;
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance)
		return 0;

	count = InterlockedDecrement(&instance->m_lRefCount);

	if (count == 0)
	{
		CliprdrEnumFORMATETC_Delete(instance);
		return 0;
	}
	else
	{
		return count;
	}
}

static HRESULT STDMETHODCALLTYPE CliprdrEnumFORMATETC_Next(IEnumFORMATETC* This, ULONG celt,
                                                           FORMATETC* rgelt, ULONG* pceltFetched)
{
	ULONG copied = 0;
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance || !celt || !rgelt)
		return E_INVALIDARG;

	while ((instance->m_nIndex < instance->m_nNumFormats) && (copied < celt))
	{
		cliprdr_format_deep_copy(&rgelt[copied++], &instance->m_pFormatEtc[instance->m_nIndex++]);
	}

	if (pceltFetched != 0)
		*pceltFetched = copied;

	return (copied == celt) ? S_OK : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE CliprdrEnumFORMATETC_Skip(IEnumFORMATETC* This, ULONG celt)
{
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance)
		return E_INVALIDARG;

	if (instance->m_nIndex + (LONG)celt > instance->m_nNumFormats)
		return E_FAIL;

	instance->m_nIndex += celt;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrEnumFORMATETC_Reset(IEnumFORMATETC* This)
{
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance)
		return E_INVALIDARG;

	instance->m_nIndex = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE CliprdrEnumFORMATETC_Clone(IEnumFORMATETC* This,
                                                            IEnumFORMATETC** ppEnum)
{
	CliprdrEnumFORMATETC* instance = (CliprdrEnumFORMATETC*)This;

	if (!instance || !ppEnum)
		return E_INVALIDARG;

	*ppEnum =
	    (IEnumFORMATETC*)CliprdrEnumFORMATETC_New(instance->m_nNumFormats, instance->m_pFormatEtc);

	if (!*ppEnum)
		return E_OUTOFMEMORY;

	((CliprdrEnumFORMATETC*)*ppEnum)->m_nIndex = instance->m_nIndex;
	return S_OK;
}

CliprdrEnumFORMATETC* CliprdrEnumFORMATETC_New(ULONG nFormats, FORMATETC* pFormatEtc)
{
	CliprdrEnumFORMATETC* instance;
	IEnumFORMATETC* iEnumFORMATETC;

	if ((nFormats != 0) && !pFormatEtc)
		return NULL;

	instance = (CliprdrEnumFORMATETC*)calloc(1, sizeof(CliprdrEnumFORMATETC));

	if (!instance)
		goto error;

	iEnumFORMATETC = &instance->iEnumFORMATETC;
	iEnumFORMATETC->lpVtbl = (IEnumFORMATETCVtbl*)calloc(1, sizeof(IEnumFORMATETCVtbl));

	if (!iEnumFORMATETC->lpVtbl)
		goto error;

	iEnumFORMATETC->lpVtbl->QueryInterface = CliprdrEnumFORMATETC_QueryInterface;
	iEnumFORMATETC->lpVtbl->AddRef = CliprdrEnumFORMATETC_AddRef;
	iEnumFORMATETC->lpVtbl->Release = CliprdrEnumFORMATETC_Release;
	iEnumFORMATETC->lpVtbl->Next = CliprdrEnumFORMATETC_Next;
	iEnumFORMATETC->lpVtbl->Skip = CliprdrEnumFORMATETC_Skip;
	iEnumFORMATETC->lpVtbl->Reset = CliprdrEnumFORMATETC_Reset;
	iEnumFORMATETC->lpVtbl->Clone = CliprdrEnumFORMATETC_Clone;
	instance->m_lRefCount = 1;
	instance->m_nIndex = 0;
	instance->m_nNumFormats = nFormats;

	if (nFormats > 0)
	{
		instance->m_pFormatEtc = (FORMATETC*)calloc(nFormats, sizeof(FORMATETC));

		if (!instance->m_pFormatEtc)
			goto error;

		for (ULONG i = 0; i < nFormats; i++)
			cliprdr_format_deep_copy(&instance->m_pFormatEtc[i], &pFormatEtc[i]);
	}

	return instance;
error:
	CliprdrEnumFORMATETC_Delete(instance);
	return NULL;
}

void CliprdrEnumFORMATETC_Delete(CliprdrEnumFORMATETC* instance)
{
	if (instance)
	{
		free(instance->iEnumFORMATETC.lpVtbl);

		if (instance->m_pFormatEtc)
		{
			for (LONG i = 0; i < instance->m_nNumFormats; i++)
			{
				if (instance->m_pFormatEtc[i].ptd)
					CoTaskMemFree(instance->m_pFormatEtc[i].ptd);
			}

			free(instance->m_pFormatEtc);
		}

		free(instance);
	}
}

/***********************************************************************************/

static UINT32 get_local_format_id_by_name(wfClipboard* clipboard, const TCHAR* format_name)
{
	formatMapping* map;
	WCHAR* unicode_name;
#if !defined(UNICODE)
	size_t size;
#endif

	if (!clipboard || !format_name)
		return 0;

#if defined(UNICODE)
	unicode_name = _wcsdup(format_name);
#else
	size = _tcslen(format_name);
	unicode_name = calloc(size + 1, sizeof(WCHAR));

	if (!unicode_name)
		return 0;

	MultiByteToWideChar(CP_OEMCP, 0, format_name, strlen(format_name), unicode_name, size);
#endif

	if (!unicode_name)
		return 0;

	for (size_t i = 0; i < clipboard->map_size; i++)
	{
		map = &clipboard->format_mappings[i];

		if (map->name)
		{
			if (wcscmp(map->name, unicode_name) == 0)
			{
				free(unicode_name);
				return map->local_format_id;
			}
		}
	}

	free(unicode_name);
	return 0;
}

static INLINE BOOL file_transferring(wfClipboard* clipboard)
{
	return get_local_format_id_by_name(clipboard, CFSTR_FILEDESCRIPTORW) ? TRUE : FALSE;
}

static UINT32 get_remote_format_id(wfClipboard* clipboard, UINT32 local_format)
{
	formatMapping* map;

	if (!clipboard)
		return 0;

	for (UINT32 i = 0; i < clipboard->map_size; i++)
	{
		map = &clipboard->format_mappings[i];

		if (map->local_format_id == local_format)
			return map->remote_format_id;
	}

	return local_format;
}

static void map_ensure_capacity(wfClipboard* clipboard)
{
	if (!clipboard)
		return;

	if (clipboard->map_size >= clipboard->map_capacity)
	{
		size_t new_size;
		formatMapping* new_map;
		new_size = clipboard->map_capacity * 2;
		new_map =
		    (formatMapping*)realloc(clipboard->format_mappings, sizeof(formatMapping) * new_size);

		if (!new_map)
			return;

		clipboard->format_mappings = new_map;
		clipboard->map_capacity = new_size;
	}
}

static BOOL clear_format_map(wfClipboard* clipboard)
{
	formatMapping* map;

	if (!clipboard)
		return FALSE;

	if (clipboard->format_mappings)
	{
		for (size_t i = 0; i < clipboard->map_capacity; i++)
		{
			map = &clipboard->format_mappings[i];
			map->remote_format_id = 0;
			map->local_format_id = 0;
			free(map->name);
			map->name = NULL;
		}
	}

	clipboard->map_size = 0;
	return TRUE;
}

static UINT cliprdr_send_tempdir(wfClipboard* clipboard)
{
	CLIPRDR_TEMP_DIRECTORY tempDirectory;

	if (!clipboard)
		return -1;

	if (GetEnvironmentVariableA("TEMP", tempDirectory.szTempDir, sizeof(tempDirectory.szTempDir)) ==
	    0)
		return -1;

	return clipboard->context->TempDirectory(clipboard->context, &tempDirectory);
}

static BOOL cliprdr_GetUpdatedClipboardFormats(wfClipboard* clipboard, PUINT lpuiFormats,
                                               UINT cFormats, PUINT pcFormatsOut)
{
	UINT index = 0;
	UINT format = 0;
	BOOL clipboardOpen = FALSE;

	if (!clipboard->legacyApi)
		return clipboard->GetUpdatedClipboardFormats(lpuiFormats, cFormats, pcFormatsOut);

	clipboardOpen = try_open_clipboard(clipboard->hwnd);

	if (!clipboardOpen)
	{
		*pcFormatsOut = 0;
		return TRUE; /* Other app holding clipboard */
	}

	while (index < cFormats)
	{
		format = EnumClipboardFormats(format);

		if (!format)
			break;

		lpuiFormats[index] = format;
		index++;
	}

	*pcFormatsOut = index;
	CloseClipboard();
	return TRUE;
}

static UINT cliprdr_send_format_list(wfClipboard* clipboard)
{
	UINT rc;
	int count = 0;
	UINT32 numFormats = 0;
	UINT32 formatId = 0;
	char formatName[1024];
	CLIPRDR_FORMAT* formats = NULL;
	CLIPRDR_FORMAT_LIST formatList = { 0 };

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	/* Ignore if other app is holding clipboard */
	if (try_open_clipboard(clipboard->hwnd))
	{
		count = CountClipboardFormats();
		numFormats = (UINT32)count;
		formats = (CLIPRDR_FORMAT*)calloc(numFormats, sizeof(CLIPRDR_FORMAT));

		if (!formats)
		{
			CloseClipboard();
			return CHANNEL_RC_NO_MEMORY;
		}

		{
			UINT32 index = 0;

			if (IsClipboardFormatAvailable(CF_HDROP))
			{
				formats[index++].formatId = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW);
				formats[index++].formatId = RegisterClipboardFormat(CFSTR_FILECONTENTS);
			}
			else
			{
				while ((formatId = EnumClipboardFormats(formatId)) != 0)
					formats[index++].formatId = formatId;
			}

			numFormats = index;
		}

		if (!CloseClipboard())
		{
			free(formats);
			return ERROR_INTERNAL_ERROR;
		}

		for (UINT index = 0; index < numFormats; index++)
		{
			if (GetClipboardFormatNameA(formats[index].formatId, formatName, sizeof(formatName)))
			{
				formats[index].formatName = _strdup(formatName);
			}
		}
	}

	formatList.numFormats = numFormats;
	formatList.formats = formats;
	formatList.common.msgType = CB_FORMAT_LIST;
	rc = clipboard->context->ClientFormatList(clipboard->context, &formatList);

	for (UINT index = 0; index < numFormats; index++)
		free(formats[index].formatName);

	free(formats);
	return rc;
}

static UINT cliprdr_send_data_request(wfClipboard* clipboard, UINT32 formatId)
{
	UINT rc;
	UINT32 remoteFormatId;
	CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientFormatDataRequest)
		return ERROR_INTERNAL_ERROR;

	remoteFormatId = get_remote_format_id(clipboard, formatId);

	formatDataRequest.requestedFormatId = remoteFormatId;
	clipboard->requestedFormatId = formatId;
	rc = clipboard->context->ClientFormatDataRequest(clipboard->context, &formatDataRequest);

	if (WaitForSingleObject(clipboard->response_data_event, INFINITE) != WAIT_OBJECT_0)
		rc = ERROR_INTERNAL_ERROR;
	else if (!ResetEvent(clipboard->response_data_event))
		rc = ERROR_INTERNAL_ERROR;

	return rc;
}

UINT cliprdr_send_request_filecontents(wfClipboard* clipboard, const void* streamid, ULONG index,
                                       UINT32 flag, UINT64 position, ULONG nreq)
{
	UINT rc;
	CLIPRDR_FILE_CONTENTS_REQUEST fileContentsRequest;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientFileContentsRequest)
		return ERROR_INTERNAL_ERROR;

	fileContentsRequest.streamId = (UINT32)(ULONG_PTR)streamid;
	fileContentsRequest.listIndex = index;
	fileContentsRequest.dwFlags = flag;
	fileContentsRequest.nPositionLow = position & 0xFFFFFFFF;
	fileContentsRequest.nPositionHigh = (position >> 32) & 0xFFFFFFFF;
	fileContentsRequest.cbRequested = nreq;
	fileContentsRequest.clipDataId = 0;
	fileContentsRequest.common.msgFlags = 0;
	rc = clipboard->context->ClientFileContentsRequest(clipboard->context, &fileContentsRequest);

	if (WaitForSingleObject(clipboard->req_fevent, INFINITE) != WAIT_OBJECT_0)
		rc = ERROR_INTERNAL_ERROR;
	else if (!ResetEvent(clipboard->req_fevent))
		rc = ERROR_INTERNAL_ERROR;

	return rc;
}

static UINT cliprdr_send_response_filecontents(wfClipboard* clipboard, UINT32 streamId, UINT32 size,
                                               BYTE* data)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE fileContentsResponse;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientFileContentsResponse)
		return ERROR_INTERNAL_ERROR;

	fileContentsResponse.streamId = streamId;
	fileContentsResponse.cbRequested = size;
	fileContentsResponse.requestedData = data;
	fileContentsResponse.common.msgFlags = CB_RESPONSE_OK;
	return clipboard->context->ClientFileContentsResponse(clipboard->context,
	                                                      &fileContentsResponse);
}

static LRESULT CALLBACK cliprdr_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	static wfClipboard* clipboard = NULL;

	switch (Msg)
	{
		case WM_CREATE:
			DEBUG_CLIPRDR("info: WM_CREATE");
			clipboard = (wfClipboard*)((CREATESTRUCT*)lParam)->lpCreateParams;
			clipboard->hwnd = hWnd;

			if (!clipboard->legacyApi)
				clipboard->AddClipboardFormatListener(hWnd);
			else
				clipboard->hWndNextViewer = SetClipboardViewer(hWnd);

			break;

		case WM_CLOSE:
			DEBUG_CLIPRDR("info: WM_CLOSE");

			if (!clipboard->legacyApi)
				clipboard->RemoveClipboardFormatListener(hWnd);

			break;

		case WM_DESTROY:
			if (clipboard->legacyApi)
				ChangeClipboardChain(hWnd, clipboard->hWndNextViewer);

			break;

		case WM_CLIPBOARDUPDATE:
			DEBUG_CLIPRDR("info: WM_CLIPBOARDUPDATE");

			if (clipboard->sync)
			{
				if ((GetClipboardOwner() != clipboard->hwnd) &&
				    (S_FALSE == OleIsCurrentClipboard(clipboard->data_obj)))
				{
					if (clipboard->hmem)
					{
						GlobalFree(clipboard->hmem);
						clipboard->hmem = NULL;
					}

					cliprdr_send_format_list(clipboard);
				}
			}

			break;

		case WM_RENDERALLFORMATS:
			DEBUG_CLIPRDR("info: WM_RENDERALLFORMATS");

			/* discard all contexts in clipboard */
			if (!try_open_clipboard(clipboard->hwnd))
			{
				DEBUG_CLIPRDR("OpenClipboard failed with 0x%x", GetLastError());
				break;
			}

			EmptyClipboard();
			CloseClipboard();
			break;

		case WM_RENDERFORMAT:
			DEBUG_CLIPRDR("info: WM_RENDERFORMAT");

			if (cliprdr_send_data_request(clipboard, (UINT32)wParam) != 0)
			{
				DEBUG_CLIPRDR("error: cliprdr_send_data_request failed.");
				break;
			}

			if (!SetClipboardData((UINT)wParam, clipboard->hmem))
			{
				DEBUG_CLIPRDR("SetClipboardData failed with 0x%x", GetLastError());

				if (clipboard->hmem)
				{
					GlobalFree(clipboard->hmem);
					clipboard->hmem = NULL;
				}
			}

			/* Note: GlobalFree() is not needed when success */
			break;

		case WM_DRAWCLIPBOARD:
			if (clipboard->legacyApi)
			{
				if ((GetClipboardOwner() != clipboard->hwnd) &&
				    (S_FALSE == OleIsCurrentClipboard(clipboard->data_obj)))
				{
					cliprdr_send_format_list(clipboard);
				}

				SendMessage(clipboard->hWndNextViewer, Msg, wParam, lParam);
			}

			break;

		case WM_CHANGECBCHAIN:
			if (clipboard->legacyApi)
			{
				HWND hWndCurrViewer = (HWND)wParam;
				HWND hWndNextViewer = (HWND)lParam;

				if (hWndCurrViewer == clipboard->hWndNextViewer)
					clipboard->hWndNextViewer = hWndNextViewer;
				else if (clipboard->hWndNextViewer)
					SendMessage(clipboard->hWndNextViewer, Msg, wParam, lParam);
			}

			break;

		case WM_CLIPRDR_MESSAGE:
			DEBUG_CLIPRDR("info: WM_CLIPRDR_MESSAGE");

			switch (wParam)
			{
				case OLE_SETCLIPBOARD:
					DEBUG_CLIPRDR("info: OLE_SETCLIPBOARD");

					if (wf_create_file_obj(clipboard, &clipboard->data_obj))
					{
						if (OleSetClipboard(clipboard->data_obj) != S_OK)
						{
							wf_destroy_file_obj(clipboard->data_obj);
							clipboard->data_obj = NULL;
						}
					}

					break;

				default:
					break;
			}

			break;

		case WM_DESTROYCLIPBOARD:
		case WM_ASKCBFORMATNAME:
		case WM_HSCROLLCLIPBOARD:
		case WM_PAINTCLIPBOARD:
		case WM_SIZECLIPBOARD:
		case WM_VSCROLLCLIPBOARD:
		default:
			return DefWindowProc(hWnd, Msg, wParam, lParam);
	}

	return 0;
}

static int create_cliprdr_window(wfClipboard* clipboard)
{
	WNDCLASSEX wnd_cls = { 0 };

	wnd_cls.cbSize = sizeof(WNDCLASSEX);
	wnd_cls.style = CS_OWNDC;
	wnd_cls.lpfnWndProc = cliprdr_proc;
	wnd_cls.cbClsExtra = 0;
	wnd_cls.cbWndExtra = 0;
	wnd_cls.hIcon = NULL;
	wnd_cls.hCursor = NULL;
	wnd_cls.hbrBackground = NULL;
	wnd_cls.lpszMenuName = NULL;
	wnd_cls.lpszClassName = _T("ClipboardHiddenMessageProcessor");
	wnd_cls.hInstance = GetModuleHandle(NULL);
	wnd_cls.hIconSm = NULL;
	RegisterClassEx(&wnd_cls);
	clipboard->hwnd =
	    CreateWindowEx(WS_EX_LEFT, _T("ClipboardHiddenMessageProcessor"), _T("rdpclip"), 0, 0, 0, 0,
	                   0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), clipboard);

	if (!clipboard->hwnd)
	{
		DEBUG_CLIPRDR("error: CreateWindowEx failed with %x.", GetLastError());
		return -1;
	}

	return 0;
}

static DWORD WINAPI cliprdr_thread_func(LPVOID arg)
{
	int ret;
	MSG msg;
	BOOL mcode;
	wfClipboard* clipboard = (wfClipboard*)arg;
	OleInitialize(0);

	if ((ret = create_cliprdr_window(clipboard)) != 0)
	{
		OleUninitialize();
		DEBUG_CLIPRDR("error: create clipboard window failed.");
		return 0;
	}

	while ((mcode = GetMessage(&msg, 0, 0, 0)) != 0)
	{
		if (mcode == -1)
		{
			DEBUG_CLIPRDR("error: clipboard thread GetMessage failed.");
			break;
		}
		else
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	OleUninitialize();
	return 0;
}

static void clear_file_array(wfClipboard* clipboard)
{
	if (!clipboard)
		return;

	/* clear file_names array */
	if (clipboard->file_names)
	{
		for (size_t i = 0; i < clipboard->nFiles; i++)
		{
			free(clipboard->file_names[i]);
			clipboard->file_names[i] = NULL;
		}

		free(clipboard->file_names);
		clipboard->file_names = NULL;
	}

	/* clear fileDescriptor array */
	if (clipboard->fileDescriptor)
	{
		for (size_t i = 0; i < clipboard->nFiles; i++)
		{
			free(clipboard->fileDescriptor[i]);
			clipboard->fileDescriptor[i] = NULL;
		}

		free(clipboard->fileDescriptor);
		clipboard->fileDescriptor = NULL;
	}

	clipboard->file_array_size = 0;
	clipboard->nFiles = 0;
}

static BOOL wf_cliprdr_get_file_contents(WCHAR* file_name, BYTE* buffer, LONG positionLow,
                                         LONG positionHigh, DWORD nRequested, DWORD* puSize)
{
	BOOL res = FALSE;
	HANDLE hFile;
	DWORD nGet, rc;

	if (!file_name || !buffer || !puSize)
	{
		WLog_ERR(TAG, "get file contents Invalid Arguments.");
		return FALSE;
	}

	hFile = CreateFileW(file_name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		return FALSE;

	rc = SetFilePointer(hFile, positionLow, &positionHigh, FILE_BEGIN);

	if (rc == INVALID_SET_FILE_POINTER)
		goto error;

	if (!ReadFile(hFile, buffer, nRequested, &nGet, NULL))
	{
		DEBUG_CLIPRDR("ReadFile failed with 0x%08lX.", GetLastError());
		goto error;
	}

	res = TRUE;
error:

	if (!CloseHandle(hFile))
		res = FALSE;

	if (res)
		*puSize = nGet;

	return res;
}

/* path_name has a '\' at the end. e.g. c:\newfolder\, file_name is c:\newfolder\new.txt */
static FILEDESCRIPTORW* wf_cliprdr_get_file_descriptor(WCHAR* file_name, size_t pathLen)
{
	HANDLE hFile;
	FILEDESCRIPTORW* fd;
	fd = (FILEDESCRIPTORW*)calloc(1, sizeof(FILEDESCRIPTORW));

	if (!fd)
		return NULL;

	hFile = CreateFileW(file_name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		free(fd);
		return NULL;
	}

	fd->dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
	fd->dwFileAttributes = GetFileAttributes(file_name);

	if (!GetFileTime(hFile, NULL, NULL, &fd->ftLastWriteTime))
	{
		fd->dwFlags &= ~FD_WRITESTIME;
	}

	fd->nFileSizeLow = GetFileSize(hFile, &fd->nFileSizeHigh);
	wcscpy_s(fd->cFileName, sizeof(fd->cFileName) / 2, file_name + pathLen);
	(void)CloseHandle(hFile);
	return fd;
}

static BOOL wf_cliprdr_array_ensure_capacity(wfClipboard* clipboard)
{
	if (!clipboard)
		return FALSE;

	if (clipboard->nFiles == clipboard->file_array_size)
	{
		size_t new_size;
		FILEDESCRIPTORW** new_fd;
		WCHAR** new_name;
		new_size = (clipboard->file_array_size + 1) * 2;
		new_fd = (FILEDESCRIPTORW**)realloc(clipboard->fileDescriptor,
		                                    new_size * sizeof(FILEDESCRIPTORW*));

		if (new_fd)
			clipboard->fileDescriptor = new_fd;

		new_name = (WCHAR**)realloc(clipboard->file_names, new_size * sizeof(WCHAR*));

		if (new_name)
			clipboard->file_names = new_name;

		if (!new_fd || !new_name)
			return FALSE;

		clipboard->file_array_size = new_size;
	}

	return TRUE;
}

static BOOL wf_cliprdr_add_to_file_arrays(wfClipboard* clipboard, WCHAR* full_file_name,
                                          size_t pathLen)
{
	if (!wf_cliprdr_array_ensure_capacity(clipboard))
		return FALSE;

	/* add to name array */
	clipboard->file_names[clipboard->nFiles] = (LPWSTR)malloc(MAX_PATH * 2);

	if (!clipboard->file_names[clipboard->nFiles])
		return FALSE;

	wcscpy_s(clipboard->file_names[clipboard->nFiles], MAX_PATH, full_file_name);
	/* add to descriptor array */
	clipboard->fileDescriptor[clipboard->nFiles] =
	    wf_cliprdr_get_file_descriptor(full_file_name, pathLen);

	if (!clipboard->fileDescriptor[clipboard->nFiles])
	{
		free(clipboard->file_names[clipboard->nFiles]);
		return FALSE;
	}

	clipboard->nFiles++;
	return TRUE;
}

static BOOL wf_cliprdr_traverse_directory(wfClipboard* clipboard, WCHAR* Dir, size_t pathLen)
{
	HANDLE hFind;
	WCHAR DirSpec[MAX_PATH];
	WIN32_FIND_DATA FindFileData;

	if (!clipboard || !Dir)
		return FALSE;

	StringCchCopy(DirSpec, MAX_PATH, Dir);
	StringCchCat(DirSpec, MAX_PATH, TEXT("\\*"));
	hFind = FindFirstFile(DirSpec, &FindFileData);

	if (hFind == INVALID_HANDLE_VALUE)
	{
		DEBUG_CLIPRDR("FindFirstFile failed with 0x%x.", GetLastError());
		return FALSE;
	}

	while (FindNextFile(hFind, &FindFileData))
	{
		if ((((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) &&
		     (wcscmp(FindFileData.cFileName, _T(".")) == 0)) ||
		    (wcscmp(FindFileData.cFileName, _T("..")) == 0))
		{
			continue;
		}

		if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		{
			WCHAR DirAdd[MAX_PATH];
			StringCchCopy(DirAdd, MAX_PATH, Dir);
			StringCchCat(DirAdd, MAX_PATH, _T("\\"));
			StringCchCat(DirAdd, MAX_PATH, FindFileData.cFileName);

			if (!wf_cliprdr_add_to_file_arrays(clipboard, DirAdd, pathLen))
				return FALSE;

			if (!wf_cliprdr_traverse_directory(clipboard, DirAdd, pathLen))
				return FALSE;
		}
		else
		{
			WCHAR fileName[MAX_PATH];
			StringCchCopy(fileName, MAX_PATH, Dir);
			StringCchCat(fileName, MAX_PATH, _T("\\"));
			StringCchCat(fileName, MAX_PATH, FindFileData.cFileName);

			if (!wf_cliprdr_add_to_file_arrays(clipboard, fileName, pathLen))
				return FALSE;
		}
	}

	FindClose(hFind);
	return TRUE;
}

static UINT wf_cliprdr_send_client_capabilities(wfClipboard* clipboard)
{
	CLIPRDR_CAPABILITIES capabilities;
	CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet;

	if (!clipboard || !clipboard->context || !clipboard->context->ClientCapabilities)
		return ERROR_INTERNAL_ERROR;

	capabilities.cCapabilitiesSets = 1;
	capabilities.capabilitySets = (CLIPRDR_CAPABILITY_SET*)&(generalCapabilitySet);
	generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
	generalCapabilitySet.capabilitySetLength = 12;
	generalCapabilitySet.version = CB_CAPS_VERSION_2;
	generalCapabilitySet.generalFlags =
	    CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED | CB_FILECLIP_NO_FILE_PATHS;
	return clipboard->context->ClientCapabilities(clipboard->context, &capabilities);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT wf_cliprdr_monitor_ready(CliprdrClientContext* context,
                                     const CLIPRDR_MONITOR_READY* monitorReady)
{
	UINT rc;
	wfClipboard* clipboard = (wfClipboard*)context->custom;

	if (!context || !monitorReady)
		return ERROR_INTERNAL_ERROR;

	clipboard->sync = TRUE;
	rc = wf_cliprdr_send_client_capabilities(clipboard);

	if (rc != CHANNEL_RC_OK)
		return rc;

	return cliprdr_send_format_list(clipboard);
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT wf_cliprdr_server_capabilities(CliprdrClientContext* context,
                                           const CLIPRDR_CAPABILITIES* capabilities)
{
	CLIPRDR_CAPABILITY_SET* capabilitySet;
	wfClipboard* clipboard = (wfClipboard*)context->custom;

	if (!context || !capabilities)
		return ERROR_INTERNAL_ERROR;

	for (UINT32 index = 0; index < capabilities->cCapabilitiesSets; index++)
	{
		capabilitySet = &(capabilities->capabilitySets[index]);

		if ((capabilitySet->capabilitySetType == CB_CAPSTYPE_GENERAL) &&
		    (capabilitySet->capabilitySetLength >= CB_CAPSTYPE_GENERAL_LEN))
		{
			CLIPRDR_GENERAL_CAPABILITY_SET* generalCapabilitySet =
			    (CLIPRDR_GENERAL_CAPABILITY_SET*)capabilitySet;
			clipboard->capabilities = generalCapabilitySet->generalFlags;
			break;
		}
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT wf_cliprdr_server_format_list(CliprdrClientContext* context,
                                          const CLIPRDR_FORMAT_LIST* formatList)
{
	UINT rc = ERROR_INTERNAL_ERROR;
	formatMapping* mapping;
	CLIPRDR_FORMAT* format;
	wfClipboard* clipboard = (wfClipboard*)context->custom;

	if (!clear_format_map(clipboard))
		return ERROR_INTERNAL_ERROR;

	for (UINT32 i = 0; i < formatList->numFormats; i++)
	{
		format = &(formatList->formats[i]);
		mapping = &(clipboard->format_mappings[i]);
		mapping->remote_format_id = format->formatId;

		if (format->formatName)
		{
			mapping->name = ConvertUtf8ToWCharAlloc(format->formatName, NULL);

			if (mapping->name)
				mapping->local_format_id = RegisterClipboardFormatW((LPWSTR)mapping->name);
		}
		else
		{
			mapping->name = NULL;
			mapping->local_format_id = mapping->remote_format_id;
		}

		clipboard->map_size++;
		map_ensure_capacity(clipboard);
	}

	if (file_transferring(clipboard))
	{
		if (PostMessage(clipboard->hwnd, WM_CLIPRDR_MESSAGE, OLE_SETCLIPBOARD, 0))
			rc = CHANNEL_RC_OK;
	}
	else
	{
		if (!try_open_clipboard(clipboard->hwnd))
			return CHANNEL_RC_OK; /* Ignore, other app holding clipboard */

		if (EmptyClipboard())
		{
			for (UINT32 i = 0; i < (UINT32)clipboard->map_size; i++)
				SetClipboardData(clipboard->format_mappings[i].local_format_id, NULL);

			rc = CHANNEL_RC_OK;
		}

		if (!CloseClipboard() && GetLastError())
			return ERROR_INTERNAL_ERROR;
	}

	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_format_list_response(CliprdrClientContext* context,
                                       const CLIPRDR_FORMAT_LIST_RESPONSE* formatListResponse)
{
	(void)context;
	(void)formatListResponse;

	if (formatListResponse->common.msgFlags != CB_RESPONSE_OK)
		WLog_WARN(TAG, "format list update failed");

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_lock_clipboard_data(CliprdrClientContext* context,
                                      const CLIPRDR_LOCK_CLIPBOARD_DATA* lockClipboardData)
{
	(void)context;
	(void)lockClipboardData;
	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_unlock_clipboard_data(CliprdrClientContext* context,
                                        const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlockClipboardData)
{
	(void)context;
	(void)unlockClipboardData;
	return CHANNEL_RC_OK;
}

static BOOL wf_cliprdr_process_filename(wfClipboard* clipboard, WCHAR* wFileName, size_t str_len)
{
	size_t pathLen;
	size_t offset = str_len;

	if (!clipboard || !wFileName)
		return FALSE;

	/* find the last '\' in full file name */
	while (offset > 0)
	{
		if (wFileName[offset] == L'\\')
			break;
		else
			offset--;
	}

	pathLen = offset + 1;

	if (!wf_cliprdr_add_to_file_arrays(clipboard, wFileName, pathLen))
		return FALSE;

	if ((clipboard->fileDescriptor[clipboard->nFiles - 1]->dwFileAttributes &
	     FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		/* this is a directory */
		if (!wf_cliprdr_traverse_directory(clipboard, wFileName, pathLen))
			return FALSE;
	}

	return TRUE;
}

static SSIZE_T wf_cliprdr_tryopen(wfClipboard* clipboard, UINT32 requestedFormatId, BYTE** pData)
{
	SSIZE_T rc = -1;
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(pData);

	*pData = NULL;

	/* Ignore if other app is holding the clipboard */
	if (!try_open_clipboard(clipboard->hwnd))
		return 0;

	HANDLE hClipdata = GetClipboardData(requestedFormatId);

	if (!hClipdata)
		goto fail;

	char* globlemem = (char*)GlobalLock(hClipdata);
	const SSIZE_T size = GlobalSize(hClipdata);
	if (size <= 0)
		goto unlock;

	BYTE* buff = malloc(size);
	if (buff == NULL)
		goto fail;
	CopyMemory(buff, globlemem, size);
	*pData = buff;
	rc = size;

unlock:
	GlobalUnlock(hClipdata);

fail:
	CloseClipboard();

	return rc;
}

static SSIZE_T wf_cliprdr_get_filedescriptor(wfClipboard* clipboard, BYTE** pData)
{
	WINPR_ASSERT(clipboard);
	WINPR_ASSERT(pData);

	SSIZE_T rc = -1;
	LPDATAOBJECT dataObj = NULL;
	FORMATETC format_etc = { 0 };
	STGMEDIUM stg_medium = { 0 };

	*pData = NULL;

	HRESULT result = OleGetClipboard(&dataObj);
	if (FAILED(result))
		return -1;

	/* get DROPFILES struct from OLE */
	format_etc.cfFormat = CF_HDROP;
	format_etc.tymed = TYMED_HGLOBAL;
	format_etc.dwAspect = 1;
	format_etc.lindex = -1;
	result = IDataObject_GetData(dataObj, &format_etc, &stg_medium);

	if (FAILED(result))
	{
		DEBUG_CLIPRDR("dataObj->GetData failed.");
		goto exit;
	}

	HGLOBAL hdl = stg_medium.u.hGlobal;
	DROPFILES* dropFiles = (DROPFILES*)GlobalLock(hdl);

	if (!dropFiles)
	{
		ReleaseStgMedium(&stg_medium);
		clipboard->nFiles = 0;
		goto exit;
	}

	clear_file_array(clipboard);

	if (dropFiles->fWide)
	{
		/* dropFiles contains file names */
		size_t len = 0;
		for (WCHAR* wFileName = (WCHAR*)((char*)dropFiles + dropFiles->pFiles);
		     (len = wcslen(wFileName)) > 0; wFileName += len + 1)
		{
			wf_cliprdr_process_filename(clipboard, wFileName, wcslen(wFileName));
		}
	}
	else
	{
		size_t len = 0;
		for (char* p = (char*)((char*)dropFiles + dropFiles->pFiles); (len = strlen(p)) > 0;
		     p += len + 1, clipboard->nFiles++)
		{
			const int ilen = WINPR_ASSERTING_INT_CAST(int, len);
			const int cchWideChar = MultiByteToWideChar(CP_ACP, MB_COMPOSITE, p, ilen, NULL, 0);
			WCHAR* wFileName = (LPWSTR)calloc(cchWideChar, sizeof(WCHAR));
			MultiByteToWideChar(CP_ACP, MB_COMPOSITE, p, ilen, wFileName, cchWideChar);
			wf_cliprdr_process_filename(clipboard, wFileName, cchWideChar);
			free(wFileName);
		}
	}

	GlobalUnlock(hdl);
	ReleaseStgMedium(&stg_medium);
exit:
{
	const size_t size = 4ull + clipboard->nFiles * sizeof(FILEDESCRIPTORW);
	FILEGROUPDESCRIPTORW* groupDsc = (FILEGROUPDESCRIPTORW*)calloc(size, 1);

	if (groupDsc)
	{
		groupDsc->cItems = WINPR_ASSERTING_INT_CAST(UINT, clipboard->nFiles);

		for (size_t i = 0; i < clipboard->nFiles; i++)
		{
			if (clipboard->fileDescriptor[i])
				groupDsc->fgd[i] = *clipboard->fileDescriptor[i];
		}

		*pData = (BYTE*)groupDsc;
		rc = size;
	}
}

	IDataObject_Release(dataObj);
	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_format_data_request(CliprdrClientContext* context,
                                      const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	if (!context || !formatDataRequest)
		return ERROR_INTERNAL_ERROR;

	wfClipboard* clipboard = (wfClipboard*)context->custom;

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	const UINT32 requestedFormatId = formatDataRequest->requestedFormatId;
	BYTE* requestedFormatData = NULL;
	SSIZE_T res = 0;
	if (requestedFormatId == RegisterClipboardFormat(CFSTR_FILEDESCRIPTORW))
	{
		res = wf_cliprdr_get_filedescriptor(clipboard, &requestedFormatData);
	}
	else
	{
		res = wf_cliprdr_tryopen(clipboard, requestedFormatId, &requestedFormatData);
	}

	UINT rc = ERROR_INTERNAL_ERROR;
	if (res >= 0)
	{
		const CLIPRDR_FORMAT_DATA_RESPONSE response = {
			.common = { .msgType = CB_FORMAT_DATA_RESPONSE,
			            .msgFlags = CB_RESPONSE_OK,
			            .dataLen = (uint32_t)res },
			.requestedFormatData = requestedFormatData
		};

		rc = clipboard->context->ClientFormatDataResponse(clipboard->context, &response);
	}
	else
	{
		const CLIPRDR_FORMAT_DATA_RESPONSE response = { .common = { .msgType =
			                                                            CB_FORMAT_DATA_RESPONSE,
			                                                        .msgFlags = CB_RESPONSE_FAIL,
			                                                        .dataLen = 0 },
			                                            .requestedFormatData = NULL };

		rc = clipboard->context->ClientFormatDataResponse(clipboard->context, &response);
	}

	free(requestedFormatData);
	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_format_data_response(CliprdrClientContext* context,
                                       const CLIPRDR_FORMAT_DATA_RESPONSE* formatDataResponse)
{
	BYTE* data;
	HANDLE hMem;
	wfClipboard* clipboard;

	if (!context || !formatDataResponse)
		return ERROR_INTERNAL_ERROR;

	clipboard = (wfClipboard*)context->custom;

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	if (formatDataResponse->common.msgFlags != CB_RESPONSE_OK)
	{
		clipboard->hmem = NULL;

		if (!SetEvent(clipboard->response_data_event))
			return ERROR_INTERNAL_ERROR;

		return CHANNEL_RC_OK;
	}

	hMem = GlobalAlloc(GMEM_MOVEABLE, formatDataResponse->common.dataLen);

	if (!hMem)
		return ERROR_INTERNAL_ERROR;

	data = (BYTE*)GlobalLock(hMem);

	if (!data)
	{
		GlobalFree(hMem);
		return ERROR_INTERNAL_ERROR;
	}

	CopyMemory(data, formatDataResponse->requestedFormatData, formatDataResponse->common.dataLen);

	if (!GlobalUnlock(hMem) && GetLastError())
	{
		GlobalFree(hMem);
		return ERROR_INTERNAL_ERROR;
	}

	clipboard->hmem = hMem;

	if (!SetEvent(clipboard->response_data_event))
		return ERROR_INTERNAL_ERROR;

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_file_contents_request(CliprdrClientContext* context,
                                        const CLIPRDR_FILE_CONTENTS_REQUEST* fileContentsRequest)
{
	DWORD uSize = 0;
	BYTE* pData = NULL;
	HRESULT hRet = S_OK;
	FORMATETC vFormatEtc = { 0 };
	LPDATAOBJECT pDataObj = NULL;
	STGMEDIUM vStgMedium = { 0 };
	BOOL bIsStreamFile = TRUE;
	static LPSTREAM pStreamStc = NULL;
	static UINT32 uStreamIdStc = 0;
	wfClipboard* clipboard;
	UINT rc = ERROR_INTERNAL_ERROR;
	UINT sRc;
	UINT32 cbRequested;

	if (!context || !fileContentsRequest)
		return ERROR_INTERNAL_ERROR;

	clipboard = (wfClipboard*)context->custom;

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	cbRequested = fileContentsRequest->cbRequested;
	if (fileContentsRequest->dwFlags == FILECONTENTS_SIZE)
		cbRequested = sizeof(UINT64);

	pData = (BYTE*)calloc(1, cbRequested);

	if (!pData)
		goto error;

	hRet = OleGetClipboard(&pDataObj);

	if (FAILED(hRet))
	{
		WLog_ERR(TAG, "filecontents: get ole clipboard failed.");
		goto error;
	}

	vFormatEtc.cfFormat = RegisterClipboardFormat(CFSTR_FILECONTENTS);
	vFormatEtc.tymed = TYMED_ISTREAM;
	vFormatEtc.dwAspect = 1;
	vFormatEtc.lindex = fileContentsRequest->listIndex;
	vFormatEtc.ptd = NULL;

	if ((uStreamIdStc != fileContentsRequest->streamId) || !pStreamStc)
	{
		LPENUMFORMATETC pEnumFormatEtc;
		ULONG CeltFetched;
		FORMATETC vFormatEtc2;

		if (pStreamStc)
		{
			IStream_Release(pStreamStc);
			pStreamStc = NULL;
		}

		bIsStreamFile = FALSE;
		hRet = IDataObject_EnumFormatEtc(pDataObj, DATADIR_GET, &pEnumFormatEtc);

		if (hRet == S_OK)
		{
			do
			{
				hRet = IEnumFORMATETC_Next(pEnumFormatEtc, 1, &vFormatEtc2, &CeltFetched);

				if (hRet == S_OK)
				{
					if (vFormatEtc2.cfFormat == RegisterClipboardFormat(CFSTR_FILECONTENTS))
					{
						hRet = IDataObject_GetData(pDataObj, &vFormatEtc, &vStgMedium);

						if (hRet == S_OK)
						{
							pStreamStc = vStgMedium.u.pstm;
							uStreamIdStc = fileContentsRequest->streamId;
							bIsStreamFile = TRUE;
						}

						break;
					}
				}
			} while (hRet == S_OK);
		}
	}

	if (bIsStreamFile == TRUE)
	{
		if (fileContentsRequest->dwFlags == FILECONTENTS_SIZE)
		{
			STATSTG vStatStg = { 0 };
			hRet = IStream_Stat(pStreamStc, &vStatStg, STATFLAG_NONAME);

			if (hRet == S_OK)
			{
				*((UINT32*)&pData[0]) = vStatStg.cbSize.QuadPart & 0xFFFFFFFF;
				*((UINT32*)&pData[4]) = (vStatStg.cbSize.QuadPart >> 32) & 0xFFFFFFFF;
				uSize = cbRequested;
			}
		}
		else if (fileContentsRequest->dwFlags == FILECONTENTS_RANGE)
		{
			LARGE_INTEGER dlibMove;
			ULARGE_INTEGER dlibNewPosition;
			dlibMove.QuadPart = (INT64)(((UINT64)fileContentsRequest->nPositionHigh << 32) |
			                            fileContentsRequest->nPositionLow);
			hRet = IStream_Seek(pStreamStc, dlibMove, STREAM_SEEK_SET, &dlibNewPosition);

			if (SUCCEEDED(hRet))
				hRet = IStream_Read(pStreamStc, pData, cbRequested, (PULONG)&uSize);
		}
	}
	else
	{
		if (fileContentsRequest->dwFlags == FILECONTENTS_SIZE)
		{
			if (clipboard->nFiles <= fileContentsRequest->listIndex)
				goto error;
			*((UINT32*)&pData[0]) =
			    clipboard->fileDescriptor[fileContentsRequest->listIndex]->nFileSizeLow;
			*((UINT32*)&pData[4]) =
			    clipboard->fileDescriptor[fileContentsRequest->listIndex]->nFileSizeHigh;
			uSize = cbRequested;
		}
		else if (fileContentsRequest->dwFlags == FILECONTENTS_RANGE)
		{
			BOOL bRet;
			if (clipboard->nFiles <= fileContentsRequest->listIndex)
				goto error;
			bRet = wf_cliprdr_get_file_contents(
			    clipboard->file_names[fileContentsRequest->listIndex], pData,
			    fileContentsRequest->nPositionLow, fileContentsRequest->nPositionHigh, cbRequested,
			    &uSize);

			if (bRet == FALSE)
			{
				WLog_ERR(TAG, "get file contents failed.");
				uSize = 0;
				goto error;
			}
		}
	}

	rc = CHANNEL_RC_OK;
error:

	if (pDataObj)
		IDataObject_Release(pDataObj);

	if (uSize == 0)
	{
		free(pData);
		pData = NULL;
	}

	sRc =
	    cliprdr_send_response_filecontents(clipboard, fileContentsRequest->streamId, uSize, pData);
	free(pData);

	if (sRc != CHANNEL_RC_OK)
		return sRc;

	return rc;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT
wf_cliprdr_server_file_contents_response(CliprdrClientContext* context,
                                         const CLIPRDR_FILE_CONTENTS_RESPONSE* fileContentsResponse)
{
	wfClipboard* clipboard;

	if (!context || !fileContentsResponse)
		return ERROR_INTERNAL_ERROR;

	if (fileContentsResponse->common.msgFlags != CB_RESPONSE_OK)
		return E_FAIL;

	clipboard = (wfClipboard*)context->custom;

	if (!clipboard)
		return ERROR_INTERNAL_ERROR;

	clipboard->req_fsize = fileContentsResponse->cbRequested;
	clipboard->req_fdata = (char*)malloc(fileContentsResponse->cbRequested);

	if (!clipboard->req_fdata)
		return ERROR_INTERNAL_ERROR;

	CopyMemory(clipboard->req_fdata, fileContentsResponse->requestedData,
	           fileContentsResponse->cbRequested);

	if (!SetEvent(clipboard->req_fevent))
	{
		free(clipboard->req_fdata);
		return ERROR_INTERNAL_ERROR;
	}

	return CHANNEL_RC_OK;
}

BOOL wf_cliprdr_init(wfContext* wfc, CliprdrClientContext* cliprdr)
{
	wfClipboard* clipboard;
	rdpContext* context = (rdpContext*)wfc;

	if (!context || !cliprdr)
		return FALSE;

	wfc->clipboard = (wfClipboard*)calloc(1, sizeof(wfClipboard));

	if (!wfc->clipboard)
		return FALSE;

	clipboard = wfc->clipboard;
	clipboard->wfc = wfc;
	clipboard->context = cliprdr;
	clipboard->channels = context->channels;
	clipboard->sync = FALSE;
	clipboard->map_capacity = 32;
	clipboard->map_size = 0;
	clipboard->hUser32 = LoadLibraryA("user32.dll");

	if (clipboard->hUser32)
	{
		clipboard->AddClipboardFormatListener = GetProcAddressAs(
		    clipboard->hUser32, "AddClipboardFormatListener", fnAddClipboardFormatListener);
		clipboard->RemoveClipboardFormatListener = GetProcAddressAs(
		    clipboard->hUser32, "RemoveClipboardFormatListener", fnRemoveClipboardFormatListener);
		clipboard->GetUpdatedClipboardFormats = GetProcAddressAs(
		    clipboard->hUser32, "GetUpdatedClipboardFormats", fnGetUpdatedClipboardFormats);
	}

	if (!(clipboard->hUser32 && clipboard->AddClipboardFormatListener &&
	      clipboard->RemoveClipboardFormatListener && clipboard->GetUpdatedClipboardFormats))
		clipboard->legacyApi = TRUE;

	if (!(clipboard->format_mappings =
	          (formatMapping*)calloc(clipboard->map_capacity, sizeof(formatMapping))))
		goto error;

	if (!(clipboard->response_data_event = CreateEvent(NULL, TRUE, FALSE, NULL)))
		goto error;

	if (!(clipboard->req_fevent = CreateEvent(NULL, TRUE, FALSE, NULL)))
		goto error;

	if (!(clipboard->thread = CreateThread(NULL, 0, cliprdr_thread_func, clipboard, 0, NULL)))
		goto error;

	cliprdr->MonitorReady = wf_cliprdr_monitor_ready;
	cliprdr->ServerCapabilities = wf_cliprdr_server_capabilities;
	cliprdr->ServerFormatList = wf_cliprdr_server_format_list;
	cliprdr->ServerFormatListResponse = wf_cliprdr_server_format_list_response;
	cliprdr->ServerLockClipboardData = wf_cliprdr_server_lock_clipboard_data;
	cliprdr->ServerUnlockClipboardData = wf_cliprdr_server_unlock_clipboard_data;
	cliprdr->ServerFormatDataRequest = wf_cliprdr_server_format_data_request;
	cliprdr->ServerFormatDataResponse = wf_cliprdr_server_format_data_response;
	cliprdr->ServerFileContentsRequest = wf_cliprdr_server_file_contents_request;
	cliprdr->ServerFileContentsResponse = wf_cliprdr_server_file_contents_response;
	cliprdr->custom = (void*)wfc->clipboard;
	return TRUE;
error:
	wf_cliprdr_uninit(wfc, cliprdr);
	return FALSE;
}

BOOL wf_cliprdr_uninit(wfContext* wfc, CliprdrClientContext* cliprdr)
{
	wfClipboard* clipboard;

	if (!wfc || !cliprdr)
		return FALSE;

	clipboard = wfc->clipboard;

	if (!clipboard)
		return FALSE;

	cliprdr->custom = NULL;

	if (clipboard->hwnd)
		PostMessage(clipboard->hwnd, WM_QUIT, 0, 0);

	if (clipboard->thread)
	{
		(void)WaitForSingleObject(clipboard->thread, INFINITE);
		(void)CloseHandle(clipboard->thread);
	}

	if (clipboard->response_data_event)
		(void)CloseHandle(clipboard->response_data_event);

	if (clipboard->req_fevent)
		(void)CloseHandle(clipboard->req_fevent);

	clear_file_array(clipboard);
	clear_format_map(clipboard);
	free(clipboard->format_mappings);
	free(clipboard);
	return TRUE;
}
