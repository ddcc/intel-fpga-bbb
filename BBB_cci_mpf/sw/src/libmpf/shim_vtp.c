//
// Copyright (c) 2017, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// Neither the name of the Intel Corporation nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/**
 * \file shim_vtp.c
 * \brief MPF VTP (virtual to physical) translation shim
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <opae/mpf/mpf.h>
#include "mpf_internal.h"

// Try for 1GB pages for requests above 75% of the page
static const size_t CCI_MPF_VTP_1GB_PAGE_THRESHOLD = 805306368;
// The threshold for using 2MB pages is lower since the performance gain
// is more significant.
static const size_t CCI_MPF_VTP_2MB_PAGE_THRESHOLD = (128*1024);


// ========================================================================
//
//   Module internal methods.
//
// ========================================================================

//
// Turn the FPGA side on.
//
static fpga_result vtpEnable(
    _mpf_handle_p _mpf_handle
)
{
    fpga_result r;

    // Nothing to do if now HW VTP
    if (! _mpf_handle->vtp.is_hw_vtp_available) return FPGA_OK;

    // Disable VTP
    r = mpfWriteCsr(_mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_MODE, 0);

    r = mpfWriteCsr(_mpf_handle,
                    CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_PAGE_TABLE_PADDR,
                    mpfVtpPtGetPageTableRootPA(_mpf_handle->vtp.pt) / CL(1));

    // Enable VTP
    r = mpfWriteCsr(_mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_MODE, 1);

    return r;
}


//
// Pin a region of memory. Alignment and other details have already been checked
// by one of the buffer allocation functions.
//
static fpga_result vtpPinRegion(
    _mpf_handle_p _mpf_handle,
    uint64_t len,
    void* buf_addr,
    uint32_t pt_flags
)
{
    fpga_result r;
    const size_t page_mask_1gb = mpfPageSizeEnumToBytes(MPF_VTP_PAGE_1GB) - 1;
    const size_t page_mask_2mb = mpfPageSizeEnumToBytes(MPF_VTP_PAGE_2MB) - 1;
    const bool preallocated = (pt_flags & MPF_VTP_PT_FLAG_PREALLOC);

    uint8_t* page = buf_addr;

    while (len)
    {
        size_t this_page_bytes;
        mpf_vtp_page_size this_page_size;

        mpfVtpPtLockMutex(_mpf_handle->vtp.pt);

        // Preallocated pages may already be pinned since two buffer requests
        // may share a page.
        if (preallocated)
        {
            mpf_vtp_pt_paddr existing_pa;
            uint32_t existing_flags;
            r = mpfVtpPtTranslateVAtoPA(_mpf_handle->vtp.pt, page, false,
                                        &existing_pa, &this_page_size, &existing_flags);
            if (FPGA_OK == r)
            {
                this_page_bytes = mpfPageSizeEnumToBytes(this_page_size);
                goto already_pinned;
            }
        }

        // Detect huge pages
        if ((0 == ((size_t)page & page_mask_2mb)) &&
            (_mpf_handle->vtp.max_physical_page_size >= MPF_VTP_PAGE_2MB))
        {
            r = mpfOsGetPageSize(page, &this_page_size);
            if (FPGA_OK != r) goto fail;

            // If the page is 1GB then the request has to be aligned
            // to the start of the page and VTP has to be willing to
            // manage 1GB pages. If not, treat it as a 2MB page.
            if ((this_page_size >= MPF_VTP_PAGE_1GB) &&
                ((0 != ((size_t)page & page_mask_1gb)) ||
                 (_mpf_handle->vtp.max_physical_page_size < MPF_VTP_PAGE_1GB)))
            {
                this_page_size = MPF_VTP_PAGE_2MB;
            }
        }
        else
        {
            this_page_size = MPF_VTP_PAGE_4KB;
        }

        this_page_bytes = mpfPageSizeEnumToBytes(this_page_size);

        r = mpfVtpPinAndInsertPage(_mpf_handle, true, page, this_page_size, pt_flags,
                                   NULL, NULL);
        if (FPGA_OK != r) goto fail;

        if (pt_flags)
        {
            mpfVtpPtSetAllocBufSize(_mpf_handle->vtp.pt, page, len);
        }

      already_pinned:
        // The alloc flags are set only on the first page
        pt_flags &= ~(MPF_VTP_PT_FLAG_ALLOC | MPF_VTP_PT_FLAG_PREALLOC);
        page += this_page_bytes;
        len -= this_page_bytes;

        mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    }

    return FPGA_OK;

  fail:
    mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    return r;
}


