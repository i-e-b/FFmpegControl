// MpegTS_ChunkEncoder.cpp : Defines the initialization routines for the DLL.
//

#include "stdafx.h"
#include "MpegTS_ChunkEncoder.h"
#include "FrameWave/fwImage.h"

extern "C" {  
#include "libavformat/avformat.h"
}

#define STREAM_PIX_FMT PIX_FMT_YUV420P

// Which output muxer to use?
#define NEW_M2TS 
#undef OLD_M2TS

static AVStream *add_audio_stream(EncoderJob &jobSpec, AVFormatContext *oc, int codec_id)
{
	AVCodecContext *c;
	AVStream *st;

	if (jobSpec.audio_st == NULL) {
		st = av_new_stream(oc, 1);
		if (!st) {
			fprintf(stderr, "Could not alloc stream\n");
			return NULL;
		}
	} else {
		st = jobSpec.audio_st;
	}


	c = st->codec;
	c->codec_id = (CodecID)codec_id;
	c->codec_type = AVMediaType::AVMEDIA_TYPE_VIDEO;

	/* put sample parameters */
	c->bit_rate = 96000;
	c->flags2 = CODEC_FLAG2_LOCAL_HEADER;
	c->flags = CODEC_FLAG_LOW_DELAY;
	c->channels = 1;  // 1 for MONO, 2 for STEREO
	c->sample_fmt = AVSampleFormat::AV_SAMPLE_FMT_S16;
	c->sample_rate = 44100;

	jobSpec.audio_outbuf_size = (FF_MIN_BUFFER_SIZE * 5);
	jobSpec.audio_outbuf = (uint8_t*)av_malloc(jobSpec.audio_outbuf_size);

	return st;
}

// returns audio frame size (in samples?)
static int open_audio(AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	AVCodec *codec;

	c = st->codec;
	
	oc->flags = AVFMT_FLAG_GENPTS; // Generate timecodes

	/* find the audio encoder */
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec) {
		fprintf(stderr, "codec not found\n");
		return -1;
	}

	/* open it */
	if (avcodec_open(c, codec) < 0) {
		fprintf(stderr, "could not open codec\n");
		return -1;
	}

	return c->frame_size;
}



