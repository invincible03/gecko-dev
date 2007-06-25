/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2001
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Chris Saari <saari@netscape.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
/*
The Graphics Interchange Format(c) is the copyright property of CompuServe
Incorporated. Only CompuServe Incorporated is authorized to define, redefine,
enhance, alter, modify or change in any way the definition of the format.

CompuServe Incorporated hereby grants a limited, non-exclusive, royalty-free
license for the use of the Graphics Interchange Format(sm) in computer
software; computer software utilizing GIF(sm) must acknowledge ownership of the
Graphics Interchange Format and its Service Mark by CompuServe Incorporated, in
User and Technical Documentation. Computer software utilizing GIF, which is
distributed or may be distributed without User or Technical Documentation must
display to the screen or printer a message acknowledging ownership of the
Graphics Interchange Format and the Service Mark by CompuServe Incorporated; in
this case, the acknowledgement may be displayed in an opening screen or leading
banner, or a closing screen or trailing banner. A message such as the following
may be used:

    "The Graphics Interchange Format(c) is the Copyright property of
    CompuServe Incorporated. GIF(sm) is a Service Mark property of
    CompuServe Incorporated."

For further information, please contact :

    CompuServe Incorporated
    Graphics Technology Department
    5000 Arlington Center Boulevard
    Columbus, Ohio  43220
    U. S. A.

CompuServe Incorporated maintains a mailing list with all those individuals and
organizations who wish to receive copies of this document when it is corrected
or revised. This service is offered free of charge; please provide us with your
mailing address.
*/

#include <stddef.h>
#include "prtypes.h"
#include "prmem.h"
#include "prlog.h"
#include "GIF2.h"

#include "nsGIFDecoder2.h"
#include "nsIInputStream.h"
#include "nsIComponentManager.h"
#include "imgIContainerObserver.h"

#include "imgILoad.h"

#include "imgContainer.h"

/*
 * GETN(n, s) requests at least 'n' bytes available from 'q', at start of state 's'
 *
 * Note, the hold will never need to be bigger than 256 bytes to gather up in the hold,
 * as each GIF block (except colormaps) can never be bigger than 256 bytes.
 * Colormaps are directly copied in the resp. global_colormap or dynamically allocated local_colormap.
 * So a fixed buffer in gif_struct is good enough.
 * This buffer is only needed to copy left-over data from one GifWrite call to the next
 */
#define GETN(n,s)                      \
  PR_BEGIN_MACRO                       \
    mGIFStruct.bytes_to_consume = (n); \
    mGIFStruct.state = (s);            \
  PR_END_MACRO

/* Get a 16-bit value stored in little-endian format */
#define GETINT16(p)   ((p)[1]<<8|(p)[0])


//////////////////////////////////////////////////////////////////////
// GIF Decoder Implementation
// This is an adaptor between GIF2 and imgIDecoder

NS_IMPL_ISUPPORTS1(nsGIFDecoder2, imgIDecoder)

nsGIFDecoder2::nsGIFDecoder2()
  : mCurrentRow(-1)
  , mLastFlushedRow(-1)
  , mRGBLine(nsnull)
  , mRGBLineMaxSize(0)
  , mCurrentPass(0)
  , mLastFlushedPass(0)
  , mGIFOpen(PR_FALSE)
{
  // Clear out the structure, excluding the arrays
  memset(&mGIFStruct, 0, sizeof(mGIFStruct));
}

nsGIFDecoder2::~nsGIFDecoder2()
{
  Close();
}

//******************************************************************************
/** imgIDecoder methods **/
//******************************************************************************

//******************************************************************************
/* void init (in imgILoad aLoad); */
NS_IMETHODIMP nsGIFDecoder2::Init(imgILoad *aLoad)
{
  mObserver = do_QueryInterface(aLoad);

  mImageContainer = do_CreateInstance("@mozilla.org/image/container;1");
  aLoad->SetImage(mImageContainer);
  
  // Start with the version (GIF89a|GIF87a)
  mGIFStruct.state = gif_type;
  mGIFStruct.bytes_to_consume = 6;

  return NS_OK;
}


//******************************************************************************
/** nsIOutputStream methods **/
//******************************************************************************

//******************************************************************************
/* void close (); */
NS_IMETHODIMP nsGIFDecoder2::Close()
{
  if (mImageFrame) 
    EndImageFrame();
  EndGIF();

  PR_FREEIF(mGIFStruct.rowbuf);
  PR_FREEIF(mGIFStruct.local_colormap);
  PR_FREEIF(mRGBLine);

  return NS_OK;
}

//******************************************************************************
/* void flush (); */
NS_IMETHODIMP nsGIFDecoder2::Flush()
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