//
// Pin an existing pre-allocated buffer.
//
static fpga_result vtpPreallocBuffer(
    _mpf_handle_p _mpf_handle,
    uint64_t len,
    void** buf_addr,
    int fpga_flags      // enum fpga_buffer_flags
)
{
    fpga_result r;

    //
    // Buffer is allocated already before calling VTP. Map the existing
    // buffer for use on the FPGA.
    //
    const size_t p_mask = mpfPageSizeEnumToBytes(MPF_VTP_PAGE_4KB) - 1;

    if (! _mpf_handle->vtp.use_fpga_buf_preallocated)
    {
        MPF_FPGA_MSG("This version of the OPAE SDK does not support FPGA_BUF_PREALLOCATED.\n"
                     "It is very old. Please install a more recent version of the OPAE SDK.");
        return FPGA_INVALID_PARAM;
    }

    // Buffer is already allocated, check addresses.
    if ((NULL == *buf_addr) || ((size_t)*buf_addr & p_mask))
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("Preallocated buffer address is NULL or not page aligned");
        }
        return FPGA_INVALID_PARAM;
    }
    // Check length
    if (! len || (len & p_mask))
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("Preallocated buffer size is not a non-zero multiple of page size");
        }
        return FPGA_INVALID_PARAM;
    }

    uint8_t* page = *buf_addr;

    // Align buffer start to the page size
    mpf_vtp_page_size this_page_size;
    r = mpfOsGetPageSize(*buf_addr, &this_page_size);
    size_t this_page_bytes = mpfPageSizeEnumToBytes(this_page_size);
    if (FPGA_OK != r) return FPGA_NO_MEMORY;

    // Mask out page offset bits
    page = (uint8_t*)((uint64_t)page & ~(this_page_bytes - 1));
    // Adjust the buffer length so it ends where it used to
    len += (uint64_t)*buf_addr - (uint64_t)page;

    // Share each page with the FPGA and insert them into the VTP page table.
    uint32_t pt_flags = MPF_VTP_PT_FLAG_PREALLOC;
#ifdef MFP_OPAE_HAS_BUF_READ_ONLY
    if (FPGA_BUF_READ_ONLY & fpga_flags)
    {
        pt_flags |= MPF_VTP_PT_FLAG_READ_ONLY;
    }
#endif
    r = vtpPinRegion(_mpf_handle, len, (void*)page, pt_flags);
    return r;
}


//
// Allocate and pin a new buffer.
//
static fpga_result vtpAllocBuffer(
    _mpf_handle_p _mpf_handle,
    uint64_t len,
    void** buf_addr,
    int fpga_flags      // enum fpga_buffer_flags
)
{
    fpga_result r;
    mpf_vtp_page_size page_size = MPF_VTP_PAGE_4KB;

    // Is the allocation request large enough for a huge page?
    if ((len > CCI_MPF_VTP_1GB_PAGE_THRESHOLD) &&
        (MPF_VTP_PAGE_1GB <= _mpf_handle->vtp.max_physical_page_size))
    {
        page_size = MPF_VTP_PAGE_1GB;
    }
    else if ((len > CCI_MPF_VTP_2MB_PAGE_THRESHOLD) &&
        (MPF_VTP_PAGE_2MB <= _mpf_handle->vtp.max_physical_page_size))
    {
        page_size = MPF_VTP_PAGE_2MB;
    }

    size_t page_bytes = mpfPageSizeEnumToBytes(page_size);

    // Round len up to the page size
    len = (len + page_bytes - 1) & ~(page_bytes - 1);

    if (_mpf_handle->dbg_mode) MPF_FPGA_MSG("requested 0x%" PRIx64 " byte buffer", len);

    r = mpfOsMapMemory(len, &page_size, buf_addr);
    if (FPGA_OK != r) return FPGA_NO_MEMORY;

    // Share each page with the FPGA and insert them into the VTP page table.
    uint32_t pt_flags = MPF_VTP_PT_FLAG_ALLOC;
#ifdef MFP_OPAE_HAS_BUF_READ_ONLY
    if (FPGA_BUF_READ_ONLY & fpga_flags)
    {
        pt_flags |= MPF_VTP_PT_FLAG_READ_ONLY;
    }
#endif
    r = vtpPinRegion(_mpf_handle, len, *buf_addr, pt_flags);
    return r;
}