static AVStream *add_video_stream(AVFormatContext *oc, int codec_id, EncoderJob &jobSpec)
{
	AVCodecContext *c;
	AVStream *st;

	if (jobSpec.video_st == NULL) {
		st = av_new_stream(oc, 0);
		if (!st) {
			fprintf(stderr, "Could not alloc stream\n");
			return NULL;
		}
	} else {
		st = jobSpec.video_st;
	}

	c = st->codec;
	c->codec_id = (CodecID)codec_id;
	c->codec_type = AVMediaType::AVMEDIA_TYPE_VIDEO;

#ifdef OLD_M2TS
	oc->flags = AVFMT_FLAG_GENPTS | AVFMT_TS_DISCONT;
#endif

	c->flags |= CODEC_FLAG_CLOSED_GOP | CODEC_FLAG_QPEL; // Don't allow references to cross GOP boundaries, Allow sub-pixel motion estimation
	c->me_method = ME_HEX; // Hex isn't great, but is easy for the encoder. For smallest output on powerful encoders, use 'ME_FULL'
	c->me_range = 16; // this is 4*number of pixels, or 0 for full search (off-line only)
	c->trellis = 0;
	c->refs = 1;
	c->coder_type = 0;		// Baseline profile (Compatible with mobile devices)
	//c->coder_type = 1;	// Main profile (Dedicated players & PCs)

	c->b_frame_strategy = 0;
	c->me_subpel_quality = 6;

	c->flags2 = CODEC_FLAG2_LOCAL_HEADER | CODEC_FLAG2_STRICT_GOP; // Insert headers all over the place, Force GOP boundaries to be regular.

	int keyrate = (jobSpec.FrameRate * jobSpec.SegmentDuration); // about 1 key per chunk, minimum.
	c->keyint_min = keyrate;
	c->gop_size = keyrate;

	c->scenechange_threshold = 40;
	c->i_quant_factor = 0.4;
	c->qmax = 30;
	c->max_qdiff = 8;
	c->qblur = 0.5;

	// For smooth-streaming compatibility, it's best to leave this as '0'
	c->max_b_frames = 0; // if coder type is '1' above, you may set this to 1 or 2

	// The x264 library seems to have massive problems with VBV buffers, and this makes
	// lot of noise on the console. I'm nore sure what the settings should be, as the
	// documentation is pretty much non-existent.
	bool use_vbr = false; // Use variable bit rate?
	int bit_rate = jobSpec.Bitrate;
	//c->rc_eq = "blurCplx^(1-qComp)";

	if (use_vbr) {
		c->bit_rate_tolerance = bit_rate / 4;
		int vbv_buf = 500; // milliseconds?
		c->rc_buffer_size = vbv_buf;
		c->rc_min_vbv_overflow_use = 3;
		c->rc_max_available_vbv_use = vbv_buf;
		c->rc_max_rate = bit_rate;
		c->rc_min_rate = bit_rate / 4;
	} else {
		c->bit_rate = bit_rate;
		c->rc_max_rate = bit_rate;
		c->rc_min_rate = bit_rate;
	}

	// Set other quality settings based on bit rate.
	if (bit_rate >= 1500000) {
		c->compression_level = 1;
		c->qcompress = 0.1;
		c->qmin = 2; // two is the reasonable minimum. 1 is near-lossless.
	} else if (bit_rate >= 1000000) {
		c->compression_level = 1;
		c->qcompress = 0.1;
		c->qmin = 2;
	} else if (bit_rate >= 750000) {
		c->compression_level = 8;
		c->qcompress = 0.2;
		c->qmin = 3;
	} else if (bit_rate >= 500000) {
		c->compression_level = 32;
		c->qcompress = 0.2;
		c->qmin = 5;
	} else if (bit_rate >= 250000) {
		c->compression_level = 64;
		c->qcompress = 0.4;
		c->qmin = 8;
	} else if (bit_rate >= 90000) {
		c->compression_level = 128;
		c->qcompress = 0.8;
		c->qmin = 10;
	} else {
		c->compression_level = 255;
		c->qcompress = 0.9;
		c->qmin = 15;
	}

	/* resolution must be a multiple of two */
	c->width = jobSpec.Width;
	c->height = jobSpec.Height;
	c->time_base.den = jobSpec.FrameRate;
	c->time_base.num = 1;
	c->ticks_per_frame = 2; // div by two for H.264
	c->pix_fmt = STREAM_PIX_FMT;
	return st;
}

static AVFrame *alloc_picture(PixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	uint8_t *picture_buf;
	int size;

	picture = avcodec_alloc_frame();
	if (!picture) return NULL;
	size = avpicture_get_size(pix_fmt, width, height);
	picture_buf = (uint8_t*)av_malloc(size);
	if (!picture_buf) {
		av_free(picture);
		return NULL;
	}
	avpicture_fill((AVPicture *)picture, picture_buf,
		pix_fmt, width, height);
	return picture;
}

static int open_video(EncoderJob &jobSpec, AVFormatContext *oc, AVStream *st)
{
	AVCodec *codec;
	AVCodecContext *c;

	c = st->codec;

	/* find the video encoder */
	codec = avcodec_find_encoder(c->codec_id);
	if (!codec) {
		fprintf(stderr, "codec not found\n");
		return -1;
	}

	/* open the codec */
	if (avcodec_open(c, codec) < 0) {
		fprintf(stderr, "could not open codec\n");
		return -1;
	}

	jobSpec.video_outbuf = NULL;
	if (!(oc->oformat->flags & AVFMT_RAWPICTURE)) {
		jobSpec.video_outbuf_size = jobSpec.Width * jobSpec.Height * 64; // way bigger than it needs to be
		jobSpec.video_outbuf = (uint8_t*)av_malloc(jobSpec.video_outbuf_size);
	}

	// allocate the encoded raw picture
	jobSpec.picture = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!jobSpec.picture) {
		fprintf(stderr, "Could not allocate picture\n");
		return -1;
	}
	jobSpec.picture->data[3] = jobSpec.picture->data[0]; // hide the reference so we can correctly free(). Nasty hack!
	
	return 0;
}