//******************************************************************************
/* static callback from nsIInputStream::ReadSegments */
static NS_METHOD ReadDataOut(nsIInputStream* in,
                             void* closure,
                             const char* fromRawSegment,
                             PRUint32 toOffset,
                             PRUint32 count,
                             PRUint32 *writeCount)
{
  nsGIFDecoder2 *decoder = NS_STATIC_CAST(nsGIFDecoder2*, closure);
  nsresult rv = decoder->ProcessData((unsigned char*)fromRawSegment, count, writeCount);
  if (NS_FAILED(rv)) {
    *writeCount = 0;
    return rv;
  }

  return NS_OK;
}

// Push any new rows according to mCurrentPass/mLastFlushedPass and
// mCurrentRow/mLastFlushedRow.  Note: caller is responsible for
// updating mlastFlushed{Row,Pass}.
void
nsGIFDecoder2::FlushImageData()
{
  PRInt32 imgWidth;
  mImageContainer->GetWidth(&imgWidth);
  nsIntRect frameRect;
  mImageFrame->GetRect(frameRect);
  
  switch (mCurrentPass - mLastFlushedPass) {
    case 0: {  // same pass
      PRInt32 remainingRows = mCurrentRow - mLastFlushedRow;
      if (remainingRows) {
        nsIntRect r(0, frameRect.y + mLastFlushedRow + 1,
                    imgWidth, remainingRows);
        mObserver->OnDataAvailable(nsnull, mImageFrame, &r);
      }    
    }
    break;
  
    case 1: {  // one pass on - need to handle bottom & top rects
      nsIntRect r(0, frameRect.y, imgWidth, mCurrentRow + 1);
      mObserver->OnDataAvailable(nsnull, mImageFrame, &r);
      nsIntRect r2(0, frameRect.y + mLastFlushedRow + 1,
                   imgWidth, frameRect.height - mLastFlushedRow - 1);
      mObserver->OnDataAvailable(nsnull, mImageFrame, &r2);
    }
    break;

    default: {  // more than one pass on - push the whole frame
      nsIntRect r(0, frameRect.y, imgWidth, frameRect.height);
      mObserver->OnDataAvailable(nsnull, mImageFrame, &r);
    }
  }
}

//******************************************************************************
nsresult nsGIFDecoder2::ProcessData(unsigned char *data, PRUint32 count, PRUint32 *_retval)
{
  // Push the data to the GIF decoder
  
  nsresult rv = GifWrite(data, count);
  NS_ENSURE_SUCCESS(rv, rv);

  if (mImageFrame && mObserver) {
    FlushImageData();
    mLastFlushedRow = mCurrentRow;
    mLastFlushedPass = mCurrentPass;
  }

  *_retval = count;

  return NS_OK;
}

//******************************************************************************
/* unsigned long writeFrom (in nsIInputStream inStr, in unsigned long count); */
NS_IMETHODIMP nsGIFDecoder2::WriteFrom(nsIInputStream *inStr, PRUint32 count, PRUint32 *_retval)
{
  nsresult rv = inStr->ReadSegments(ReadDataOut, this,  count, _retval);

  /* necko doesn't propagate the errors from ReadDataOut - take matters
     into our own hands.  if we have at least one frame of an animated
     gif, then return success so we keep displaying as much as possible. */
  if (NS_SUCCEEDED(rv) && mGIFStruct.state == gif_error) {
    PRUint32 numFrames = 0;
    if (mImageContainer)
      mImageContainer->GetNumFrames(&numFrames);
    if (numFrames <= 0)
      return NS_ERROR_FAILURE;
  }

  return rv;
}


//******************************************************************************
// GIF decoder callback methods. Part of public API for GIF2
//******************************************************************************

//******************************************************************************
void nsGIFDecoder2::BeginGIF()
{
  // If we have passed an illogical screen size, bail and hope that we'll get
  // set later by the first frame's local image header.
  if (mGIFStruct.screen_width == 0 || mGIFStruct.screen_height == 0)
    return;
    
  if (mObserver)
    mObserver->OnStartDecode(nsnull);

  mImageContainer->Init(mGIFStruct.screen_width, mGIFStruct.screen_height, mObserver);

  if (mObserver)
    mObserver->OnStartContainer(nsnull, mImageContainer);

  mGIFOpen = PR_TRUE;
}

//******************************************************************************
void nsGIFDecoder2::EndGIF()
{
  if (!mGIFOpen)
    return;

  if (mObserver) {
    mObserver->OnStopContainer(nsnull, mImageContainer);
    mObserver->OnStopDecode(nsnull, NS_OK, nsnull);
  }
  
  mImageContainer->SetLoopCount(mGIFStruct.loop_count);
  mImageContainer->DecodingComplete();

  mGIFOpen = PR_FALSE;
}