// Convert PT flags read-only to OPAE standard flag.
static inline int vtpReportReadOnly(
    uint32_t pt_flags
)
{
    int f = 0;

    if (MPF_VTP_PT_FLAG_READ_ONLY & pt_flags)
    {
#ifdef MFP_OPAE_HAS_BUF_READ_ONLY
        f = FPGA_BUF_READ_ONLY;
#endif
    }

    return f;
}


// ========================================================================
//
//   MPF internal methods.
//
// ========================================================================


fpga_result mpfVtpPinAndInsertPage(
    _mpf_handle_p _mpf_handle,
    bool pt_locked,
    mpf_vtp_pt_vaddr va,
    mpf_vtp_page_size page_size,
    uint32_t pt_flags,
    mpf_vtp_pt_paddr* pinned_pa,
    fpga_result* pin_result
)
{
    fpga_result r;

    // mpf_vtp_page_size values are the log2 of the size.  Convert to bytes.
    size_t page_bytes = mpfPageSizeEnumToBytes(page_size);

    // Allocate buffer at va
    mpf_vtp_pt_vaddr alloc_va = va;
    uint64_t wsid;

    int fpga_flags = FPGA_BUF_PREALLOCATED;
    if (MPF_VTP_PT_FLAG_READ_ONLY & pt_flags)
    {
#ifdef MFP_OPAE_HAS_BUF_READ_ONLY
        fpga_flags |= FPGA_BUF_READ_ONLY;
#endif
    }

    if (! _mpf_handle->vtp.use_fpga_buf_preallocated)
    {
        MPF_FPGA_MSG("This version of the OPAE SDK does not support FPGA_BUF_PREALLOCATED.\n"
                     "It is very old. Please install a more recent version of the OPAE SDK.");
        return FPGA_INVALID_PARAM;
    }

    r = fpgaPrepareBuffer(_mpf_handle->handle, page_bytes, &va, &wsid, fpga_flags);
    if (NULL != pin_result)
    {
        *pin_result = r;
    }
    if (FPGA_OK != r)
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("FAILED allocating %s page VA %p, status %d",
                         mpfVtpPageSizeToString(page_size),
                         alloc_va, r);
        }

        return r;
    }

    // Confirm that the allocated VA is in the expected location
    if (alloc_va != va)
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("FAILED allocating %s page VA %p -- at %p instead of requested address",
                         mpfVtpPageSizeToString(page_size),
                         alloc_va, va);
        }

        return FPGA_NO_MEMORY;
    }

    // Invalidate any old address translation in the FPGA. There may be failed
    // speculation entries in the FPGA cache that have to be removed now that
    // the page is valid.
    r = vtpInvalHWVAMapping(_mpf_handle, va, pt_locked);
    if (FPGA_OK != r) return r;

    // Get the physical address of the buffer
    mpf_vtp_pt_paddr alloc_pa;
    r = fpgaGetIOAddress(_mpf_handle->handle, wsid, &alloc_pa);
    if (FPGA_OK != r) return r;

    if (pinned_pa)
    {
        *pinned_pa = alloc_pa;
    }

    if (_mpf_handle->dbg_mode)
    {
        MPF_FPGA_MSG("allocate %s page VA %p, PA 0x%" PRIx64 ", wsid 0x%" PRIx64,
                     mpfVtpPageSizeToString(page_size),
                     alloc_va, alloc_pa, wsid);
    }

    // Insert VTP page table entry
    r = mpfVtpPtInsertPageMapping(_mpf_handle->vtp.pt,
                                  alloc_va, alloc_pa, wsid, page_size, pt_flags);
    if (FPGA_OK != r)
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("FAILED inserting page mapping, status %d", r);
        }

        fpgaReleaseBuffer(_mpf_handle->handle, wsid);

        return r;
    }

    return FPGA_OK;
}


