
#include "stdafx.h"

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
};

#include "opencv2/opencv.hpp"

#ifdef _DEBUG
#pragma comment(lib, "opencv_ts300d.lib")
#pragma comment(lib, "opencv_world300d.lib")
#else
#pragma comment(lib, "opencv_ts300.lib")
#pragma comment(lib, "opencv_world300.lib")
#endif

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "swresample.lib")

#define MAX_AUDIO_FRAME_SIZE 192000		// 1 second of 48KHZ 32bit audio -> 32bit / 8 * 48000 * 1 = 192000 Byte	

#define SAVE_FILE 0

/*
	函数功能：将图片 foreground 叠加到图片 background 上
*/
static void overlayImage(const cv::Mat &background, const cv::Mat &foreground, cv::Mat &output, cv::Point2i location)
{
	background.copyTo(output);

	for (int y = std::max(location.y, 0); y < background.rows; ++y)
	{
		int fY = y - location.y;
		if (fY >= foreground.rows)
			break;

		for (int x = std::max(location.x, 0); x < background.cols; ++x)
		{
			int fX = x - location.x;
			if (fX >= foreground.cols)
				break;

			double opacity = ((double)foreground.data[fY * foreground.step + fX * foreground.channels() + 3]) / 255.;

			for (int c = 0; opacity > 0 && c < output.channels(); ++c)
			{
				unsigned char foregroundPx =
					foreground.data[fY * foreground.step + fX * foreground.channels() + c];
				unsigned char backgroundPx =
					background.data[y * background.step + x * background.channels() + c];

				output.data[y*output.step + output.channels()*x + c] =
					backgroundPx * (1. - opacity) + foregroundPx * opacity;
			}
		}
	}
}

