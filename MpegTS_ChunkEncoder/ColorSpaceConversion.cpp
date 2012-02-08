// ColorSpaceConversion.cpp
// Handles some frame conversion between FFMPEG frames.
// Written because swscale is a leaky mess.

#include "stdafx.h"
#include "MpegTS_ChunkEncoder.h"
#include "FrameWave/fwImage.h"

extern "C" {  
#include "libavformat/avformat.h"
}

// a whole bunch of tables for speed
#include "YUVMultipliers.h"

#define clip(A) ((A>255)?(255):((A<0)?(0):(A)))

/// Directly convert an RGB24/8 buffer into a set of equally sized 8bit-per-sample YUV444 buffers
void DLL Rgb2YuvIS(int width, int height, uint8_t *RgbSrc, uint8_t *Y, uint8_t *U, uint8_t *V) {
	uint8_t *s = RgbSrc;
	uint8_t *y = Y, *u = U, *v = V;
	for (int z = height - 1; z >= 0; z--) {
		s = RgbSrc + (z * width * 3);
		for (int h = 0; h < width; h++) { 
			int r = *(s++);
			int g = *(s++);
			int b = *(s++);
			*(y) = _A[r] + _B[g] + _C[b] + 16;
			*(u) = -_G[r] - _H[g] + _D[b] + 128;
			*(v) = _D[r] - _E[g] - _F[b] + 128;
			y++; u++; v++;
		}
	}
}

// transposable rescale -- works for horz and vert.
void RescaleFence (uint8_t *Src, uint8_t *Dst, int SrcStart, int SrcStride, int SrcLength, int DstStart, int DstStride, int DstLength) {
	int NumPixels = (DstLength > SrcLength) ? (DstLength) : (SrcLength);
	int E = 0;
	int srcw = SrcLength;
	int dstw = DstLength;

	int Mid = NumPixels >> 1;
	bool slope = false;
	if (SrcLength > DstLength) {
		slope = true;
		srcw = DstLength;
		dstw = SrcLength;
	} else {
		NumPixels--;
	}
	int o = DstStart;
	int i = SrcStart;

	int v = Src[i];
	E = 0;

	int p = NumPixels - 2;
	if (DstLength > SrcLength) p--;
	while (p-- > 0) {
		if (E < Mid) { // do interpolation
			v = (short)((v + Src[i + SrcStride]) >> 1);
		}
		E += srcw;
		Dst[o] = v;

		if (slope) i+= SrcStride;
		else o += DstStride;

		if (E >= dstw) {
			E -= dstw;
			if (slope) o += DstStride;
			else i += SrcStride;
			v = Src[i];
		}
	}
	if (DstLength > SrcLength) {
		Dst[o] = v;
	}
}

// DOWNSCALING-ONLY IMPLEMENTATION. DO NOT CALL! (called by PlanarScale() )
void RescaleFence_DOWN (uint8_t *Src, uint8_t *Dst, int SrcStart, int SrcStride, int SrcLength, int DstStart, int DstStride, int DstLength) {
	int NumPixels = SrcLength;
	int E = 0;
	int srcw = DstLength;
	int dstw = SrcLength;
	int Mid = NumPixels >> 1;
	bool slope = true;
	int o = DstStart;
	int i = SrcStart;
	int v = Src[i];
	E = 0;

	int p = NumPixels - 1;
	while (p-- > 0) {
		if (E < Mid) { // do interpolation
			v = (short)((v + Src[i + SrcStride]) >> 1);
		}
		E += srcw;
		Dst[o] = v;
		i+= SrcStride;

		if (E >= dstw) {
			E -= dstw;
			o += DstStride;
			v = Src[i];
		}
	}
}


/// Rescale an interleaved image. Src and Dst buffers should be equal sized.
void DLL InterleavedScale(uint8_t *Src, uint8_t *Dst, int SrcWidth, int SrcHeight, int DstWidth, int DstHeight, bool HighQuality) {
	if (SrcHeight == DstHeight && SrcWidth == DstHeight) {
		// Exactly the same size. Just copy.
		int length = SrcHeight * SrcWidth;
		for (int i = 0; i < length; i++) Dst[i] = Src[i];
		return;
	}

	int interp_mode = (HighQuality) ? ((int)FWI_INTER_LINEAR) : ((int)FWI_INTER_NN); //HQ used for Y; not for U,V.

	double x_scale = (double)DstWidth / (double)SrcWidth;
	double y_scale = (double)DstHeight / (double)SrcHeight;

	FwiSize srcRoiSize;
	srcRoiSize.height = SrcHeight;
	srcRoiSize.width = SrcWidth;

	FwiRect srcROI;
	srcROI.height = SrcHeight;
	srcROI.width = SrcWidth;
	srcROI.x = 0;
	srcROI.y = 0;

	FwiRect dstROI;
	dstROI.height = DstHeight;
	dstROI.width = DstWidth;
	dstROI.x = 0;
	dstROI.y = 0;

	FwStatus status = fwiResizeSqrPixel_8u_C3R(Src, srcRoiSize, SrcWidth * 3, srcROI,
		Dst, DstWidth * 3, dstROI, x_scale, y_scale, 0, 0, interp_mode, Dst);
}