inline
bool mpfVtpPinOnDemandMode(
    _mpf_handle_p _mpf_handle
)
{
    return (_mpf_handle && _mpf_handle->vtp.srv);
}


fpga_result mpfVtpInit(
    _mpf_handle_p _mpf_handle
)
{
    fpga_result r;

    // Is the VTP feature present?
    _mpf_handle->vtp.is_hw_vtp_available = false;

    // Already initialized?
    if (NULL != _mpf_handle->vtp.pt) return FPGA_EXCEPTION;
    if (NULL != _mpf_handle->vtp.srv) return FPGA_EXCEPTION;

    // Test whether FPGA_BUF_PREALLOCATED is supported.  libfpga on old systems
    // might not.  fpgaPrepareBuffer() has a special mode for probing by
    // setting the buffer size to 0.
    uint64_t dummy_wsid;
    _mpf_handle->vtp.use_fpga_buf_preallocated =
        (FPGA_OK == fpgaPrepareBuffer(_mpf_handle->handle, 0, NULL, &dummy_wsid,
                                      FPGA_BUF_PREALLOCATED));
    if (_mpf_handle->dbg_mode)
    {
        MPF_FPGA_MSG("FPGA_BUF_PREALLOCATED %s",
                     (_mpf_handle->vtp.use_fpga_buf_preallocated ?
                          "supported." :
                          "not supported.  Using compatibility mode."));
    }

    _mpf_handle->vtp.max_physical_page_size = MPF_VTP_PAGE_1GB;

    // Initialize the page table
    r = mpfVtpPtInit(_mpf_handle, &(_mpf_handle->vtp.pt));
    if (FPGA_OK != r) return r;

    if (mpfShimPresent(_mpf_handle, CCI_MPF_SHIM_VTP))
    {
        _mpf_handle->vtp.is_hw_vtp_available = true;

        // VTP hardware can't deal with 1GB pages yet
        _mpf_handle->vtp.max_physical_page_size = MPF_VTP_PAGE_2MB;

        // Reset the HW TLB
        r = mpfVtpInvalHWTLB(_mpf_handle);

        // Initialize the software translation service
        r = mpfVtpSrvInit(_mpf_handle, &(_mpf_handle->vtp.srv));
        if (FPGA_OK != r) goto fail;
    }

    return FPGA_OK;

  fail:
    mpfVtpPtTerm(_mpf_handle->vtp.pt);
    return r;
}


fpga_result mpfVtpTerm(
    _mpf_handle_p _mpf_handle
)
{
    fpga_result r, r_ret;

    r_ret = FPGA_OK;
    if (_mpf_handle->dbg_mode) MPF_FPGA_MSG("VTP terminating...");

    if (_mpf_handle->vtp.is_hw_vtp_available)
    {
        // Turn off VTP in the FPGA, blocking all traffic.
        mpfWriteCsr(_mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_MODE, 0);

        r_ret = mpfVtpSrvTerm(_mpf_handle->vtp.srv);
        _mpf_handle->vtp.srv = NULL;
    }
    else if (_mpf_handle->vtp.srv)
    {
        // VTP pinning service shouldn't be running if there is no HW VTP!
        return FPGA_EXCEPTION;
    }

    r = mpfVtpMonitorTerm(_mpf_handle->vtp.munmap_monitor);
    _mpf_handle->vtp.munmap_monitor = NULL;
    if (FPGA_OK == r_ret) r_ret = r;

    r = mpfVtpPtTerm(_mpf_handle->vtp.pt);
    _mpf_handle->vtp.pt = NULL;
    if (FPGA_OK == r_ret) r_ret = r;

    return r_ret;
}


