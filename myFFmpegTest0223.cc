// myFFmpegTest0223.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"

extern "C"
{
//#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
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

/* 雷霄骅
FIX: H.264 in some container format (FLV, MP4, MKV etc.) need "h264_mp4toannexb" bitstream filter (BSF)
	* Add SPS,PPS in front of IDR frame
	* Add start code ("0,0,0,1") in front of NALU
H.264 in some container (MPEG2TS) don't need this BSF.
*/

//'1': Use H.264 Bitstream Filter 
#define USE_H264BSF 1				// 

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
	av_register_all();
	avformat_network_init();
	
	// 几个变量
	AVFormatContext* ifmt_ctx = NULL;			// Format context of input stream
	const char*	input_filepath = NULL;			// Path of input stream

	AVFormatContext* ofmt_ctx = NULL;			// Format context of output stream
	const char* output_filepath = NULL;			// Path of output stream

	FILE* output_bgr = NULL;					// 存放 BGR 格式数据的本地文件
	FILE* output_yuv = NULL;					// 存放 YUV 格式数据的本地文件
	FILE* output_h264 = NULL;					// 存放 H264 格式数据的本地文件

	uint8_t* buffer_bgr = NULL;					// 存放 BGR 格式原始数据的缓冲区
	uint8_t* buffer_yuv = NULL;					// 存放 YUV 格式原始数据的缓冲区

	struct SwsContext* img_convert_ctx_2bgr = NULL;		// Original format --> BGR format
	struct SwsContext* img_convert_ctx_2yuv = NULL;		// BGR format --> YUV format

	/* 输入流 */

	input_filepath = "rtmp://live.hkstv.hk.lxdns.com/live/hks";

	if (avformat_open_input(&ifmt_ctx, input_filepath, NULL, NULL) < 0)
	{
		printf("error: avformat_open_input\n");
		goto END;
	}

	if (avformat_find_stream_info(ifmt_ctx, NULL) < 0)
	{
		printf("error: avformat_find_stream_info\n");
		goto END;
	}

	int idx_audio = -1;							// index of input audio stream
	int idx_video = -1;							// index of input video stream

	for (int i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		if (AVMEDIA_TYPE_AUDIO == ifmt_ctx->streams[i]->codec->codec_type)
		{
			idx_audio = i;
		}
		else if (AVMEDIA_TYPE_VIDEO == ifmt_ctx->streams[i]->codec->codec_type)
		{
			idx_video = i;
		}
		else
		{
			break;
		}
	}

	if (idx_video < 0 || idx_audio < 0)
	{
		printf("error: can not find any audio stream or video stream\n");
		goto END;
	}

	AVStream* istream_audio = NULL;				// Stream structure of input audio stream
	AVCodecContext*	codec_ctx_audio = NULL;		// main external API structure of input audio stream
	AVCodec* codec_audio = NULL;				// AVCodec structure of input audio stream

	AVStream* istream_video = NULL;				// ... of input video stream
	AVCodecContext*	codec_ctx_video = NULL;		// 
	AVCodec* codec_video = NULL;				//

	istream_audio = ifmt_ctx->streams[idx_audio];
	codec_ctx_audio = istream_audio->codec;

	codec_audio = avcodec_find_decoder(codec_ctx_audio->codec_id);
	if (NULL == codec_audio)
	{
		printf("error: avcodec_find_decoder, input audio\n");
		goto END;
	}

	istream_video = ifmt_ctx->streams[idx_video];
	codec_ctx_video = istream_video->codec;

	codec_video = avcodec_find_decoder(codec_ctx_video->codec_id);
	if (NULL == codec_video)
	{
		printf("error: avcodec_find_decoder, input video\n");
		goto END;
	}

	if (avcodec_open2(codec_ctx_audio, codec_audio, NULL) < 0)
	{
		printf("error: avcodec_open2, input audio\n");
		goto END;
	}

	if (avcodec_open2(codec_ctx_video, codec_video, NULL) < 0)
	{
		printf("error: avcodec_open2, input video\n");
		goto END;
	}

	int picture_width = codec_ctx_video->width;
	int picture_height = codec_ctx_video->height;
	AVPixelFormat pixel_format = codec_ctx_video->pix_fmt;

	/* 输出流 */

	output_bgr = fopen("output.bgr", "wb+");
	output_yuv = fopen("output.yuv", "wb+");
	output_h264 = fopen("output.h264", "wb+");

	//output_filepath = "rtmp://127.0.0.1/live/mytest";
	output_filepath = "output.flv";

	if (avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", output_filepath) < 0)
	{
		printf("error: avformat_alloc_output_context2\n");
		goto END;
	}

	AVStream* ostream_audio = NULL;
	AVStream* ostream_video = NULL;

	ostream_audio = avformat_new_stream(ofmt_ctx, NULL);				// 输出音频流
	avcodec_copy_context(ostream_audio->codec, istream_audio->codec);
	ostream_audio->time_base = istream_audio->time_base;

	ostream_video = avformat_new_stream(ofmt_ctx, NULL);				// 输出视频流
	avcodec_copy_context(ostream_video->codec, istream_video->codec);
	ostream_video->time_base = istream_video->time_base;

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


	AVPacket ipkt = { 0 };						// 存放输入流解封装得到的编码数据
	av_init_packet(&ipkt);
	ipkt.data = NULL;
	ipkt.size = 0;

	AVFrame  iframe = { 0 };					// 存放输入视频流解码后得到的原始数据

	AVFrame  oframe_bgr = { 0 };				// 存放转码后的原始数据（原格式 -> BGR格式）
	int pic_size_bgr = -1;
	
	oframe_bgr.width = picture_width;
	oframe_bgr.height = picture_height;
	oframe_bgr.format = AV_PIX_FMT_BGR24;

	pic_size_bgr = avpicture_get_size((AVPixelFormat)oframe_bgr.format, oframe_bgr.width, oframe_bgr.height);
	buffer_bgr = (uint8_t*)av_malloc(pic_size_bgr);
	avpicture_fill((AVPicture*)&oframe_bgr, buffer_bgr, (AVPixelFormat)oframe_bgr.format, oframe_bgr.width, oframe_bgr.height);
	img_convert_ctx_2bgr = sws_getContext(picture_width, picture_height, pixel_format,
		oframe_bgr.width, oframe_bgr.height, (AVPixelFormat)oframe_bgr.format, SWS_BICUBIC, NULL, NULL, NULL);

	AVFrame  oframe_yuv = { 0 };				// 存放再次转码后的原始数据（BGR -> YUV）
	int pic_size_yuv = -1;

	oframe_yuv.width = picture_width;
	oframe_yuv.height = picture_height;
	oframe_yuv.format = AV_PIX_FMT_YUV420P;

	pic_size_yuv = avpicture_get_size((AVPixelFormat)oframe_yuv.format, oframe_yuv.width, oframe_yuv.height);
	buffer_yuv = (uint8_t*)av_malloc(pic_size_yuv);
	avpicture_fill((AVPicture*)&oframe_yuv, buffer_yuv, (AVPixelFormat)oframe_yuv.format, oframe_yuv.width, oframe_yuv.height);
	img_convert_ctx_2yuv = sws_getContext(oframe_bgr.width, oframe_bgr.height, (AVPixelFormat)oframe_bgr.format,
		oframe_yuv.width, oframe_yuv.height, (AVPixelFormat)oframe_yuv.format, SWS_BICUBIC, NULL, NULL, NULL);

	// 用于编码：YUV -> H264
	AVCodecContext*	codec_ctx_2h264 = NULL;
	AVCodec* codec_2h264 = NULL;
	AVCodecID codec_id_2h264;

	codec_id_2h264 = AV_CODEC_ID_H264;
	codec_2h264 = avcodec_find_encoder(codec_id_2h264);
	codec_ctx_2h264 = avcodec_alloc_context3(codec_2h264);

	codec_ctx_2h264->bit_rate = 400000; //codec_ctx_video->bit_rate;			// ？
	codec_ctx_2h264->width = picture_width;
	codec_ctx_2h264->height = picture_height;
	codec_ctx_2h264->time_base = codec_ctx_video->time_base;
	codec_ctx_2h264->pix_fmt = AV_PIX_FMT_YUV420P;
	codec_ctx_2h264->codec_type = AVMEDIA_TYPE_VIDEO;

	if (avcodec_open2(codec_ctx_2h264, codec_2h264, NULL) < 0)
	{
		printf("error: avcodec_open2, output video\n");
		goto END;
	}

	AVPacket opkt = { 0 };					// 存放编码后的视频数据
	av_init_packet(&opkt);
	opkt.data = NULL;
	opkt.size = 0;

#if USE_H264BSF
	AVBitStreamFilterContext* h264bsfc = av_bitstream_filter_init("h264_mp4toannexb");
#endif

	while (1)
	{
		// 1：将输入流解封装，得到编码数据 --- demux
		if (av_read_frame(ifmt_ctx, &ipkt) < 0)	
			break;

		if (ipkt.stream_index == idx_audio)
		{
			av_interleaved_write_frame(ofmt_ctx, &ipkt);	// 对音频数据不作任何处理，直接写入输出流中
		}
		else if (ipkt.stream_index == idx_video)
		{
#if USE_H264BSF
			av_bitstream_filter_filter(h264bsfc, istream_video->codec, NULL, &ipkt.data, &ipkt.size, ipkt.data, ipkt.size, 0);
#endif
		
			int got_picture = 0;

			// 2：将输入视频流解码，得到输入视频的原始数据 --- decode
			avcodec_decode_video2(codec_ctx_video, &iframe, &got_picture, &ipkt);
			if (0 != got_picture)
			{
				// 3：将输入视频的原始数据转码为 BGR 格式
				sws_scale(img_convert_ctx_2bgr, (const uint8_t* const*)iframe.data, iframe.linesize, 0, iframe.height, oframe_bgr.data, oframe_bgr.linesize);

				// 4：简单处理：叠加图片
				cv::Mat background(oframe_bgr.height, oframe_bgr.width, CV_8UC3, oframe_bgr.data[0]);
				overlayImage(background, cv::imread("logo.jpg"), background, cv::Point(20, 30));
		
				// 保存 BGR 格式的原始数据
				fwrite(oframe_bgr.data[0], (oframe_bgr.width) * (oframe_bgr.height) * 3, 1, output_bgr);

				// 5：将处理后的原始视频数据由 BGR 格式转码为 YUV 格式
				sws_scale(img_convert_ctx_2yuv, (const uint8_t* const*)oframe_bgr.data, oframe_bgr.linesize, 0, oframe_bgr.height, oframe_yuv.data, oframe_yuv.linesize);
				
				// 保存 YUV 格式的原始数据
				fwrite(oframe_yuv.data[0], (oframe_yuv.width) * (oframe_yuv.height), 1, output_yuv);
				fwrite(oframe_yuv.data[1], (oframe_yuv.width) * (oframe_yuv.height) / 4, 1, output_yuv);
				fwrite(oframe_yuv.data[2], (oframe_yuv.width) * (oframe_yuv.height) / 4, 1, output_yuv);
	
				// 6：将原始视频数据编码为 H264 格式 --- encode
				got_picture = 0;
				oframe_yuv.pts = ipkt.pts;					// 需要设置合适的PTS，否则视频显示会有问题
				avcodec_encode_video2(codec_ctx_2h264, &opkt, &oframe_yuv, &got_picture);
				if (0 != got_picture)
				{
					// 保存 H264 格式的编码数据
					fwrite(opkt.data, 1, opkt.size, output_h264);
				
					// 7：将编码后的视频数据封装到输出流中 --- mux
					opkt.stream_index = idx_video;			// 设置合适的 stream_index，以保证顺利写入
					if (av_interleaved_write_frame(ofmt_ctx, &opkt) < 0)
					{
						printf("error: av_interleaved_write_frame, output video");
						break;
					}

					static int count = 0;
					printf("count = %d\n", count++);
				}
			}

			av_free_packet(&opkt);		// 需要么？待确定。
		}
		else
		{
			continue;
		}

		av_free_packet(&ipkt);
	}

	if (0 != av_write_trailer(ofmt_ctx))
	{
		printf("error: av_write_trailer\n");
		goto END;
	}

END:

#if USE_H264BSF
	av_bitstream_filter_close(h264bsfc);
#endif

	avformat_close_input(&ifmt_ctx);

	av_free(buffer_bgr);
	av_free(buffer_yuv);

	if (NULL != output_bgr)
		fclose(output_bgr);
	if (NULL != output_yuv)
		fclose(output_yuv);
	if (NULL != output_h264)
		fclose(output_h264);

	avcodec_free_context(&codec_ctx_2h264);

	if (NULL != ofmt_ctx)
		avio_close(ofmt_ctx->pb);

	avformat_free_context(ofmt_ctx);

    return 0;
}