//******************************************************************************
void nsGIFDecoder2::BeginImageFrame()
{
  mImageFrame = nsnull; // clear out our current frame reference

  if (!mGIFStruct.images_decoded) {
    // Send a onetime OnDataAvailable (Display Refresh) for the first frame
    // if it has a y-axis offset.  Otherwise, the area may never be refreshed
    // and the placeholder will remain on the screen. (Bug 37589)
    if (mGIFStruct.y_offset > 0) {
      PRInt32 imgWidth;
      mImageContainer->GetWidth(&imgWidth);
      nsIntRect r(0, 0, imgWidth, mGIFStruct.y_offset);
      mObserver->OnDataAvailable(nsnull, mImageFrame, &r);
    }
  }
}

//******************************************************************************
void nsGIFDecoder2::EndImageFrame()
{
  // An image can specify a delay time before which to display
  // subsequent images.
  if (mGIFStruct.delay_time < MINIMUM_DELAY_TIME)
    mGIFStruct.delay_time = MINIMUM_DELAY_TIME;

  mGIFStruct.images_decoded++;

  // If mImageFrame hasn't been initialized, call HaveDecodedRow to init it
  // One reason why it may not be initialized is because the frame
  // is out of the bounds of the image.
  if (!mImageFrame) {
    HaveDecodedRow(nsnull,0,0,0);
  } else {
    // We actually have the timeout information before we get the lzw encoded 
    // image data, at least according to the spec, but we delay in setting the 
    // timeout for the image until here to help ensure that we have the whole 
    // image frame decoded before we go off and try to display another frame.
    mImageFrame->SetTimeout(mGIFStruct.delay_time);
  }
  mImageContainer->EndFrameDecode(mGIFStruct.images_decoded, mGIFStruct.delay_time);

  if (mObserver && mImageFrame) {
    FlushImageData();

    if (mGIFStruct.images_decoded == 1) {
      // If the first frame is smaller in height than the entire image, send a
      // OnDataAvailable (Display Refresh) for the area it does not have data for.
      // This will clear the remaining bits of the placeholder. (Bug 37589)
      PRInt32 imgHeight;
      PRInt32 realFrameHeight = mGIFStruct.height + mGIFStruct.y_offset;

      mImageContainer->GetHeight(&imgHeight);
      if (imgHeight > realFrameHeight) {
        PRInt32 imgWidth;
        mImageContainer->GetWidth(&imgWidth);

        nsIntRect r(0, realFrameHeight, imgWidth, imgHeight - realFrameHeight);
        mObserver->OnDataAvailable(nsnull, mImageFrame, &r);
      }
    }

    mCurrentRow = mLastFlushedRow = -1;
    mCurrentPass = mLastFlushedPass = 0;

    mObserver->OnStopFrame(nsnull, mImageFrame);
  }

  // Clear state from this image
  mImageFrame = nsnull;
  mGIFStruct.is_local_colormap_defined = PR_FALSE;
  mGIFStruct.is_transparent = PR_FALSE;
}
  
//******************************************************************************
// GIF decoder callback notification that it has decoded a row
void nsGIFDecoder2::HaveDecodedRow(
  PRUint8* aRowBufPtr,   // Pointer to single scanline temporary buffer
  int aRowNumber,        // Row number?
  int aDuplicateCount,   // Number of times to duplicate the row?
  int aInterlacePass)    // interlace pass (1-4)
{
  const PRUint32 bpr = mGIFStruct.width * sizeof(PRUint32);

  // We have to delay allocation of the image frame until now because
  // we won't have control block info (transparency) until now. The control
  // block of a GIF stream shows up after the image header since transparency
  // is added in GIF89a and control blocks are how the extensions are done.
  // How annoying.
  if (!mImageFrame) {
    gfx_format format = gfxIFormats::RGB;
    if (mGIFStruct.is_transparent) {
      format = gfxIFormats::RGB_A1;  // XXX not really
    }

    // initialize the frame and append it to the container
    mImageFrame = do_CreateInstance("@mozilla.org/gfx/image/frame;2");
    if (!mImageFrame || NS_FAILED(mImageFrame->Init(
          mGIFStruct.x_offset, mGIFStruct.y_offset, 
          mGIFStruct.width, mGIFStruct.height, format, 24))) {
      mImageFrame = 0;
      return;
    }

    mImageFrame->SetFrameDisposalMethod(mGIFStruct.disposal_method);
    mImageContainer->AppendFrame(mImageFrame);

    if (mObserver)
      mObserver->OnStartFrame(nsnull, mImageFrame);

    if (bpr > mRGBLineMaxSize) {
      mRGBLine = (PRUint8 *)PR_REALLOC(mRGBLine, bpr);
      mRGBLineMaxSize = bpr;
    }
  }
  
  if (aRowBufPtr) {
    // Map the data into colors
    int cmapsize = mGIFStruct.global_colormap_size;
    PRUint8* cmap = mGIFStruct.global_colormap;
    if (mGIFStruct.is_local_colormap_defined) {
      cmapsize = mGIFStruct.local_colormap_size;
      cmap = mGIFStruct.local_colormap;
    }

    if (!cmap) { // cmap could have null value if the global color table flag is 0
      nsIntRect r(0, aRowNumber, mGIFStruct.width, aDuplicateCount);
      imgContainer::ClearFrame(mImageFrame, r);
    } else {
      PRUint8* rowBufIndex = aRowBufPtr;
      PRUint32* rgbRowIndex = (PRUint32*)mRGBLine;

      const PRInt32 tpixel = 
        mGIFStruct.is_transparent ? mGIFStruct.tpixel : -1;

      while (rowBufIndex != mGIFStruct.rowend) {
        if (*rowBufIndex >= cmapsize || *rowBufIndex == tpixel) {
          *rgbRowIndex++ = 0x00000000;
          ++rowBufIndex;
          continue;
        }

        PRUint32 colorIndex = *rowBufIndex * 3;
        *rgbRowIndex++ = (0xFF << 24) |
          (cmap[colorIndex] << 16) |
          (cmap[colorIndex+1] << 8) |
          (cmap[colorIndex+2]);
        ++rowBufIndex;
      }
      for (int i=0; i<aDuplicateCount; i++)
        mImageFrame->SetImageData(mRGBLine, bpr, (aRowNumber+i)*bpr);
    }

    mCurrentRow = aRowNumber + aDuplicateCount - 1;
    mCurrentPass = aInterlacePass;
    if (aInterlacePass == 1)
      mLastFlushedPass = aInterlacePass;   // interlaced starts at 1
  }
}