fpga_result vtpInvalHWVAMapping(
    mpf_handle_t mpf_handle,
    mpf_vtp_pt_vaddr va,
    bool pt_locked
)
{
    fpga_result r;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    // Nothing to do if now HW VTP
    if (! _mpf_handle->vtp.is_hw_vtp_available) return FPGA_OK;

    if (! pt_locked) mpfVtpPtLockMutex(_mpf_handle->vtp.pt);

    // Clear the FPGA-side in-use flag. If the flag gets set again
    // we will know that the translation has been reloaded by the FPGA.
    mpfVtpPtClearInUseFlag(_mpf_handle->vtp.pt, va);

    // Tell the FPGA to shoot down cached translation of VA
    r = mpfWriteCsr(mpf_handle,
                    CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_INVAL_PAGE_VADDR,
                    // Convert VA to a line index
                    (uint64_t)va / CL(1));
    if (FPGA_OK != r) return r;

    if (_mpf_handle->dbg_mode)
    {
        MPF_FPGA_MSG("invalidating page VA %p", va);
    }

    if (! pt_locked) mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    return FPGA_OK;
}


// ========================================================================
//
//   MPF exposed external methods.
//
// ========================================================================


__MPF_API__ const char* mpfVtpPageSizeToString(
    mpf_vtp_page_size page_size
)
{
    const char* s = NULL;

    switch (page_size)
    {
      case MPF_VTP_PAGE_4KB:
        s = "4KB";
        break;
      case MPF_VTP_PAGE_2MB:
        s = "2MB";
        break;
      case MPF_VTP_PAGE_1GB:
        s = "1GB";
        break;
      default:
        s = "INVALID";
    }

    return s;
}

bool __MPF_API__ mpfVtpIsAvailable(
    mpf_handle_t mpf_handle
)
{
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;
    return _mpf_handle->vtp.is_hw_vtp_available;
}


fpga_result __MPF_API__ mpfVtpPrepareBuffer(
    mpf_handle_t mpf_handle,
    uint64_t len,
    void** buf_addr,
    int flags
)
{
    fpga_result r;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;
    bool preallocated = (flags & FPGA_BUF_PREALLOCATED);

    if ((NULL == buf_addr) || (0 == len)) return FPGA_INVALID_PARAM;

    if (preallocated)
    {
        r = vtpPreallocBuffer(_mpf_handle, len, buf_addr, flags);
    }
    else
    {
        r = vtpAllocBuffer(_mpf_handle, len, buf_addr, flags);
    }

    if (_mpf_handle->dbg_mode)
    {
        mpfVtpPtLockMutex(_mpf_handle->vtp.pt);
        mpfVtpPtDumpPageTable(_mpf_handle->vtp.pt);
        mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    }

    return r;
}


fpga_result __MPF_API__ mpfVtpBufferAllocate(
    mpf_handle_t mpf_handle,
    uint64_t len,
    void** buf_addr
)
{
    return mpfVtpPrepareBuffer(mpf_handle, len, buf_addr, 0);
}