static void close_audio(EncoderJob &jobSpec, AVFormatContext *oc, AVStream *st)
{
	if (jobSpec.audio_outbuf) {
		void *temp = jobSpec.audio_outbuf;
		jobSpec.audio_outbuf = 0;
		av_free(temp);
	}
	if (jobSpec.audio_st) {
		avcodec_close(jobSpec.audio_st->codec);
		//av_free(jobSpec.audio_st);
		jobSpec.audio_st = 0;
	}
}

static void close_video(EncoderJob &jobSpec, AVFormatContext *oc, AVStream *st)
{
	if (jobSpec.picture) {
		jobSpec.picture->data[0] = jobSpec.picture->data[1] = jobSpec.picture->data[2] = 0; // hide possibly loaded frames
		jobSpec.picture->data[0] = jobSpec.picture->data[3]; // this is really to root element data[0], stored here from open_video()
		av_free(jobSpec.picture);
		jobSpec.picture = 0;
	}
	if (jobSpec.video_outbuf) {
		void *temp = jobSpec.video_outbuf;
		jobSpec.video_outbuf = 0;
		av_free(temp);
	}
	if (jobSpec.video_st) {
		avcodec_close(jobSpec.video_st->codec);
		//av_free(jobSpec.video_st);
		jobSpec.video_st = 0;
	}
}

bool should_advance(EncoderJob &jobSpec) {
	uint64_t head1 = jobSpec.a_pts; // should only be used when audio only, so don't do frame advancing.
	uint64_t head2 = jobSpec.v_pts + (90000 / jobSpec.FrameRate);
	uint64_t head = (head1 > head2) ? (head1) : (head2);
	uint64_t split = jobSpec.SplitNextKey + (jobSpec.SegmentDuration * 90000);
	return (head >= split);
}


static void write_audio_frame(EncoderJob &jobSpec, MediaFrame &frame, AVFormatContext *oc, AVStream *st)
{
	AVCodecContext *c;
	c = st->codec;

	double fs = c->frame_size;
	long as = frame.AudioSize;
	long runs = (as / fs);

	double pts_val = frame.AudioSampleTime * 90000.0; // mpeg ticks at start

	double _asr = frame.AudioSampleRate;
	long pts_incr = (fs / _asr) * 90000.0; // mpeg ticks per encoder frame.

	frame.AudioSamplesConsumed = 0;

	int bft = 0;
	while (
		((pts_val <= jobSpec.v_pts) || frame.ForceAudioConsumption)// (don't overtake video) unless forced
		&& (bft < runs)) { // (don't overrun buffer)
			int outsize = avcodec_encode_audio(c, jobSpec.audio_outbuf, fs, frame.AudioBuffer); // context, byte buffer, sample count, sample buffer

			AVPacket pkt;
			av_init_packet(&pkt);
			
			pkt.pts = pts_val;
			pkt.dts = pkt.pts;
			jobSpec.a_pts = pts_val;
			pkt.flags |= AV_PKT_FLAG_KEY;
			pts_val += pts_incr;

			pkt.stream_index= st->index;
			pkt.data = jobSpec.audio_outbuf;
			pkt.size = outsize;

			// write the compressed frame in the media file
			if (outsize > 0) {
				if (frame.ForceAudioConsumption) {
					advance_fragment(jobSpec); // advance if needed.
				}
#ifdef NEW_M2TS
				jobSpec.p->PushStream(121, Pests::TT_MpegAudio, pkt.data, pkt.size, pkt.pts);
#else
				av_interleaved_write_frame(oc, &pkt);
				put_flush_packet(jobSpec.oc->pb);
#endif
			}
			av_free_packet(&pkt);

			bft++;
			
			frame.AudioBuffer += (int)fs; // update positions
			frame.AudioSamplesConsumed += fs;
	}

}