int main(int argc, char* argv[])
{
	AVFormatContext* ifmt_ctx = NULL;
	const char*	input_filepath = NULL;

	AVFormatContext* ofmt_ctx = NULL;
	const char* output_filepath = NULL;

	int idx_audio = -1;
	int idx_video = -1;

	AVStream* istream_audio = NULL;
	AVCodecContext*	codec_ctx_audio = NULL;
	AVCodec* codec_audio = NULL;

	AVStream* istream_video = NULL;
	AVCodecContext*	codec_ctx_video = NULL;
	AVCodec* codec_video = NULL;

	AVStream* ostream_video = NULL;
	AVStream* ostream_audio = NULL;

	AVCodecContext*	codec_ctx_2h264 = NULL;
	AVCodecContext*	codec_ctx_2aac = NULL;

	struct SwsContext* img_convert_ctx_2bgr = NULL;
	struct SwsContext* img_convert_ctx_2yuv = NULL;	

	uint8_t* buffer_bgr = NULL;
	uint8_t* buffer_yuv = NULL;

	struct SwrContext* au_convert_ctx = NULL;
	uint8_t* out_audio_buffer = NULL;

	av_register_all();
	avformat_network_init();

	//input_filepath = "rtmp://live.hkstv.hk.lxdns.com/live/hks";
	input_filepath = "rtsp://admin:q12345678@192.168.1.102:554/MPEG-4/ch1/main/av_stream";

	//output_filepath = "rtmp://127.0.0.1/live/mytest";
	output_filepath = "output.flv";

	AVDictionary* options = NULL;
	av_dict_set(&options, "rtsp_transport", "tcp", 0);
	if (avformat_open_input(&ifmt_ctx, input_filepath, NULL, &options) < 0)
	{
		printf("error: avformat_open_input\n");
		goto END;
	}

	if (avformat_find_stream_info(ifmt_ctx, NULL) < 0)
	{
		printf("error: avformat_find_stream_info\n");
		goto END;
	}

	if (avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", output_filepath) < 0)
	{
		printf("error: avformat_alloc_output_context2\n");
		goto END;
	}

	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		if (AVMEDIA_TYPE_AUDIO == ifmt_ctx->streams[i]->codec->codec_type)
		{
			idx_audio = i;

			istream_audio = ifmt_ctx->streams[idx_audio];
			codec_ctx_audio = istream_audio->codec;

			ostream_audio = avformat_new_stream(ofmt_ctx, istream_audio->codec->codec);
			avcodec_copy_context(ostream_audio->codec, istream_audio->codec);
			ostream_audio->time_base = istream_audio->time_base;
			ostream_audio->codec->codec_tag = 0;
			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
				ostream_audio->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}
		else if (AVMEDIA_TYPE_VIDEO == ifmt_ctx->streams[i]->codec->codec_type)
		{
			idx_video = i;

			istream_video = ifmt_ctx->streams[idx_video];
			codec_ctx_video = istream_video->codec;

			ostream_video = avformat_new_stream(ofmt_ctx, istream_video->codec->codec);
			avcodec_copy_context(ostream_video->codec, istream_video->codec);
			ostream_video->time_base = istream_video->time_base;
			ostream_video->codec->codec_tag = 0;
			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
				ostream_video->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
		}
		else
		{
			;
		}
	}

	if (idx_video < 0 || idx_audio < 0)
	{
		printf("error: can not find any audio stream or video stream\n");
		goto END;
	}

	codec_audio = avcodec_find_decoder(codec_ctx_audio->codec_id);
	if (NULL == codec_audio)
	{
		printf("error: avcodec_find_decoder, input audio\n");
		goto END;
	}

	codec_video = avcodec_find_decoder(codec_ctx_video->codec_id);
	if (NULL == codec_video)
	{
		printf("error: avcodec_find_decoder, input video\n");
		goto END;
	}

	if (avcodec_open2(codec_ctx_audio, codec_audio, NULL) < 0)		// 打开输入音频流的解码器
	{
		printf("error: avcodec_open2, input audio\n");
		goto END;
	}

	if (avcodec_open2(codec_ctx_video, codec_video, NULL) < 0)		// 打开输入视频流的解码器
	{
		printf("error: avcodec_open2, input video\n");
		goto END;
	}

	if (avio_open(&ofmt_ctx->pb, output_filepath, AVIO_FLAG_WRITE) < 0)
	{
		printf("error: avio_open\n");
		goto END;
	}

	if (avformat_write_header(ofmt_ctx, NULL) < 0)
	{
		printf("error: avformat_write_header\n");
		goto END;
	}

	av_dump_format(ifmt_ctx, -1, input_filepath, 0);
	av_dump_format(ofmt_ctx, -1, output_filepath, 1);

	AVFrame  oframe_bgr = { 0 };	
	oframe_bgr.width = codec_ctx_video->width;
	oframe_bgr.height = codec_ctx_video->height;
	oframe_bgr.format = AV_PIX_FMT_BGR24;

	int pic_size_bgr = avpicture_get_size((AVPixelFormat)oframe_bgr.format, oframe_bgr.width, oframe_bgr.height);
	buffer_bgr = (uint8_t*)av_malloc(pic_size_bgr);
	avpicture_fill((AVPicture*)&oframe_bgr, buffer_bgr, (AVPixelFormat)oframe_bgr.format, oframe_bgr.width, oframe_bgr.height);
	img_convert_ctx_2bgr = sws_getContext(codec_ctx_video->width, codec_ctx_video->height, codec_ctx_video->pix_fmt,
		oframe_bgr.width, oframe_bgr.height, (AVPixelFormat)oframe_bgr.format, SWS_BICUBIC, NULL, NULL, NULL);

	AVFrame  oframe_yuv = { 0 };
	oframe_yuv.width = codec_ctx_video->width;
	oframe_yuv.height = codec_ctx_video->height;
	oframe_yuv.format = AV_PIX_FMT_YUV420P;

	int pic_size_yuv = avpicture_get_size((AVPixelFormat)oframe_yuv.format, oframe_yuv.width, oframe_yuv.height);
	buffer_yuv = (uint8_t*)av_malloc(pic_size_yuv);
	avpicture_fill((AVPicture*)&oframe_yuv, buffer_yuv, (AVPixelFormat)oframe_yuv.format, oframe_yuv.width, oframe_yuv.height);
	img_convert_ctx_2yuv = sws_getContext(oframe_bgr.width, oframe_bgr.height, (AVPixelFormat)oframe_bgr.format,
		oframe_yuv.width, oframe_yuv.height, (AVPixelFormat)oframe_yuv.format, SWS_BICUBIC, NULL, NULL, NULL);


	int64_t in_audio_channel_layout = av_get_default_channel_layout(codec_ctx_audio->channels);
	AVSampleFormat in_audio_sample_fmt = codec_ctx_audio->sample_fmt;
	int in_audio_sample_rate = codec_ctx_audio->sample_rate;

	AVSampleFormat out_audio_sample_fmt = AV_SAMPLE_FMT_S16;
	int out_audio_sample_rate = 44100;							// <= 48000 HZ ?
	int out_audio_nb_samples = codec_ctx_audio->frame_size;		// AAC:1024  MP3:1152  
	uint64_t out_audio_channel_layout = AV_CH_LAYOUT_STEREO;
	int out_audio_channels = av_get_channel_layout_nb_channels(out_audio_channel_layout);
	int out_audio_buffer_size = av_samples_get_buffer_size(NULL, out_audio_channels, out_audio_nb_samples, out_audio_sample_fmt, 1);	
	out_audio_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * out_audio_channels);

	au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, out_audio_channel_layout, out_audio_sample_fmt, out_audio_sample_rate,
										in_audio_channel_layout, in_audio_sample_fmt, in_audio_sample_rate, 0, NULL);
	swr_init(au_convert_ctx);

	AVCodec* codec_2h264 = NULL;
	AVCodecID codec_id_2h264;

	codec_id_2h264 = AV_CODEC_ID_H264;
	codec_2h264 = avcodec_find_encoder(codec_id_2h264);
	codec_ctx_2h264 = avcodec_alloc_context3(codec_2h264);

	codec_ctx_2h264->codec_type = AVMEDIA_TYPE_VIDEO;
	codec_ctx_2h264->pix_fmt = AV_PIX_FMT_YUV420P;
	codec_ctx_2h264->bit_rate = codec_ctx_video->bit_rate;			// 0 ？
	codec_ctx_2h264->width = codec_ctx_video->width;
	codec_ctx_2h264->height = codec_ctx_video->height;
	codec_ctx_2h264->time_base = codec_ctx_video->time_base;

	if (avcodec_open2(codec_ctx_2h264, codec_2h264, NULL) < 0)
	{
		printf("error: avcodec_open2, output video\n");
		goto END;
	}

	AVCodec* codec_2aac = NULL;
	AVCodecID codec_id_2aac;

	codec_id_2aac = AV_CODEC_ID_AAC;
	codec_2aac = avcodec_find_encoder(codec_id_2aac);
	codec_ctx_2aac = avcodec_alloc_context3(codec_2aac);

	codec_ctx_2aac->codec_type = AVMEDIA_TYPE_AUDIO;
	codec_ctx_2aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
	codec_ctx_2aac->sample_rate = codec_ctx_audio->sample_rate;
	codec_ctx_2aac->bit_rate = codec_ctx_audio->bit_rate;
	codec_ctx_2aac->channel_layout = codec_ctx_audio->channel_layout;
	codec_ctx_2aac->channels = codec_ctx_audio->channels;
	codec_ctx_2aac->time_base = codec_ctx_audio->time_base;

	if (avcodec_open2(codec_ctx_2aac, codec_2aac, NULL) < 0)
	{
		printf("error: avcodec_open2, output audio\n");
		goto END;
	}

	AVPacket ipkt = { 0 };
	av_init_packet(&ipkt);
	ipkt.data = NULL;
	ipkt.size = 0;

	AVFrame  iframe_audio = { 0 };
	AVFrame  iframe = { 0 };

	AVPacket opkt_audio = { 0 };
	av_init_packet(&opkt_audio);
	opkt_audio.data = NULL;
	opkt_audio.size = 0;

	AVPacket opkt = { 0 };
	av_init_packet(&opkt);
	opkt.data = NULL;
	opkt.size = 0;