fpga_result __MPF_API__ mpfVtpReleaseBuffer(
    mpf_handle_t mpf_handle,
    void* buf_addr
)
{
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    mpf_vtp_pt_vaddr va = buf_addr;
    mpf_vtp_pt_paddr pa;
    mpf_vtp_page_size size;
    uint32_t flags;
    uint64_t wsid;
    fpga_result r;

    if (_mpf_handle->dbg_mode)
    {
        MPF_FPGA_MSG("release buffer at VA %p", va);
    }

    mpfVtpPtLockMutex(_mpf_handle->vtp.pt);

    // Is the address the beginning of an allocation region?
    r = mpfVtpPtTranslateVAtoPA(_mpf_handle->vtp.pt, va, false, &pa, NULL, &flags);
    if ((FPGA_OK != r) ||
        (0 == (flags & (MPF_VTP_PT_FLAG_ALLOC | MPF_VTP_PT_FLAG_PREALLOC))))
    {
        mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);

        // Pin on demand mode doesn't store preallocated regions. Just ignore
        // errors when there is no record of the underlying buffer.
        if (mpfVtpPinOnDemandMode(_mpf_handle)) return FPGA_OK;

        return FPGA_NO_MEMORY;
    }

    // Get buffer range, stored in the page table.
    mpf_vtp_pt_vaddr buf_va_start, buf_va_end;
    size_t buf_size;
    r = mpfVtpPtGetAllocBufSize(_mpf_handle->vtp.pt, va, &buf_va_start, &buf_size);
    mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    buf_va_end = (char *)buf_va_start + buf_size;
    if (FPGA_OK != r) return r;

    // Loop through the mapped virtual pages until the end of the region
    // is reached or there is an error.
    while (true)
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("lookup VA %p", va);
        }

        mpfVtpPtLockMutex(_mpf_handle->vtp.pt);
        r = mpfVtpPtRemovePageMapping(_mpf_handle->vtp.pt, va,
                                      &pa, &wsid, &size, NULL);
        mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
        if (FPGA_OK != r)
        {
            if (_mpf_handle->dbg_mode)
            {
                MPF_FPGA_MSG("error unmapping VA %p", va);
            }

            r = FPGA_NO_MEMORY;
            break;
        }

        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("release %s page VA %p, PA 0x%" PRIx64 ", wsid 0x%" PRIx64,
                         mpfVtpPageSizeToString(size),
                         va, pa, wsid);
        }

        size_t page_bytes = mpfPageSizeEnumToBytes(size);

        r = mpfVtpInvalHWVAMapping(_mpf_handle, va);
        if (FPGA_OK != r) break;

        // If the kernel deallocation fails just give up.  Something bad
        // is bound to happen.
        assert(FPGA_OK == fpgaReleaseBuffer(_mpf_handle->handle, wsid));

        // Next page address
        va = (char *) va + page_bytes;

        // Done?
        if (va >= buf_va_end)
        {
            mpfVtpPtLockMutex(_mpf_handle->vtp.pt);
            if (_mpf_handle->dbg_mode) mpfVtpPtDumpPageTable(_mpf_handle->vtp.pt);
            mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);

            r = FPGA_OK;
            break;
        }
    }

    // Was the buffer allocated by MPF? If so, deallocate it.
    if (flags & MPF_VTP_PT_FLAG_ALLOC)
    {
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("unmapping buffer VA %p, size 0x%" PRIx64, buf_va_start, buf_size);
        }
        mpfOsUnmapMemory(buf_va_start, buf_size);
    }

    // Pin on demand mode doesn't store preallocated regions. Just ignore
    // errors when there is no record of the underlying buffer.
    if ((r == FPGA_NO_MEMORY) && mpfVtpPinOnDemandMode(_mpf_handle))
        return FPGA_OK;

    return r;
}


fpga_result __MPF_API__ mpfVtpBufferFree(
    mpf_handle_t mpf_handle,
    void* buf_addr
)
{
    return mpfVtpReleaseBuffer(mpf_handle, buf_addr);
}


uint64_t __MPF_API__ mpfVtpGetIOAddress(
    mpf_handle_t mpf_handle,
    void* buf_addr
)
{
    fpga_result r;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    uint64_t pa;
    mpfVtpPtLockMutex(_mpf_handle->vtp.pt);
    r = mpfVtpPtTranslateVAtoPA(_mpf_handle->vtp.pt, buf_addr, false, &pa, NULL, NULL);
    mpfVtpPtUnlockMutex(_mpf_handle->vtp.pt);
    if (FPGA_OK != r) return 0;

    return pa;
}