static void BufferToPicture(EncoderJob &jobSpec, MediaFrame &frame) {
	// Link up passed frames (thus, they must be correct!)
	jobSpec.picture->data[0] = frame.Yplane;
	jobSpec.picture->data[1] = frame.Uplane;
	jobSpec.picture->data[2] = frame.Vplane;
}

static void encode_video_frame(EncoderJob &jobSpec, MediaFrame &frame) {
	int out_size;
	AVCodecContext *c;

	AVStream *st = jobSpec.video_st;
	c = st->codec;
	AVPacket pkt;
	int64_t pts = frame.VideoSampleTime * 90000;

	// MediaFrame should be YUV 4:2:0 already.
	BufferToPicture(jobSpec, frame);
	jobSpec.picture->pts = pts;

	// try to hint keyframes:
	if (should_advance(jobSpec)) { // if we want to split the file,
		jobSpec.picture->pict_type = AVPictureType::AV_PICTURE_TYPE_I; // signal that we want a keyframe next.
	}

	// encode the image
	out_size = avcodec_encode_video(c, jobSpec.video_outbuf, jobSpec.video_outbuf_size, jobSpec.picture);
	av_init_packet(&pkt);

	pkt.pts = pts;
	pkt.dts = pts;

	if(c->coded_frame->key_frame) {
		pkt.flags |= AV_PKT_FLAG_KEY;
		advance_fragment(jobSpec); // start new file if needed, so it will start with a key-frame
	}

	jobSpec.v_pts = pts;
	pkt.stream_index = st->index;
	pkt.data= jobSpec.video_outbuf;
	pkt.size= out_size;

	if (out_size > 0) {
#ifdef NEW_M2TS
		jobSpec.p->PushStream(120, Pests::TT_H264, pkt.data, pkt.size, pkt.pts);
#else
		int ret = av_interleaved_write_frame(jobSpec.oc, &pkt);
		put_flush_packet(jobSpec.oc->pb);
#endif
	}

	av_free_packet(&pkt);
}

void shut_down(EncoderJob &jobSpec) {
	/* write the trailer, if any.  the trailer must be written
	* before you close the CodecContexts open when you wrote the
	* header; otherwise write_trailer may try to use memory that
	* was freed on av_codec_close() */

#ifdef NEW_M2TS
	// nothing
	jobSpec.p->CloseFile();
#else 
	av_write_trailer(jobSpec.oc);
#endif

	/* close each codec */ // one of these (or both) are failing.
	if (jobSpec.video_st && jobSpec.video_outbuf) close_video(jobSpec, jobSpec.oc, jobSpec.video_st);
	if (jobSpec.audio_st && jobSpec.audio_outbuf) close_audio(jobSpec, jobSpec.oc, jobSpec.audio_st);

	/* free the streams */
	for(unsigned int i = 0; i < jobSpec.oc->nb_streams; i++) {
		av_freep(&jobSpec.oc->streams[i]->codec);
		av_freep(&jobSpec.oc->streams[i]);
	}


#ifdef NEW_M2TS
	// nothing
#else 
	if (!(jobSpec.fmt->flags & AVFMT_NOFILE)) {
		// close the output file
		url_fclose(jobSpec.oc->pb);
	}
#endif

	// free the stream
	av_free(jobSpec.oc);
	delete jobSpec.p;
}

