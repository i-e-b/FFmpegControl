// MpegTS_ChunkEncoder.h : main header file for the MpegTS_ChunkEncoder DLL
//

#include "stdafx.h"
#include <fstream>

extern "C" {  
#include "libavformat/avformat.h"
}

#include "stdint.h"
#pragma once

#define DLL  __declspec(dllexport)   // export DLL information 
extern "C" {
	/// MpegTS writer. Implemented in PES_TS.cpp:
	class Pests {
		std::ofstream PushingStream;
		int vCont, aCont, ipcr;

		int WriteTransportHeader(int PID, int Start, int Adapt);
		int WriteStreamHeader(uint8_t StreamType,  int DataLength, long Timestamp); // time is 90kHz clock; returns length of head written.
		void WritePAT();
		void WriteServicePacket();
		void WritePMT(int *TrackIds, uint8_t *TrackTypes, int TrackCount);

	public:

		// These are for 'TrackTypes' fields
		static const uint8_t TT_Ignore = 0xFF;
		static const uint8_t TT_MpegVideo = 0x01; // one of several possible
		static const uint8_t TT_H264 = 0x1B;
		static const uint8_t TT_VC1 = 0xEA;
		static const uint8_t TT_AC3 = 0x81; // one of several possible
		static const uint8_t TT_MpegAudio = 0x03; // for mp3, one of several possible
		static const uint8_t TT_AacGuess = 0x50; //(might be 0x50?) for AAC, a guess!

		Pests();

		void StartFile(char *FilePath, int *TrackIds, uint8_t *TrackTypes, int TrackCount) ;
		void PushStream(int TrackId, uint8_t StreamType, uint8_t* data, int Length, long Timestamp); // time is 90kHz clock;
		void CloseFile();
		int  CanPush();
	};

	/// Structure for managing and marshalling encoding from raw buffers to encoded media
	typedef struct EncoderJob {
		int IsValid;				// true if the job was created properly.
		char BaseDirectory[1024];

		int32_t FrameRate;			// Frames per second (usually 15 - 30)
		uint64_t FrameCount;		// Number of video frames encoded
		int32_t Bitrate;			// Video Bitrate

		int32_t Width, Height;		// frame size

		double SegmentDuration;		// Seconds
		int32_t SegmentNumber, OldSegmentNumber;
		// 1-based number of segments written. Available segments is (SegmentNumber - 1)

		AVOutputFormat *fmt;
		AVFormatContext *oc;
		AVStream *audio_st;
		AVStream *video_st;

		// Internal buffers. Don't touch!!
		uint8_t *audio_outbuf;
		AVFrame *picture;
		uint8_t *video_outbuf;
		int audio_outbuf_size;
		int video_outbuf_size;
		uint64_t SplitNextKey;
		uint64_t a_pts, v_pts;
		Pests *p; // my own MP2TS writer.
	} EncoderJob;

	/// Structure for managing and marshalling encoding from encoded media files to raw buffers
	typedef struct DecoderJob {
		int IsValid;				// true if the job was created properly.

		int32_t     videoWidth, videoHeight;
		double		Framerate;

		int32_t		AudioSampleRate;
		int32_t		AudioChannels;
		int32_t		MinimumAudioBufferSize;

		uint64_t	FrameCount, SampleCount; // for faking timecodes.

		AVFormatContext *pFormatCtx;
		int             videoStream, audioStream;
		AVCodecContext  *pCodecCtx;
		AVCodec         *pCodec;
		AVFrame         *pFrame;

		AVCodecContext  *aCodecCtx;
		AVCodec         *aCodec;
	} DecoderJob;

	/// Structure for passing frame information
	typedef struct MediaFrame {
		uint64_t VideoSize; // number of bytes of Y plane (u & v will be VideoSize / 4)
		unsigned char *Yplane;
		unsigned char *Uplane, *Vplane;
		double VideoSampleTime;

		uint64_t AudioSamplesConsumed;

		uint64_t AudioSize;
		int16_t *AudioBuffer; // audio samples
		double AudioSampleTime;
		int32_t AudioSampleRate;

		uint64_t DataSize; // number of bytes of non-AV data
		unsigned char *DataStreamData;
		int32_t DataStreamTrack;
		double DataStreamTime;

		uint8_t ForceAudioConsumption; // turns off video/audio sync
	} MediaFrame;

	//-----------------------------------------------------------------------------------
	// DLL functions to export. This is the API (if you need more, write it yourself!)
	//-----------------------------------------------------------------------------------

	/// Returns the size of buffer to pass to 'GetVideoCodecData()'
	int DLL GetVideoCodecDataSize(EncoderJob &jobSpec);
	/// Returns the codec's startup data (other than in frames). JobSpec must be initialised. Buffer must be initialised to at least the size specified by 'GetVideoCodecDataSize()'
	void DLL GetVideoCodecData(EncoderJob &jobSpec, char *buffer);

	// Prepare the encoder for work (starts first chunk file)
	int DLL InitialiseEncoderJob(
		EncoderJob &jobSpec,			// Allocated EncoderJob object to fill
		int Width,
		int Height,
		const char* BaseDirectory,		// Output location in filesystem
		int FrameRate,					// Encode frame rate (must be the same as capture frame rate)
		int Bitrate,					// Video bitrate
		double SegmentDuration			// Target duration for file chunks (might get missed)
		);

	void DLL EncodeFrame(EncoderJob &jobSpec, MediaFrame &frame); // Send a frame of video and audio to the encoder

	void DLL CloseEncoderJob(EncoderJob &jobSpec); // Close a previously opened encoder job.

	// Newer rescale stuff for MBR:
	
/// Directly convert an RGB24/8 buffer into a set of equally sized integer-per-sample YUV444 buffers
void DLL Rgb2YuvIS(int width, int height, uint8_t *RgbSrc, uint8_t *Y, uint8_t *U, uint8_t *V);

/// Rescale an image plane. Output size should be equal to input, but only lower bits will be populated.
void DLL PlanarScale(uint8_t *Src, uint8_t *Dst, int SrcWidth, int SrcHeight, int DstWidth, int DstHeight, bool HighQuality);

/// Rescale an interleaved image. Src and Dst buffers should be equal sized.
void DLL InterleavedScale(uint8_t *Src, uint8_t *Dst, int SrcWidth, int SrcHeight, int DstWidth, int DstHeight, bool HighQuality);


	// Prepare a decoder for an existing media file
	int DLL InitialiseDecoderJob(
		DecoderJob &jobSpec,
		const char* Filepath);

	// Read a frame from a decode job. Returns non-zero when complete.
	// Loads an RGB24 image into frame.Yplane
	int DLL DecodeFrame(DecoderJob &jobSpec, MediaFrame &frame);

	void DLL CloseDecoderJob(DecoderJob &jobSpec); // Close a previously opened decoder job.

	// More generic, off-line conversion:
	int ResampleBuffer(AVFrame *Src, PixelFormat SrcFmt,
		AVFrame *Dst, PixelFormat DstFmt,
		int width, int height); // doesn't handle scaling!

	// just to shut the compiler up:
	void advance_fragment(EncoderJob &jobSpec);
};