fpga_result __MPF_API__ mpfVtpPinAndGetIOAddressVec(
    mpf_handle_t mpf_handle,
    mpf_vtp_pin_mode mode,
    void* buf_addr,
    int* num_pages,
    uint64_t* ioaddr,
    mpf_vtp_page_size* page_size,
    int* flags
)
{
    fpga_result r;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;
    mpf_vtp_pt* pt = _mpf_handle->vtp.pt;
    uint32_t pt_flags;

    if (!ioaddr || !page_size) return FPGA_INVALID_PARAM;

    if ((mode != MPF_VTP_PIN_MODE_LOOKUP_ONLY) || _mpf_handle->vtp.munmap_monitor)
    {
        // Auto pinning on reference is enabled. Track the MMU monitor service
        // so that cached translations are invalidated when pages are unmapped.
        mpfVtpMonitorWaitWhenBusy(_mpf_handle, true);
    }
    mpfVtpPtLockMutex(pt);

    r = mpfVtpPtTranslateVAtoPA(pt, buf_addr, true, ioaddr, page_size, &pt_flags);
    if (FPGA_OK == r)
    {
        // Already mapped. Should more virtually contiguous translations
        // be returned at the same time?
        if (num_pages && (*num_pages > 1))
        {
            assert(*num_pages <= 512);

            uint32_t pt_flags_vec[512];
            pt_flags_vec[0] = pt_flags;

            // Get some more pages, storing the total number returned in
            // *num_pages. Entry 0 of ioaddr has already been set.
            *num_pages = 1 + mpfVtpPtExtendVecVAtoPA(pt, *num_pages - 1, true,
                                                     &ioaddr[1],
                                                     &pt_flags_vec[1]);

            if (flags)
            {
                for (int i = 0; i < *num_pages; i += 1)
                {
                    flags[i] = vtpReportReadOnly(pt_flags_vec[i]);
                }
            }
        }
        else if (flags)
        {
            flags[0] = vtpReportReadOnly(pt_flags);
        }

        mpfVtpPtUnlockMutex(pt);

        return FPGA_OK;
    }

    // Need to pin the memory. At most one translation will be returned.
    if (num_pages) *num_pages = 1;

    if (MPF_VTP_PIN_MODE_LOOKUP_ONLY == mode) return r;

    // What size is the underlying physical page? As an optimization, if
    // mpfVtpPtTranslateVAtoPA indicates failure at the 4KB level then we
    // already know the new page must also be a 4KB page, since 4KB pages
    // have already been mapped within the corresponding huge page's region.
    // The mpfOsGetPageSize() call is expensive, so avoid it when possible.
    if (*page_size != MPF_VTP_PAGE_4KB)
    {
        r = mpfOsGetPageSize(buf_addr, page_size);
        if (FPGA_OK != r)
        {
            // Strange. Perhaps the page isn't mapped. We will detect that
            // later, so soldier on with the worst-case assumption.
            *page_size = MPF_VTP_PAGE_4KB;

            if (_mpf_handle->dbg_mode)
            {
                MPF_FPGA_MSG("VTP error reading page size at VA %p. Assuming 4KB.", buf_addr);
            }
        }

        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("VTP kernel says page at VA %p is on a %s page", buf_addr,
                         mpfVtpPageSizeToString(*page_size));
        }
    }

    // Align the VA to the page size.
    size_t page_bytes = mpfPageSizeEnumToBytes(*page_size);
    buf_addr = (void*)((uintptr_t)buf_addr & ~(page_bytes - 1));

    if (_mpf_handle->dbg_mode)
    {
        MPF_FPGA_MSG("VTP pinning VA %p on a %s page", buf_addr,
                     mpfVtpPageSizeToString(*page_size));
    }

    if (NULL != flags)
    {
        *flags = 0;
    }

    // Pin the page and store it in the page table.
    fpga_result pin_result;
    pt_flags = MPF_VTP_PT_FLAG_IN_USE;
    r = mpfVtpPinAndInsertPage(_mpf_handle, true, buf_addr, *page_size,
                               pt_flags, ioaddr, &pin_result);
    if ((FPGA_OK != pin_result) && (MPF_VTP_PIN_MODE_TRY_READ_ONLY == mode))
    {
        // Pinning failed. Try read-only.
        if (_mpf_handle->dbg_mode)
        {
            MPF_FPGA_MSG("VTP retrying pinning VA %p on a %s page as READ ONLY",
                         buf_addr,
                         mpfVtpPageSizeToString(*page_size));
        }

        if (NULL != flags)
        {
#ifdef MFP_OPAE_HAS_BUF_READ_ONLY
            *flags = FPGA_BUF_READ_ONLY;
#endif
        }

        pt_flags |= MPF_VTP_PT_FLAG_READ_ONLY;
        r = mpfVtpPinAndInsertPage(_mpf_handle, true, buf_addr, *page_size,
                                   pt_flags, ioaddr, &pin_result);
    }

    mpfVtpPtUnlockMutex(pt);
    return r;
}