int start_up(EncoderJob &jobSpec) {

	jobSpec.p = new Pests();

	jobSpec.oc = avformat_alloc_context();
	if (!jobSpec.oc) {
		fprintf(stderr, "Memory error\n");
		jobSpec.IsValid = false;
		return 3;
	}
	jobSpec.oc->oformat = jobSpec.fmt;
	sprintf(jobSpec.oc->filename, "%s-%05u.ts", jobSpec.BaseDirectory, jobSpec.SegmentNumber);


	// Set video codecs:
	jobSpec.fmt->video_codec = CODEC_ID_H264; // Video codec. Requires FFmpeg to be built with libx264.
	jobSpec.fmt->audio_codec = CODEC_ID_MP3; //CODEC_ID_AAC; // AAC is not working so well. Will use MP3 instead.

	jobSpec.video_st = NULL;
	jobSpec.audio_st = NULL;
	if (jobSpec.fmt->video_codec != CODEC_ID_NONE) {
		jobSpec.video_st = add_video_stream(jobSpec.oc, jobSpec.fmt->video_codec, jobSpec);
	}
	if (jobSpec.fmt->audio_codec != CODEC_ID_NONE) {
		jobSpec.audio_st = add_audio_stream(jobSpec, jobSpec.oc, jobSpec.fmt->audio_codec);
	}

	/*if (av_set_parameters(jobSpec.oc, NULL) < 0) {
		fprintf(stderr, "Invalid output format parameters\n");
			jobSpec.IsValid = false;
			return 4;
	}*/

	/* now that all the parameters are set, we can open the audio and
	video codecs and allocate the necessary encode buffers */
	if (jobSpec.video_st) {
		open_video(jobSpec, jobSpec.oc, jobSpec.video_st);
	}
	if (jobSpec.audio_st) {
		open_audio(jobSpec.oc, jobSpec.audio_st);
	}

#ifdef NEW_M2TS
	jobSpec.fmt->flags |= AVFMT_NOFILE; // we'll write our own, thanks!
	int track_ids[2] = {120, 121};
	uint8_t track_types[2] = {Pests::TT_H264, Pests::TT_MpegAudio};
	jobSpec.p->StartFile(jobSpec.oc->filename, track_ids, track_types, 2); // 120 = video, 121 = audio
#else
	// open the output file, if needed
	if (!(jobSpec.fmt->flags & AVFMT_NOFILE)) {
		if (url_fopen(&jobSpec.oc->pb, jobSpec.oc->filename, URL_WRONLY) < 0) {
			fprintf(stderr, "Could not open '%s'\n", jobSpec.oc->filename);
			jobSpec.IsValid = false;
			return 5;
		}
		av_write_header(jobSpec.oc);
	}
#endif


	// All done OK, validate and return.
	// From this point on, the developer MUST call CloseEncoderJob() before exiting.
	jobSpec.IsValid = true;
	return 0;
}

/// Move on to next fragment IF NEEDED (changes file)
/// This method won't change file if it's not ready to
void advance_fragment(EncoderJob &jobSpec) {
	// Check to see if this frame should be split.
	if (should_advance(jobSpec)) {
		jobSpec.SplitNextKey = (jobSpec.a_pts > jobSpec.v_pts) ? (jobSpec.a_pts) : (jobSpec.v_pts);
		jobSpec.SegmentNumber++;

#ifdef NEW_M2TS
		jobSpec.p->CloseFile();
		sprintf(jobSpec.oc->filename, "%s-%05u.ts", jobSpec.BaseDirectory, jobSpec.SegmentNumber);
		int track_ids[2] = {120, 121};
		uint8_t track_types[2] = {Pests::TT_H264, Pests::TT_MpegAudio};
		jobSpec.p->StartFile(jobSpec.oc->filename, track_ids, track_types, 2);

#else
		url_fclose(jobSpec.oc->pb);
		sprintf(jobSpec.oc->filename, "%s-%05u.ts", jobSpec.BaseDirectory, jobSpec.SegmentNumber);
		if (url_fopen(&jobSpec.oc->pb, jobSpec.oc->filename, URL_WRONLY) < 0) {
			fprintf(stderr, "Could not open '%s'\n", jobSpec.oc->filename);
			jobSpec.IsValid = false;
			return;
		}
		av_write_header(jobSpec.oc);

#endif

	}
}