#if SAVE_FILE
	FILE* output_bgr = fopen("output.bgr", "wb+");
	FILE* output_yuv = fopen("output.yuv", "wb+");
	FILE* output_h264 = fopen("output.h264", "wb+");
	FILE* output_pcm = fopen("output.pcm", "wb+");
	FILE* output_aac = fopen("output.aac", "wb+");
#endif

	while (1)
	{
		if (av_read_frame(ifmt_ctx, &ipkt) < 0)	
			break;

		//if (0 == ipkt.pts)
		//{
		//	continue;
		//}

		if (ipkt.stream_index == idx_audio)
		{
			avcodec_send_packet(codec_ctx_audio, &ipkt);
			avcodec_receive_frame(codec_ctx_audio, &iframe_audio);

			swr_convert(au_convert_ctx, &out_audio_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t**)iframe_audio.data, iframe_audio.nb_samples);

#if SAVE_FILE
			fwrite(out_audio_buffer, 1, out_audio_buffer_size, output_pcm);
#endif

			iframe_audio.pts = ipkt.pts;
			avcodec_send_frame(codec_ctx_2aac, &iframe_audio);
			avcodec_receive_packet(codec_ctx_2aac, &opkt_audio);

#if SAVE_FILE
			// 生成的AAC文件无法播放。待解！
			fwrite(opkt_audio.data, 1, opkt_audio.size, output_aac); 
#endif

			opkt_audio.pts = av_rescale_q_rnd(ipkt.pts, istream_audio->time_base, ostream_audio->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			opkt_audio.dts = av_rescale_q_rnd(ipkt.dts, istream_audio->time_base, ostream_audio->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			opkt_audio.duration = av_rescale_q(ipkt.duration, istream_audio->time_base, ostream_audio->time_base);			
			opkt_audio.pos = -1;
			opkt_audio.stream_index = idx_audio;

			av_interleaved_write_frame(ofmt_ctx, &opkt_audio);
			av_free_packet(&opkt_audio);

			static int audio_count = 0;
			printf("			audio count = %d\n", ++audio_count);
		}
		else if (ipkt.stream_index == idx_video)
		{
			avcodec_send_packet(codec_ctx_video, &ipkt);
			avcodec_receive_frame(codec_ctx_video, &iframe);

			sws_scale(img_convert_ctx_2bgr, (const uint8_t* const*)iframe.data, iframe.linesize, 0, iframe.height, oframe_bgr.data, oframe_bgr.linesize);

			cv::Mat background(oframe_bgr.height, oframe_bgr.width, CV_8UC3, oframe_bgr.data[0]);
			overlayImage(background, cv::imread("logo.jpg"), background, cv::Point(20, 30));

#if SAVE_FILE		
			fwrite(oframe_bgr.data[0], (oframe_bgr.width) * (oframe_bgr.height) * 3, 1, output_bgr);
#endif

			sws_scale(img_convert_ctx_2yuv, (const uint8_t* const*)oframe_bgr.data, oframe_bgr.linesize, 0, oframe_bgr.height, oframe_yuv.data, oframe_yuv.linesize);

#if SAVE_FILE
			fwrite(oframe_yuv.data[0], (oframe_yuv.width) * (oframe_yuv.height), 1, output_yuv);
			fwrite(oframe_yuv.data[1], (oframe_yuv.width) * (oframe_yuv.height) / 4, 1, output_yuv);
			fwrite(oframe_yuv.data[2], (oframe_yuv.width) * (oframe_yuv.height) / 4, 1, output_yuv);
#endif
			oframe_yuv.pts = ipkt.pts;
			oframe_yuv.width = codec_ctx_2h264->width;
			oframe_yuv.height = codec_ctx_2h264->height;

			avcodec_send_frame(codec_ctx_2h264, &oframe_yuv);
			avcodec_receive_packet(codec_ctx_2h264, &opkt);

#if SAVE_FILE
			fwrite(opkt.data, 1, opkt.size, output_h264);
#endif

			opkt.pts = av_rescale_q_rnd(ipkt.pts, istream_video->time_base, ostream_video->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			opkt.dts = av_rescale_q_rnd(ipkt.dts, istream_video->time_base, ostream_video->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			opkt.duration = av_rescale_q(ipkt.duration, istream_video->time_base, ostream_video->time_base);
			opkt.pos = -1;
			opkt.stream_index = idx_video;

			av_interleaved_write_frame(ofmt_ctx, &opkt);
			av_free_packet(&opkt);

			static int video_count = 0;
			printf("video_count = %d\n", ++video_count);
		}
		else
		{
			;
		}

		av_free_packet(&ipkt);
	}

	if (0 != av_write_trailer(ofmt_ctx))
	{
		printf("error: av_write_trailer\n");
		goto END;
	}

END:

#if FILE
	if (NULL != output_pcm)
		fclose(output_pcm);
	if (NULL != output_aac)
		fclose(output_aac);
	if (NULL != output_bgr)
		fclose(output_bgr);
	if (NULL != output_yuv)
		fclose(output_yuv);
	if (NULL != output_h264)
		fclose(output_h264);
#endif

	avformat_close_input(&ifmt_ctx);

	av_free(out_audio_buffer);
	av_free(buffer_bgr);
	av_free(buffer_yuv);

	swr_free(&au_convert_ctx);
	sws_freeContext(img_convert_ctx_2bgr);
	sws_freeContext(img_convert_ctx_2yuv);
	
	avcodec_free_context(&codec_ctx_2h264);
	avcodec_free_context(&codec_ctx_2aac);

	if (NULL != ofmt_ctx)
		avio_close(ofmt_ctx->pb);

	avformat_free_context(ofmt_ctx);

    return 0;
}