fpga_result __MPF_API__ mpfVtpInvalHWTLB(
    mpf_handle_t mpf_handle
)
{
    fpga_result r;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    // Nothing to do if now HW VTP
    if (! _mpf_handle->vtp.is_hw_vtp_available) return FPGA_OK;

    // Mode 2 blocks traffic and invalidates the FPGA-side TLB cache
    r = mpfWriteCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_MODE, 2);
    if (FPGA_OK != r) return r;

    return vtpEnable(_mpf_handle);
}


fpga_result __MPF_API__ mpfVtpInvalHWVAMapping(
    mpf_handle_t mpf_handle,
    mpf_vtp_pt_vaddr va
)
{
    return vtpInvalHWVAMapping(mpf_handle, va, false);
}


fpga_result __MPF_API__ mpfVtpSetMaxPhysPageSize(
    mpf_handle_t mpf_handle,
    mpf_vtp_page_size max_psize
)
{
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    if (max_psize > MPF_VTP_PAGE_1GB) return FPGA_INVALID_PARAM;

    _mpf_handle->vtp.max_physical_page_size = max_psize;
    return FPGA_OK;
}


fpga_result __MPF_API__ mpfVtpSync(
    mpf_handle_t mpf_handle,
    bool wait_for_sync
)
{
    fpga_result r = FPGA_OK;
    _mpf_handle_p _mpf_handle = (_mpf_handle_p)mpf_handle;

    if (_mpf_handle->vtp.munmap_monitor)
    {
        r = mpfVtpMonitorWaitWhenBusy(_mpf_handle, wait_for_sync);
    }

    return r;
}


fpga_result __MPF_API__ mpfVtpGetStats(
    mpf_handle_t mpf_handle,
    mpf_vtp_stats* stats
)
{
    // Is the VTP feature present?
    if (! mpfShimPresent(mpf_handle, CCI_MPF_SHIM_VTP))
    {
        memset(stats, -1, sizeof(mpf_vtp_stats));
        return FPGA_NOT_SUPPORTED;
    }

    stats->numTLBHits4KB = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_4KB_TLB_NUM_HITS, NULL);
    stats->numTLBMisses4KB = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_4KB_TLB_NUM_MISSES, NULL);
    stats->numTLBHits2MB = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_2MB_TLB_NUM_HITS, NULL);
    stats->numTLBMisses2MB = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_2MB_TLB_NUM_MISSES, NULL);
    stats->numPTWalkBusyCycles = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_PT_WALK_BUSY_CYCLES, NULL);
    stats->numFailedTranslations = mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_FAILED_TRANSLATIONS, NULL);

    stats->ptWalkLastVAddr = (void*)(CL(1) * mpfReadCsr(mpf_handle, CCI_MPF_SHIM_VTP, CCI_MPF_VTP_CSR_STAT_PT_WALK_LAST_VADDR, NULL));

    return FPGA_OK;
}