// Returns the size of buffer to pass to 'GetVideoCodecData()'
int DLL GetVideoCodecDataSize(EncoderJob &jobSpec) {
	for(unsigned int i = 0; i < jobSpec.oc->nb_streams; i++) {
		if(jobSpec.oc->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO) {
			return jobSpec.oc->streams[i]->codec->extradata_size;
		}
	}
	return -1;
}

// Buffer must be initialised to at least the size specified by 'GetVideoCodecDataSize()'
void DLL GetVideoCodecData(EncoderJob &jobSpec, char *buffer) {
	for(unsigned int i = 0; i < jobSpec.oc->nb_streams; i++) {
		if(jobSpec.oc->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO) {
			memcpy(buffer, jobSpec.oc->streams[i]->codec->extradata, jobSpec.oc->streams[i]->codec->extradata_size);
		}
	}
}

int DLL InitialiseEncoderJob(
							 EncoderJob &jobSpec,			// Allocated EncoderJob object to fill
							 int Width,
							 int Height,
							 const char* BaseDirectory,		// Output location in filesystem
							 int FrameRate,					// Encode frame rate (must be the same as capture frame rate)
							 int Bitrate,					// Video bitrate
							 double SegmentDuration			// Target duration for file chunks (might get missed)
							 )
{

	fwStaticInit(); // start up FrameWave library.

	jobSpec.IsValid = false;
	int l = strlen(BaseDirectory);
	if (l >= 1024) return 1;
	strcpy(jobSpec.BaseDirectory, BaseDirectory);

	printf("CPUID: %d\n", fwGetCpuType());

	jobSpec.Width = Width;
	jobSpec.Height = Height;

	jobSpec.SegmentNumber = 1;
	jobSpec.FrameRate = FrameRate;

	int saneBitrate = Bitrate;
	if (saneBitrate > 1900000) saneBitrate = 1900000;
	if (saneBitrate < 60000) saneBitrate = 60000;
	jobSpec.Bitrate = saneBitrate;

	jobSpec.SegmentDuration = SegmentDuration;
	jobSpec.FrameCount = 1;
	jobSpec.SplitNextKey = 0;
	jobSpec.a_pts = 0;
	jobSpec.v_pts = 0;

	// Set-up FFmpeg:
	av_register_all(); // Must be called from 32-bit code.
	jobSpec.fmt = av_guess_format("mpegts", NULL, NULL);
	if (!jobSpec.fmt) {
		fprintf(stderr, "Could not find suitable output format\n");
		return 2;
	}

	return start_up(jobSpec);
}

void push_audio(EncoderJob &jobSpec, MediaFrame &frame) {// Send available frames to the encoder
	if (frame.AudioSize > 0) {
		// This function only writes frames until it reaches sync with the video pts.
		// also, it updates the AudioSamplesConsumed in 'frame'.
		write_audio_frame(jobSpec, frame, jobSpec.oc, jobSpec.audio_st);

		if (frame.ForceAudioConsumption && frame.VideoSize == 0) {
			jobSpec.FrameCount++; // if we're doing audio-only, push frames here.
		}
	}
}


void push_video(EncoderJob &jobSpec, MediaFrame &frame) {// Send available frames to the encoder
	if (frame.VideoSize > 0) {
		// For this to work smoothly, the audio is always kept behind the video; this is handled
		// in the 'write_audio_frame' function, and by writing PTS values back to the jobSpec.
		encode_video_frame(jobSpec, frame);
		jobSpec.FrameCount++;
	}
}

