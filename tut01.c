// tutorial01.c
// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101 
// on GCC 4.7.2 in Debian February 2015

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
// gcc -o tutorial01 tutorial01.c -lavformat -lavcodec -lswscale -lz
// gcc -o tutorial01 tut01.c -lavformat -lavcodec -lswscale -lswresample -lavutil -lz -lm
//
// to build (assuming libavformat and libavcodec are correctly installed
// your system).
//
// Run using
//
// tutorial01 myvideofile.mpg
//
// to write the first five frames from "myvideofile.mpg" to disk in PPM
// format.

#include <libavutil/imgutils.h>  // for av_image_get_buffer_size
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <stdio.h>

void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame) {
    FILE *pFile;
    char szFilename[32];
    int  y;

    // Open file
    sprintf(szFilename, "frame%d.ppm", iFrame);
    errno_t err =fopen_s(&pFile, szFilename, "wb");
    if(err != 0)
        return;

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for(y=0; y<height; y++)
        fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

    // Close file
    fclose(pFile);
}

int main(int argc, char *argv[]) {
    // Initalizing these to NULL prevents segfaults!
    // e.g. AVFrame           *pFrame = NULL;

    if(argc < 2) {
        printf("Please provide a movie file\n");
        return -1;
    }
    // Register all formats and codecs
    // can be omitted in ffmpeg > 4.0: av_register_all();

    // Open video file
    // allocate AVFormatContext avformat_alloc_context() could also pass the address
    // to a NULL ptr which will lead to avformat_open_input to alloc ate a AVFormatContext
    // AVFormatContext *pFormatContext = avformat_alloc_context();
    AVFormatContext *pFormatCtx = NULL;
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        return -1; // Couldn't open file

    // print format name and duration
    printf("Format %s, duration %lld us", pFormatCtx->iformat->long_name,
           pFormatCtx->duration);

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Find the first video stream
    // this component describes the properties of a codec used by the stream i
    // https://ffmpeg.org/doxygen/trunk/structAVCodecParameters.html
    AVCodecParameters *pCodecParameters =  NULL;
    int video_stream_index = -1;

    for(int i=0; i<pFormatCtx->nb_streams; i++)
    {
        // using codec->codec_type directly is deprecated use AVCodec::type instead
        // u get AVCodec using avcodec_find_encoder with codec_id
        pCodecParameters = pFormatCtx->streams[i]->codecpar;
        if(pCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
        //pCodec = avcodec_find_decoder(pFormatCtx->streams[i]->codec->codec_id);
    }
    if(video_stream_index == -1)
        return -1; // Didn't find a video stream

    AVCodec *pCodec = NULL;
    pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if(pCodec == NULL)
    {
        fprintf(stderr, "ERROR: Unsupported codec!");
        return -1; // Unsupported codec
    }

    // Copy context params
    AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
    if(!pCodecCtx)
        return -1; // failed to alloc

    // used codec params since avcodec_copy_context is deprecated
    if(avcodec_parameters_to_context(pCodecCtx, pCodecParameters) < 0) {
        fprintf(stderr, "Couldn't copy codec params to codec context");
        return -1; // Error copying codec params to context
    }

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
        return -1; // Could not open codec


    // Allocate video frame
    AVFrame *pFrame = av_frame_alloc();
    if(pFrame == NULL)
        return -1;

    // Allocate an AVFrame structure
    AVFrame *pFrameRGB = av_frame_alloc();
    if(pFrameRGB==NULL)
        return -1;

    /* AVPacket *pPacket = av_packet_alloc(); */
    /* if (!pPacket) */
    /* { */
    /*     fprintf(stderr, "failed to allocated memory for AVPacket"); */
    /*     return -1; */
    /* } */

    // deprecated avpicture_get_size use av_image_get_buffer_size
    // Determine required buffer size and allocate buffer
    // last param is linesize alignment: ffmpeg uses specific alignment for compatability
    // with SIMD; if you're going to use this picture as output buffer for
    // calls to e.g. avcodec_decode_video2, this should be 32
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
                                            pCodecCtx->height, 32);
    if(numBytes < 0)
    {
        fprintf(stderr, "Failed getting av buffer size!");
        return -1;
    }

    uint8_t *buffer = (uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
    if(!buffer)
        return -1;

    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    // deprectade: avpicture_fill((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24,
    //              pCodecCtx->width, pCodecCtx->height);
    // AVPicture also deprecated in favor of AVFrame
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24,
            pCodecCtx->width, pCodecCtx->height, 32);

    // initialize SWS context for software scaling
    struct SwsContext *sws_ctx = sws_getContext(
        pCodecCtx->width,
        pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width,
        pCodecCtx->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    // Read frames and save first five frames to disk
    int i = 0;
    int frameFinished;
    AVPacket          packet;

    while(av_read_frame(pFormatCtx, &packet) >= 0) {
        // Is this a packet from the video stream?
        if(packet.stream_index == video_stream_index) {
            // Decode video frame
            // deprecated: avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
            // Supply raw packet data as input to a decoder
            int response = avcodec_send_packet(pCodecCtx, &packet);
            if (response < 0)
            {
                fprintf(stderr, "Error while sending a packet to the decoder: %s",
                        av_err2str(response));
                return response;
            }

            while (response >= 0)
            {
                // Return decoded output data (into a frame) from a decoder
                response = avcodec_receive_frame(pCodecCtx, pFrame);
                if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                    break;
                } else if (response < 0) {
                    fprintf(stderr, "Error while receiving a frame from the decoder: %s",
                            av_err2str(response));
                    return response;
                }

                // Convert the image from its native format to RGB
                sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                        pFrame->linesize, 0, pCodecCtx->height,
                        pFrameRGB->data, pFrameRGB->linesize);

                // Save the frame to disk
                if(++i <= 5)
                    SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
            }
        }

        // Free the packet that was allocated by av_read_frame
        // deprecated: av_free_packet(&packet);
        av_packet_unref(&packet);
    }

    // Free the RGB image
    av_free(buffer);
    av_frame_free(&pFrameRGB);

    // Free the YUV frame
    av_frame_free(&pFrame);

    // Close the codecs
    // avcodec_close: Close a given AVCodecContext and free all the data
    // associated with it (but not the AVCodecContext itself). 
    // Do not use this function. Use avcodec_free_context() to destroy a codec
    // context (either open or closed)
    avcodec_free_context(&pCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