//******************************************************************************
// Send the data to the display front-end.
PRUint32 nsGIFDecoder2::OutputRow()
{
  int width, drow_start, drow_end;

  drow_start = drow_end = mGIFStruct.irow;

  /*
   * Haeberli-inspired hack for interlaced GIFs: Replicate lines while
   * displaying to diminish the "venetian-blind" effect as the image is
   * loaded. Adjust pixel vertical positions to avoid the appearance of the
   * image crawling up the screen as successive passes are drawn.
   */
  if (mGIFStruct.progressive_display && mGIFStruct.interlaced && (mGIFStruct.ipass < 4)) {
    /* ipass = 1,2,3 results in resp. row_dup = 7,3,1 and row_shift = 3,1,0 */
    const PRUint32 row_dup = 15 >> mGIFStruct.ipass;
    const PRUint32 row_shift = row_dup >> 1;

    drow_start -= row_shift;
    drow_end = drow_start + row_dup;

    /* Extend if bottom edge isn't covered because of the shift upward. */
    if (((mGIFStruct.height - 1) - drow_end) <= row_shift)
      drow_end = mGIFStruct.height - 1;

    /* Clamp first and last rows to upper and lower edge of image. */
    if (drow_start < 0)
      drow_start = 0;
    if ((PRUintn)drow_end >= mGIFStruct.height)
      drow_end = mGIFStruct.height - 1;
  }

  /* Protect against too much image data */
  if ((PRUintn)drow_start >= mGIFStruct.height) {
    NS_WARNING("GIF2.cpp::OutputRow - too much image data");
    return 0;
  }

  /* Check for scanline below edge of logical screen */
  if ((mGIFStruct.y_offset + mGIFStruct.irow) < mGIFStruct.screen_height) {
    /* Clip if right edge of image exceeds limits */
    if ((mGIFStruct.x_offset + mGIFStruct.width) > mGIFStruct.screen_width)
      width = mGIFStruct.screen_width - mGIFStruct.x_offset;
    else
      width = mGIFStruct.width;

    if (width > 0)
      /* Decoded data available callback */
      HaveDecodedRow(
        mGIFStruct.rowbuf,                // Pointer to single scanline temporary buffer
        drow_start,                // Row number
        drow_end - drow_start + 1, // Number of times to duplicate the row?
        mGIFStruct.ipass);                // interlace pass (1-4)
  }

  mGIFStruct.rowp = mGIFStruct.rowbuf;

  if (!mGIFStruct.interlaced) {
    mGIFStruct.irow++;
  } else {
    static const PRUint8 kjump[5] = { 1, 8, 8, 4, 2 };
    do {
      // Row increments resp. per 8,8,4,2 rows
      mGIFStruct.irow += kjump[mGIFStruct.ipass];
      if (mGIFStruct.irow >= mGIFStruct.height) {
        // Next pass starts resp. at row 4,2,1,0
        mGIFStruct.irow = 8 >> mGIFStruct.ipass;
        mGIFStruct.ipass++;
      }
    } while (mGIFStruct.irow >= mGIFStruct.height);
  }

  return --mGIFStruct.rows_remaining;
}