void push_data(EncoderJob &jobSpec, MediaFrame &frame) { // push raw data into the output
	if (frame.DataSize < 1) return; // no data to push
#ifdef NEW_M2TS
	/*
	long pts = frame.DataStreamTime * 90000.0;
	jobSpec.p->PushStream(frame.DataStreamTrack, Pests::TT_MpegAudio, frame.DataStreamData, frame.DataSize, pts);
	*/
#else
	// Not yet supported (how to: make a packet, fill in the blanks and call av_interleaved_write_frame() )
#endif
}

void DLL EncodeFrame(EncoderJob &jobSpec, MediaFrame &frame) { // Send a frame of video and audio to the encoder
	// Check job:
	if (!jobSpec.IsValid) return;

	push_video(jobSpec, frame); // <-- may change fragment here
	push_audio(jobSpec, frame);
	push_data(jobSpec, frame);
}

void DLL CloseEncoderJob(EncoderJob &jobSpec) {
	if (!jobSpec.IsValid) return; // already closed!
	shut_down(jobSpec);
	jobSpec.IsValid = false;
	// DONE!
}


int DLL InitialiseDecoderJob(
							 DecoderJob &jobSpec,
							 const char* Filepath)
{
	jobSpec.IsValid = false;
	av_register_all(); // Must be called from 32-bit code.
	
	if (avformat_open_input(&jobSpec.pFormatCtx, Filepath, NULL, NULL) != 0) return 1; // Couldn't open file
	if (av_find_stream_info(jobSpec.pFormatCtx) < 0) return 2; // couldn't find stream information

	int videoStream = -1;
	int audioStream = -1;
	for(unsigned int i = 0; i < jobSpec.pFormatCtx->nb_streams; i++) {
		if(jobSpec.pFormatCtx->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO && videoStream < 0) {
			videoStream=i;
		}
		if(jobSpec.pFormatCtx->streams[i]->codec->codec_type == AVMediaType::AVMEDIA_TYPE_AUDIO && audioStream < 0) {
			audioStream=i;
		}
	}
	if (videoStream < 0) return 3; // No video stream
	if (audioStream < 0) return 4; // No audio stream

	jobSpec.videoStream = videoStream;
	jobSpec.audioStream = audioStream;

	// Prepare audio decode
	jobSpec.aCodecCtx = jobSpec.pFormatCtx->streams[audioStream]->codec;
	jobSpec.AudioSampleRate = jobSpec.aCodecCtx->sample_rate;
	jobSpec.AudioChannels = jobSpec.aCodecCtx->channels;
	jobSpec.aCodec = avcodec_find_decoder(jobSpec.aCodecCtx->codec_id);
	//jobSpec.aCodecCtx->antialias_algo = FF_AA_FLOAT;
	//jobSpec.aCodecCtx->error_recognition = 0;

	if (!jobSpec.aCodec) return 5; // unsupported codec
	if (avcodec_open(jobSpec.aCodecCtx, jobSpec.aCodec) < 0) return 6;

	// Prepare video decode
	jobSpec.pCodecCtx = jobSpec.pFormatCtx->streams[videoStream]->codec;
	jobSpec.pCodec = avcodec_find_decoder(jobSpec.pCodecCtx->codec_id);
	if (jobSpec.pCodec == NULL) return 7; // Unsupported codec
	if (avcodec_open(jobSpec.pCodecCtx, jobSpec.pCodec) < 0) return 8;

	jobSpec.videoWidth = jobSpec.pCodecCtx->width;
	jobSpec.videoHeight = jobSpec.pCodecCtx->height;

	// Prepare some destination memory
	jobSpec.pFrame = avcodec_alloc_frame();
	AVStream *vst = jobSpec.pFormatCtx->streams[videoStream];

	jobSpec.Framerate = av_q2d(vst->r_frame_rate);

	jobSpec.MinimumAudioBufferSize = AVCODEC_MAX_AUDIO_FRAME_SIZE * 64; // make it far too large to compensate for various bugs...
	jobSpec.IsValid = true;

	return 0;
}


