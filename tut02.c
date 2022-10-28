// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
// gcc -o tutorial02 tut02.c -ISDL2-2.0.12/x86_64-w64-mingw32/include/SDL2 -LSDL2-2.0.12/x86_64-w64-mingw32/lib -lmingw32 -lSDL2main -lSDL2 -mwindows -lavformat -lavcodec -lswscale -lswresample -lavutil -lz -lm
// 
//
// to build.
//
// Run using
//
// tutorial02 myvideofile.mpg

#include <libavutil/imgutils.h>  // for av_image_get_buffer_size
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>

int main(int argc, char *argv[]) {
    // Initalizing these to NULL prevents segfaults!
    // e.g. AVFrame           *pFrame = NULL;

    if(argc < 2) {
        fprintf(stderr, "Usage: tut02 <file>\n");
        exit(1);
    }

    // initialize sdl and tell what features we're going to use
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    // Open video file
    // allocate AVFormatContext avformat_alloc_context() could also pass the address
    // to a NULL ptr which will lead to avformat_open_input to alloc ate a AVFormatContext
    // AVFormatContext *pFormatContext = avformat_alloc_context();
    AVFormatContext *pFormatCtx = NULL;
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        exit(1); // Couldn't open file

    // print format name and duration
    printf("Format %s, duration %lld us", pFormatCtx->iformat->long_name,
           pFormatCtx->duration);

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        exit(1); // Couldn't find stream information

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
        exit(1); // Didn't find a video stream

    AVCodec *pCodec = NULL;
    pCodec = avcodec_find_decoder(pCodecParameters->codec_id);
    if(pCodec == NULL)
    {
        fprintf(stderr, "ERROR: Unsupported codec!");
        exit(1); // Unsupported codec
    }

    // Copy context params
    AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
    if(!pCodecCtx)
        exit(1); // failed to alloc

    // used codec params since avcodec_copy_context is deprecated
    if(avcodec_parameters_to_context(pCodecCtx, pCodecParameters) < 0) {
        fprintf(stderr, "Couldn't copy codec params to codec context");
        exit(1); // Error copying codec params to context
    }

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
        exit(1); // Could not open codec

    // Allocate video frame
    AVFrame *pFrame = av_frame_alloc();
    if(pFrame == NULL)
        exit(1);

    // SDL_Overlay is not used in SDL2 anymore render to a texture with the appropriate
    // format instead
    // Create window
    SDL_Window *window = SDL_CreateWindow(
            "FFmpeg Video", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            pCodecCtx->width, pCodecCtx->height, 0);//SDL_WINDOW_SHOWN);
    if( window == NULL )
    {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }

    // window, index of rendering driver -1=first that meets reqs, flags
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if(renderer == NULL)
    {
        fprintf(stderr, "Could not create renderer! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }
    
    // Allocate a place to put our YUV image on that screen
    SDL_Texture *texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_YV12,
            SDL_TEXTUREACCESS_STREAMING,  // important to not use static
            pCodecCtx->width,
            pCodecCtx->height
        );
    if (!texture) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        // destroying these technically not neccessary since the program
        // is closing anyway but better if we want to factor this out later
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }

    // initialize SWS context for software scaling
    // convert native format to YUV420P thats used by SDL YUV Overlay
    // NOTE: YUV420P is the same as YV12, except the U and V arrays are switched
    struct SwsContext *sws_ctx = sws_getContext(
        pCodecCtx->width,
        pCodecCtx->height,
        pCodecCtx->pix_fmt,
        pCodecCtx->width,
        pCodecCtx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    /**
     * As we said before, we are using YV12 to display the image, and getting
     * YUV420 data from ffmpeg.
     */

    // buffer for image data
    int numBytes;
    uint8_t * buffer = NULL;

    numBytes = av_image_get_buffer_size(
                AV_PIX_FMT_YUV420P,
                pCodecCtx->width,
                pCodecCtx->height,
                32
            );
    if(numBytes < 0)
    {
        fprintf(stderr, "Failed getting av buffer size!");
        exit(1);
    }
    buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));

    AVFrame *pFrameConverted = av_frame_alloc();  // av_frame_alloc does not alloc the data buffers
    // Setup the data pointers and linesizes based on the specified image
    // parameters and the provided array.
    av_image_fill_arrays(
        pFrameConverted->data,
        pFrameConverted->linesize,
        buffer,
        AV_PIX_FMT_YUV420P,
        pCodecCtx->width,
        pCodecCtx->height,
        32
    );

    // frame that will hold converted output
    AVPacket packet;
    SDL_Rect rect;
    // used later to handle quit event
    SDL_Event event;
    while(av_read_frame(pFormatCtx, &packet) >= 0)
    {
        // Is this a packet from the video stream?
        if(packet.stream_index == video_stream_index)
        {
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

                // Convert the image into YUV format that SDL uses
                sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
                        pFrame->linesize, 0, pCodecCtx->height,
                        pFrameConverted->data, pFrameConverted->linesize);

                // get clip fps
                double fps = av_q2d(pFormatCtx->streams[video_stream_index]->r_frame_rate);

                // get clip sleep time
                double sleep_time = 1.0/(double)fps;

                // sleep: usleep won't work when using SDL_CreateWindow
                // usleep(sleep_time);
                // Use SDL_Delay in milliseconds to allow for cpu scheduling
                // waits at least the specified time
                // delay granularity is at least 10ms
                SDL_Delay((1000 * sleep_time) - 10);    // [5]

                // display the data
                // as a rect so SDL takes care of the scaling for us (can use GPU)
                // 0,0 upper left corner in SDL
                rect.x = 0;
                rect.y = 0;
                rect.w = pCodecCtx->width;
                rect.h = pCodecCtx->height;

                // print frame info
                printf(
                        "Frame %c (%d) pts %d dts %d key_frame %d "
                        "[coded_picture_number %d, display_picture_number %d, %dx%d]\n",
                        av_get_picture_type_char(pFrame->pict_type),
                        pCodecCtx->frame_number,
                        pFrame->pts,
                        pFrame->pkt_dts,
                        pFrame->key_frame,
                        pFrame->coded_picture_number,
                        pFrame->display_picture_number,
                        pCodecCtx->width,
                        pCodecCtx->height
                      );

                // Use this function to update a rectangle within a planar
                // YV12 or IYUV texture with new pixel data.
                SDL_UpdateYUVTexture(
                    texture,            // the texture to update
                    // a pointer to the rectangle of pixels to update, or
                    // NULL to update the entire texture
                    &rect,              
                    // the raw pixel data for the Y plane
                    pFrameConverted->data[0],
                    // the number of bytes between rows of pixel data for the Y plane
                    pFrameConverted->linesize[0],
                    // the raw pixel data for the U plane
                    pFrameConverted->data[1],
                    // the number of bytes between rows of pixel data for the U plane
                    pFrameConverted->linesize[1],
                    // the raw pixel data for the V plane
                    pFrameConverted->data[2],
                    // the number of bytes between rows of pixel data for the V plane
                    pFrameConverted->linesize[2]
                );

                // clear the current rendering target with the drawing color
                SDL_RenderClear(renderer);

                // copy a portion of the texture to the current rendering target
                SDL_RenderCopy(
                        renderer,   // the rendering context
                        texture,    // the source texture
                        NULL,       // the source SDL_Rect structure or NULL for the entire texture
                        NULL        // the destination SDL_Rect structure or NULL for the entire rendering
                        // target; the texture will be stretched to fill the given rectangle
                        );

                // update the screen with any rendering performed since the previous call
                SDL_RenderPresent(renderer);
            }
        }

        // Free the packet that was allocated by av_read_frame
        // deprecated: av_free_packet(&packet);
        av_packet_unref(&packet);

        // use SDL event system to check if window should be closed
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                SDL_DestroyTexture(texture);
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(window);
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
    }


    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    av_frame_free(&pFrame);
    // does this also free the data buffers that were manually allocated?
    av_frame_free(&pFrameConverted);

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