//******************************************************************************
/* Perform Lempel-Ziv-Welch decoding */
PRBool
nsGIFDecoder2::DoLzw(const PRUint8 *q)
{
  /* Copy all the decoder state variables into locals so the compiler
   * won't worry about them being aliased.  The locals will be homed
   * back into the GIF decoder structure when we exit.
   */
  int avail       = mGIFStruct.avail;
  int bits        = mGIFStruct.bits;
  int codesize    = mGIFStruct.codesize;
  int codemask    = mGIFStruct.codemask;
  int count       = mGIFStruct.count;
  int oldcode     = mGIFStruct.oldcode;
  const int clear_code = ClearCode();
  PRUint8 firstchar = mGIFStruct.firstchar;
  PRInt32 datum     = mGIFStruct.datum;
  PRUint16 *prefix  = mGIFStruct.prefix;
  PRUint8 *stackp   = mGIFStruct.stackp;
  PRUint8 *suffix   = mGIFStruct.suffix;
  PRUint8 *stack    = mGIFStruct.stack;
  PRUint8 *rowp     = mGIFStruct.rowp;
  PRUint8 *rowend   = mGIFStruct.rowend;

  if (rowp == rowend)
    return PR_TRUE;

#define OUTPUT_ROW()                                        \
  PR_BEGIN_MACRO                                            \
    if (!OutputRow())                                       \
      goto END;                                             \
    rowp = mGIFStruct.rowp;                                 \
  PR_END_MACRO

  for (const PRUint8* ch = q; count-- > 0; ch++)
  {
    /* Feed the next byte into the decoder's 32-bit input buffer. */
    datum += ((int32) *ch) << bits;
    bits += 8;

    /* Check for underflow of decoder's 32-bit input buffer. */
    while (bits >= codesize)
    {
      /* Get the leading variable-length symbol from the data stream */
      int code = datum & codemask;
      datum >>= codesize;
      bits -= codesize;

      /* Reset the dictionary to its original state, if requested */
      if (code == clear_code) {
        codesize = mGIFStruct.datasize + 1;
        codemask = (1 << codesize) - 1;
        avail = clear_code + 2;
        oldcode = -1;
        continue;
      }

      /* Check for explicit end-of-stream code */
      if (code == (clear_code + 1)) {
        /* end-of-stream should only appear after all image data */
        return (mGIFStruct.rows_remaining == 0);
      }

      if (oldcode == -1) {
        *rowp++ = suffix[code];
        if (rowp == rowend)
          OUTPUT_ROW();

        firstchar = oldcode = code;
        continue;
      }

      int incode = code;
      if (code >= avail) {
        *stackp++ = firstchar;
        code = oldcode;

        if (stackp == stack + MAX_BITS)
          return PR_FALSE;
      }

      while (code >= clear_code)
      {
        if (code == prefix[code])
          return PR_FALSE;

        *stackp++ = suffix[code];
        code = prefix[code];

        if (stackp == stack + MAX_BITS)
          return PR_FALSE;
      }

      *stackp++ = firstchar = suffix[code];

      /* Define a new codeword in the dictionary. */
      if (avail < 4096) {
        prefix[avail] = oldcode;
        suffix[avail] = firstchar;
        avail++;

        /* If we've used up all the codewords of a given length
         * increase the length of codewords by one bit, but don't
         * exceed the specified maximum codeword size of 12 bits.
         */
        if (((avail & codemask) == 0) && (avail < 4096)) {
          codesize++;
          codemask += avail;
        }
      }
      oldcode = incode;

      /* Copy the decoded data out to the scanline buffer. */
      do {
        *rowp++ = *--stackp;
        if (rowp == rowend)
          OUTPUT_ROW();
      } while (stackp > stack);
    }
  }

  END:

  /* Home the local copies of the GIF decoder state variables */
  mGIFStruct.avail = avail;
  mGIFStruct.bits = bits;
  mGIFStruct.codesize = codesize;
  mGIFStruct.codemask = codemask;
  mGIFStruct.count = count;
  mGIFStruct.oldcode = oldcode;
  mGIFStruct.firstchar = firstchar;
  mGIFStruct.datum = datum;
  mGIFStruct.stackp = stackp;
  mGIFStruct.rowp = rowp;

  return PR_TRUE;
}

/******************************************************************************/
/*
 * process data arriving from the stream for the gif decoder
 */