/// Rescale an image plane. Src and Dst buffers should be equal sized.
void DLL PlanarScale(uint8_t *Src, uint8_t *Dst, int SrcWidth, int SrcHeight, int DstWidth, int DstHeight, bool HighQuality) {
	if (SrcHeight == DstHeight && SrcWidth == DstHeight) {
		// Exactly the same size. Just copy.
		int length = SrcHeight * SrcWidth;
		for (int i = 0; i < length; i++) Dst[i] = Src[i];
		return;
	}
	
	if (SrcHeight > DstHeight && SrcWidth > DstWidth) { // down scaling
		for (int x = 0; x < SrcWidth; x++) {
			RescaleFence_DOWN(Src, Dst, // copy Src to dst, scaling vertically
				x, // start at top of column
				SrcWidth, // Move 1 row at a time source
				SrcHeight, // source length
				
				x, // start at top of column
				SrcWidth, // Move 1 row at a time in dest (equally spaced to src)
				DstHeight); // dest length
		}
		for (int y = 0; y < DstHeight; y++) {
			RescaleFence_DOWN(Dst, Dst, // copy Dst onto itself, scaling horizontally and packing rows together in memory
				(y * SrcWidth), // start at left of row
				1, // Move 1 column at a time (1 pixel)
				SrcWidth, // source length
				
				(y * DstWidth), // start at left of row (in packed space)
				1, // Move 1 column at a time (1 pixel)
				DstWidth); // dest length
		}
	} else { // some upscaling -- use FrameWave
		int interp_mode = (HighQuality) ? ((int)FWI_INTER_LINEAR) : ((int)FWI_INTER_NN); //HQ used for Y; not for U,V.

		double x_scale = (double)DstWidth / (double)SrcWidth;
		double y_scale = (double)DstHeight / (double)SrcHeight;
		
		FwiSize srcRoiSize;
		srcRoiSize.height = SrcHeight;
		srcRoiSize.width = SrcWidth;

		FwiRect srcROI;
		srcROI.height = SrcHeight;
		srcROI.width = SrcWidth;
		srcROI.x = 0;
		srcROI.y = 0;

		FwiRect dstROI;
		dstROI.height = DstHeight;
		dstROI.width = DstWidth;
		dstROI.x = 0;
		dstROI.y = 0;

		FwStatus status = fwiResizeSqrPixel_8u_C1R(Src, srcRoiSize, SrcWidth, srcROI,
			Dst, DstWidth, dstROI, x_scale, y_scale, 0, 0, interp_mode, Dst);
	}

}



// Convert equal-sized YUV420 frame into BGR24 frame
void ColorConvert_YUV420_BGR24(AVFrame *Src, AVFrame *Dst, int width, int height) {
	// To allow inversion, this has to be done scanline-by-scanline:

	uint8_t *dst = NULL;

	uint8_t *Y = Src->data[0];
	uint8_t *u = Src->data[1];
	uint8_t *v = Src->data[2];

	int r,g,b;
	int C,D,E;

	for (int y = 0; y < height; y++) {
		dst = Dst->data[0] + (Dst->linesize[0] * y); // if you've got a negative linesize, this will flip.
		u = Src->data[1] + (Src->linesize[1] * (y/2));
		v = Src->data[2] + (Src->linesize[2] * (y/2));
		Y = Src->data[0] + (Src->linesize[0] * y);
		int uvincr = 0;

		int x = 0;
		for (; x < width; x++) {
			C = ((int)(*Y) - 16) * 298;
			D = (int)(*u) - 128;
			E = (int)(*v) - 128;

			r = ( C           + 409 * E + 128) >> 8;
			r = clip(r);
			g = ( C - 100 * D - 208 * E + 128) >> 8;
			g = clip(g);
			b = ( C + 516 * D           + 128) >> 8;
			b = clip(b);

			*(dst++) = (uint8_t)b;
			*(dst++) = (uint8_t)g;
			*(dst++) = (uint8_t)r;

			// Increment source:
			Y++;
			u += uvincr;
			v += uvincr;
			uvincr = 1 - uvincr;
		}
	}
}
// Convert equal-sized single-plane frame into another single-plane frame (with scanline re-ordering)
void ColorConvert_Reorder_Interleaved(AVFrame *Src, AVFrame *Dst, int width, int height) {
	// To allow inversion, this has to be done scanline-by-scanline:

	uint8_t *dst = NULL;
	uint8_t *src = Src->data[0];

	for (int y = 0; y < height; y++) {
		dst = Dst->data[0] + (Dst->linesize[0] * y); // if you've got a negative linesize, this will flip.
		for (int x = 0; x < width; x++) {
			*(dst++) = *(src++);
			*(dst++) = *(src++);
			*(dst++) = *(src++);
		}
	}
}

// More generic, off-line conversion:
int ResampleBuffer(AVFrame *Src, PixelFormat SrcFmt,
				   AVFrame *Dst, PixelFormat DstFmt,
				   int width, int height) // doesn't handle scaling! Use matching buffers!
{
	// Work out the formats, and ship off to the right function:
	if (SrcFmt == PixelFormat::PIX_FMT_BGR24
		&& DstFmt == PixelFormat::PIX_FMT_BGR24) {
			ColorConvert_Reorder_Interleaved(Src, Dst, width, height);
			return 0;
	}

	if (SrcFmt == PixelFormat::PIX_FMT_YUV420P
		&& DstFmt == PixelFormat::PIX_FMT_BGR24) {
			ColorConvert_YUV420_BGR24(Src, Dst, width, height);
			return 0;
	}

	printf("Video format unknown\n");
	return -1; // not supported
}