// Decode a raw AV frame into a prepared MediaFrame (buffers must be initialised)
int DLL DecodeFrame(DecoderJob &jobSpec, MediaFrame &frame) {
	AVPacket packet;
	int frameFinished = 0;
	frame.AudioSize = 0;
	frame.VideoSize = 0;

	av_init_packet(&packet);

	if (av_read_frame(jobSpec.pFormatCtx, &packet) < 0 ) {
		
		if (jobSpec.pFormatCtx->pb && jobSpec.pFormatCtx->pb->error) {
			printf("[EOF]");
			av_free_packet(&packet);
			return -1; // end of file
		} else {
			printf("[EOP]");
			av_free_packet(&packet);
			if (av_read_frame(jobSpec.pFormatCtx, &packet) < 0 ) {
				return -1; // no more packets?
			}
		}
	}

	// a few sanity checks
	if (jobSpec.Framerate < 1.0) jobSpec.Framerate = 25.010101;
	if (jobSpec.AudioSampleRate < 8000) jobSpec.AudioSampleRate = 44100;

	AVStream *audio_st = jobSpec.pFormatCtx->streams[jobSpec.audioStream];
	AVStream *video_st = jobSpec.pFormatCtx->streams[jobSpec.videoStream];

	int VideoID = video_st->id;
	int AudioID = audio_st->id;

	//int PID = StreamID;
	int PID = jobSpec.pFormatCtx->streams[packet.stream_index]->id;

	if (PID == VideoID) {
		printf("V[");
		avcodec_decode_video2(jobSpec.pCodecCtx, jobSpec.pFrame, &frameFinished, &packet);

		if (!frameFinished) {
			//printf(".");
			return 0; // don't dispose of frame!
		}

		// We might have any format incoming, so we need to convert to the expected RGB24
		AVFrame pict;

		// Write frame data flipped vertically (to match standard capture devices)
		pict.linesize[0] = jobSpec.videoWidth * 3;
		frame.VideoSize = pict.linesize[0] * jobSpec.videoHeight;
		pict.data[0] = frame.Yplane;

		// replacement for leaky swscale
		// As this is meant for off-line processing, might be OK to have it back?
		ResampleBuffer(jobSpec.pFrame, video_st->codec->pix_fmt, &pict, PIX_FMT_BGR24, jobSpec.videoWidth, jobSpec.videoHeight);

		jobSpec.FrameCount = 1 + jobSpec.pFrame->repeat_pict;
		frame.VideoSampleTime = packet.pts * av_q2d(video_st->time_base);
		printf("]");
	} else if (PID == AudioID) {
		printf("A[");

		int audio_end = jobSpec.MinimumAudioBufferSize;
		int len = 0;
		int total_len = 0;
		uint8_t *buf_ptr = (uint8_t*)frame.AudioBuffer;

		int audio_size = jobSpec.MinimumAudioBufferSize;

		while (packet.size > 0) {
			len = avcodec_decode_audio3(jobSpec.aCodecCtx, (short*)buf_ptr, &audio_size, &packet);

				buf_ptr += audio_size;
				total_len += audio_size / 2;
				packet.size -= audio_size;

			if (len < 1) {
				break; // done.
			}
        }

		frame.AudioSize = (total_len < 0) ? (0) : (total_len);
		frame.AudioSampleTime = packet.pts * av_q2d(audio_st->time_base); // convert timestamp to seconds

		jobSpec.SampleCount = frame.AudioSize;
		printf("]");
	} else {
		printf("_");
		return 1;
	}
	
	return 1;
}

// Close a previously opened decoder job.
void DLL CloseDecoderJob(DecoderJob &jobSpec) {
	if (!jobSpec.IsValid) return;

	av_free(jobSpec.pFrame);// Free the YUV frame
	avcodec_close(jobSpec.pCodecCtx); // Close the codec
	av_close_input_file(jobSpec.pFormatCtx);// Close the video file
}