nsresult nsGIFDecoder2::GifWrite(const PRUint8 *buf, PRUint32 len)
{
  if (!buf || !len)
    return NS_ERROR_FAILURE;

  const PRUint8 *q = buf;

  // Add what we have sofar to the block
  // If previous call to me left something in the hold first complete current block
  // Or if we are filling the colormaps, first complete the colormap
  PRUint8* p = (mGIFStruct.state == gif_global_colormap) ? mGIFStruct.global_colormap :
               (mGIFStruct.state == gif_image_colormap) ? mGIFStruct.local_colormap :
               (mGIFStruct.bytes_in_hold) ? mGIFStruct.hold : nsnull;
  if (p) {
    // Add what we have sofar to the block
    PRUint32 l = PR_MIN(len, mGIFStruct.bytes_to_consume);
    memcpy(p+mGIFStruct.bytes_in_hold, buf, l);

    if (l < mGIFStruct.bytes_to_consume) {
      // Not enough in 'buf' to complete current block, get more
      mGIFStruct.bytes_in_hold += l;
      mGIFStruct.bytes_to_consume -= l;
      return NS_OK;
    }
    // Reset hold buffer count
    mGIFStruct.bytes_in_hold = 0;
    // Point 'q' to complete block in hold (or in colormap)
    q = p;
  }

  // Invariant:
  //    'q' is start of current to be processed block (hold, colormap or buf)
  //    'bytes_to_consume' is number of bytes to consume from 'buf'
  //    'buf' points to the bytes to be consumed from the input buffer
  //    'len' is number of bytes left in input buffer from position 'buf'.
  //    At entrance of the for loop will 'buf' will be moved 'bytes_to_consume'
  //    to point to next buffer, 'len' is adjusted accordingly.
  //    So that next round in for loop, q gets pointed to the next buffer.

  for (;len >= mGIFStruct.bytes_to_consume; q=buf) {
    // Eat the current block from the buffer, q keeps pointed at current block
    buf += mGIFStruct.bytes_to_consume;
    len -= mGIFStruct.bytes_to_consume;

    switch (mGIFStruct.state)
    {
    case gif_lzw:
      if (!DoLzw(q)) {
        mGIFStruct.state = gif_error;
        break;
      }
      GETN(1, gif_sub_block);
      break;

    case gif_lzw_start:
    {
      /* Initialize LZW parser/decoder */
      mGIFStruct.datasize = *q;
      const int clear_code = ClearCode();
      if (mGIFStruct.datasize > MAX_LZW_BITS ||
          clear_code >= MAX_BITS) {
        mGIFStruct.state = gif_error;
        break;
      }

      mGIFStruct.avail = clear_code + 2;
      mGIFStruct.oldcode = -1;
      mGIFStruct.codesize = mGIFStruct.datasize + 1;
      mGIFStruct.codemask = (1 << mGIFStruct.codesize) - 1;
      mGIFStruct.datum = mGIFStruct.bits = 0;

      /* init the tables */
      for (int i = 0; i < clear_code; i++)
        mGIFStruct.suffix[i] = i;

      mGIFStruct.stackp = mGIFStruct.stack;

      GETN(1, gif_sub_block);
    }
    break;

    /* All GIF files begin with "GIF87a" or "GIF89a" */
    case gif_type:
      if (!strncmp((char*)q, "GIF89a", 6)) {
        mGIFStruct.version = 89;
      } else if (!strncmp((char*)q, "GIF87a", 6)) {
        mGIFStruct.version = 87;
      } else {
        mGIFStruct.state = gif_error;
        break;
      }
      GETN(7, gif_global_header);
      break;

    case gif_global_header:
      /* This is the height and width of the "screen" or
       * frame into which images are rendered.  The
       * individual images can be smaller than the
       * screen size and located with an origin anywhere
       * within the screen.
       */

      mGIFStruct.screen_width = GETINT16(q);
      mGIFStruct.screen_height = GETINT16(q + 2);
      mGIFStruct.global_colormap_size = 2<<(q[4]&0x07);

      // screen_bgcolor is not used
      //mGIFStruct.screen_bgcolor = q[5];
      // q[6] = Pixel Aspect Ratio
      //   Not used
      //   float aspect = (float)((q[6] + 15) / 64.0);

      // Start the GIF container
      BeginGIF();

      if (q[4] & 0x80) { /* global map */
        // Get the global colormap
        const PRUint32 size = 3*mGIFStruct.global_colormap_size;
        if (len < size) {
          // Use 'hold' pattern to get the global colormap
          GETN(size, gif_global_colormap);
          break;
        }
        // Copy everything and directly go to gif_lzw_start
        memcpy(mGIFStruct.global_colormap, buf, size);
        buf += size;
        len -= size;
      }

      GETN(1, gif_image_start);
      break;

    case gif_global_colormap:
      // Everything is already copied into global_colormap
      GETN(1, gif_image_start);
      break;

    case gif_image_start:
      switch (*q) {
        case ';':  /* terminator */
          mGIFStruct.state = gif_done;
          break;

        case '!': /* extension */
          GETN(2, gif_extension);
          break;

        case ',':
          GETN(9, gif_image_header);
          break;

        default:
          /* If we get anything other than ',' (image separator), '!'
           * (extension), or ';' (trailer), there is extraneous data
           * between blocks. The GIF87a spec tells us to keep reading
           * until we find an image separator, but GIF89a says such
           * a file is corrupt. We follow GIF89a and bail out. */
          if (mGIFStruct.images_decoded > 0) {
            /* The file is corrupt, but one or more images have
             * been decoded correctly. In this case, we proceed
             * as if the file were correctly terminated and set
             * the state to gif_done, so the GIF will display.
             */
            mGIFStruct.state = gif_done;
          } else {
            /* No images decoded, there is nothing to display. */
            mGIFStruct.state = gif_error;
          }
      }
      break;

    case gif_extension:
      mGIFStruct.bytes_to_consume = q[1];
      if (mGIFStruct.bytes_to_consume) {
        switch (*q) {
        case 0xf9:
          mGIFStruct.state = gif_control_extension;
          break;
  
        case 0xff:
          mGIFStruct.state = gif_application_extension;
          break;
  
        case 0xfe:
          mGIFStruct.state = gif_consume_comment;
          break;
  
        default:
          mGIFStruct.state = gif_skip_block;
        }
      } else {
        GETN(1, gif_image_start);
      }
      break;

    case gif_consume_block:
      if (!*q)
        GETN(1, gif_image_start);
      else
        GETN(*q, gif_skip_block);
      break;

    case gif_skip_block:
      GETN(1, gif_consume_block);
      break;

    case gif_control_extension:
      mGIFStruct.is_transparent = *q & 0x1;
      mGIFStruct.tpixel = q[3];
      mGIFStruct.disposal_method = ((*q) >> 2) & 0x7;
      // Some specs say 3rd bit (value 4), other specs say value 3
      // Let's choose 3 (the more popular)
      if (mGIFStruct.disposal_method == 4)
        mGIFStruct.disposal_method = 3;
      mGIFStruct.delay_time = GETINT16(q + 1) * 10;
      GETN(1, gif_consume_block);
      break;

    case gif_comment_extension:
      if (*q)
        GETN(*q, gif_consume_comment);
      else
        GETN(1, gif_image_start);
      break;

    case gif_consume_comment:
      GETN(1, gif_comment_extension);
      break;

    case gif_application_extension:
      /* Check for netscape application extension */
      if (!strncmp((char*)q, "NETSCAPE2.0", 11) ||
        !strncmp((char*)q, "ANIMEXTS1.0", 11))
        GETN(1, gif_netscape_extension_block);
      else
        GETN(1, gif_consume_block);
      break;

    /* Netscape-specific GIF extension: animation looping */
    case gif_netscape_extension_block:
      if (*q)
        GETN(*q, gif_consume_netscape_extension);
      else
        GETN(1, gif_image_start);
      break;

    /* Parse netscape-specific application extensions */
    case gif_consume_netscape_extension:
      switch (q[0] & 7) {
        case 1:
          /* Loop entire animation specified # of times.  Only read the
             loop count during the first iteration. */
          mGIFStruct.loop_count = GETINT16(q + 1);
  
          /* Zero loop count is infinite animation loop request */
          if (mGIFStruct.loop_count == 0)
            mGIFStruct.loop_count = -1;
  
          GETN(1, gif_netscape_extension_block);
          break;
        
        case 2:
          /* Wait for specified # of bytes to enter buffer */
          // Don't do this, this extension doesn't exist (isn't used at all) 
          // and doesn't do anything, as our streaming/buffering takes care of it all...
          // See: http://semmix.pl/color/exgraf/eeg24.htm
          GETN(1, gif_netscape_extension_block);
          break;
  
        default:
          // 0,3-7 are yet to be defined netscape extension codes
          mGIFStruct.state = gif_error;
      }
      break;

    case gif_image_header:
      /* Get image offsets, with respect to the screen origin */
      mGIFStruct.x_offset = GETINT16(q);
      mGIFStruct.y_offset = GETINT16(q + 2);

      /* Get image width and height. */
      mGIFStruct.width  = GETINT16(q + 4);
      mGIFStruct.height = GETINT16(q + 6);

      /* Work around broken GIF files where the logical screen
       * size has weird width or height.  We assume that GIF87a
       * files don't contain animations.
       */
      if (!mGIFStruct.images_decoded &&
          ((mGIFStruct.screen_height < mGIFStruct.height) ||
           (mGIFStruct.screen_width < mGIFStruct.width) ||
           (mGIFStruct.version == 87)))
      {
        mGIFStruct.screen_height = mGIFStruct.height;
        mGIFStruct.screen_width = mGIFStruct.width;
        mGIFStruct.x_offset = 0;
        mGIFStruct.y_offset = 0;

        BeginGIF();
      }

      /* Work around more broken GIF files that have zero image
         width or height */
      if (!mGIFStruct.height || !mGIFStruct.width) {
        mGIFStruct.height = mGIFStruct.screen_height;
        mGIFStruct.width = mGIFStruct.screen_width;
        if (!mGIFStruct.height || !mGIFStruct.width) {
          mGIFStruct.state = gif_error;
          break;
        }
      }

      BeginImageFrame();

      /* This case will never be taken if this is the first image */
      /* being decoded. If any of the later images are larger     */
      /* than the screen size, we need to reallocate buffers.     */
      if (mGIFStruct.screen_width < mGIFStruct.width) {
        /* XXX Deviant! */

        mGIFStruct.rowbuf = (PRUint8*)PR_REALLOC(mGIFStruct.rowbuf, mGIFStruct.width);
        mGIFStruct.screen_width = mGIFStruct.width;
      } else if (!mGIFStruct.rowbuf) {
          mGIFStruct.rowbuf = (PRUint8*)PR_MALLOC(mGIFStruct.screen_width);
      }

      if (!mGIFStruct.rowbuf) {
          mGIFStruct.state = gif_oom;
          break;
      }
      if (mGIFStruct.screen_height < mGIFStruct.height)
        mGIFStruct.screen_height = mGIFStruct.height;

      if (q[8] & 0x40) {
        mGIFStruct.interlaced = PR_TRUE;
        mGIFStruct.ipass = 1;
      } else {
        mGIFStruct.interlaced = PR_FALSE;
        mGIFStruct.ipass = 0;
      }

      /* Only apply the Haeberli display hack on the first frame */
      mGIFStruct.progressive_display = (mGIFStruct.images_decoded == 0);

      /* Clear state from last image */
      mGIFStruct.irow = 0;
      mGIFStruct.rows_remaining = mGIFStruct.height;
      mGIFStruct.rowend = mGIFStruct.rowbuf + mGIFStruct.width;
      mGIFStruct.rowp = mGIFStruct.rowbuf;

      /* bits per pixel is q[8]&0x07 */

      if (q[8] & 0x80) /* has a local colormap? */
      {
        const int num_colors = 2 << (q[8] & 0x7);
        const PRUint32 size = 3*num_colors;
        PRUint8 *map = mGIFStruct.local_colormap;
        if (!map || (num_colors > mGIFStruct.local_colormap_size)) {
          map = (PRUint8*)PR_REALLOC(map, size);
          if (!map) {
            mGIFStruct.state = gif_oom;
            break;
          }
          mGIFStruct.local_colormap = map;
        }

        /* Switch to the new local palette after it loads */
        mGIFStruct.local_colormap_size = num_colors;
        mGIFStruct.is_local_colormap_defined = PR_TRUE;

        if (len < size) {
          // Use 'hold' pattern to get the image colormap
          GETN(size, gif_image_colormap);
          break;
        }
        // Copy everything and directly go to gif_lzw_start
        memcpy(mGIFStruct.local_colormap, buf, size);
        buf += size;
        len -= size;
      } else {
        /* Switch back to the global palette */
        mGIFStruct.is_local_colormap_defined = PR_FALSE;
      }
      GETN(1, gif_lzw_start);
      break;

    case gif_image_colormap:
      // Everything is already copied into local_colormap
      GETN(1, gif_lzw_start);
      break;

    case gif_sub_block:
      mGIFStruct.count = *q;
      if (mGIFStruct.count) {
        /* Still working on the same image: Process next LZW data block */
        /* Make sure there are still rows left. If the GIF data */
        /* is corrupt, we may not get an explicit terminator.   */
        if (!mGIFStruct.rows_remaining) {
#ifdef DONT_TOLERATE_BROKEN_GIFS
          mGIFStruct.state = gif_error;
#else
          /* This is an illegal GIF, but we remain tolerant. */
          GETN(1, gif_sub_block);
#endif
          break;
        }
        GETN(mGIFStruct.count, gif_lzw);
      } else {
        /* See if there are any more images in this sequence. */
        EndImageFrame();
        GETN(1, gif_image_start);
      }
      break;

    case gif_done:
    case gif_error:
      EndGIF();
      return NS_OK;
      break;

    // Handle out of memory errors
    case gif_oom:
      return NS_ERROR_OUT_OF_MEMORY;

    // We shouldn't ever get here.
    default:
      break;
    }
  }

  // Copy the leftover into mGIFStruct.hold
  mGIFStruct.bytes_in_hold = len;
  if (len) {
    // Add what we have sofar to the block
    PRUint8* p = (mGIFStruct.state == gif_global_colormap) ? mGIFStruct.global_colormap :
                 (mGIFStruct.state == gif_image_colormap) ? mGIFStruct.local_colormap :
                 mGIFStruct.hold;
    memcpy(p, buf, len);
    mGIFStruct.bytes_to_consume -= len;
  }

  return NS_OK;
}
