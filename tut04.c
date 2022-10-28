// Code based on a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial

// A small sample program that shows how to use libavformat and libavcodec to
// read video from a file.
//
// Use
//
// gcc -o tutorial03 tut03.c -ISDL2-2.0.12/x86_64-w64-mingw32/include/SDL2 -LSDL2-2.0.12/x86_64-w64-mingw32/lib -lmingw32 -lSDL2main -lSDL2 -mwindows -lavformat -lavcodec -lswscale -lswresample -lavutil -lz -lm
// 
//
// to build.
//
// Run using
//
// tutorial02 myvideofile.mpg

#include <stdio.h>
#include <assert.h>

#include <libavutil/imgutils.h>  // for av_image_get_buffer_size
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

// prevent SDL from overriding main, but then we need to call SDL_SetMainReady() in our main
// so everything can be initialized, or make sure we do it ourselves
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_thread.h>

// #ifdef __MINGW32__
// #undef main /* Prevents SDL from overriding main() */
// #endif

// in samples
#define SDL_AUDIO_BUFFER_SIZE 1024
// in number of samples per channel in an audio frame
#define MAX_AUDIO_FRAME_SIZE 192000
// Audio packets queue maximum size.
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
// Video packets queue maximum size.
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
// Custom SDL_Event type.
// first custom event should use SDL_USEREVENT, next SDL_USEREVENT + 1 etc.
// Notifies the next video frame has to be displayed.
#define FF_REFRESH_EVENT (SDL_USEREVENT)
// Notifies the program needs to quit.
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)
// :ReactivateWrap when changed to > 1
// Video Frame queue size. Needs to be power of 2 (to enable efficient mod using &)
#define VIDEO_PICTURE_QUEUE_SIZE 2

/**
 * PacketQueue Structure Declaration.
 * queue for audio packets that we get from ffmpeg and that we want to play with sdl
 * AVPacketList is a linked-list of AVPacket with just the pkt and a next ptr
 */
typedef struct PacketQueue
{
    AVPacketList *first_pkt;
    AVPacketList *last_pkt;
    int nb_packets;
    // byte size that we get from packet->size
    int size;
    SDL_mutex *mutex;
    // can be used to wait on and another thread can send a signal so the
    // waiting thread continues
    SDL_cond *cond;
} PacketQueue;
// the typedef struct TypeName {..} InstanceName;
// are actually two declarations
// struct TypeName {..}; and
// struct TypeName InstanceName;
// identifiert TypeName in the two declarations is a struct tag and
// only works when preceded by the struct kw
// the typedef in the first example gives struct TypeName a second name
// (typedef doesn't create new types it just creates a new alias for them)
// so InstanceName in the first example is an alias for struct TypeName
// could also be written as:
// struct TypeName {..};
// typedef struct TypeName InstanceName;
// if you use a typedef for creating a single name alias depends on style
// Linux kernel coding style:
// It's a mistake to use typedef for structures and pointers. When you see a
//  vps_t a;
// in the source, what does it mean? In contrast, if it says
//  struct virtual_container *a;
// you can actually tell what "a" is.

typedef struct VideoPicture
{
  AVFrame *frame;
  int width, height; /* source height & width */
  int allocated;
} VideoPicture;

typedef struct VideoState
{
    // File I/O Context
    AVFormatContext *pFormatCtx;

    // Audio stream
    int audio_stream_index;
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audioq;
    // The size of audio_buf is 1.5 times the size of the largest audio frame
    // that FFmpeg will give us, which gives us a nice cushion.
    uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    // AVPacket        audio_pkt;
    // uint8_t         *audio_pkt_data;
    // int             audio_pkt_size;

    // Video stream
    int video_stream_index;
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    struct SwsContext *video_sws_ctx;

    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;

    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;

    char            filename[1024];
    int             quit;
} VideoState;

SDL_Window     *window;
SDL_mutex      *window_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;

int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        fprintf(stderr, "SDL_CreateMutex: %s\n", SDL_GetError());
        return -1;
    }

    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        fprintf(stderr, "SDL_CreateCond: %s\n", SDL_GetError());
        return -1;
    }

    return 1;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    // alloc the new AVPacketList to be inserted in the audio PacketQueue
    AVPacketList *avPacketList = av_malloc(sizeof(AVPacketList));
    if(!avPacketList)
        return -1;

    avPacketList->pkt = *pkt;
    avPacketList->next = NULL;

    // SDL is running the audio process as a separate thread. If we don't lock
    // the queue properly, we could really mess up our data
    SDL_LockMutex(q->mutex);

    // check if queue empty
    if(!q->last_pkt)
        q->first_pkt = avPacketList;
    else
        q->last_pkt->next = avPacketList;

    q->last_pkt = avPacketList;
    q->nb_packets++;
    // add size of add packet
    q->size += avPacketList->pkt.size;

    // notify packet_queue_get which is waiting that a new packet is available
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);

    return 0;
}

// taken from ffplay
static int packet_queue_put_nullpacket(PacketQueue *q)// int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    // pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static int packet_queue_get(PacketQueue *q, AVPacket *out_pkt, int block)
{
    AVPacketList *pkt_list;
    int ret;

    SDL_LockMutex(q->mutex);
    // wait till we get a packet from the queue if block != 0
    // otherwise try to get a packet and just return on empty q
    for (;;)
    {
        // global quit so we can stop waiting when e.g. the window is closed
        if(global_video_state->quit)
        {
            ret = -1;
            break;
        }

        pkt_list = q->first_pkt;
        // empty q
        if(pkt_list)
        {
            q->first_pkt = pkt_list->next;
            // q is now empty
            if(!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;

            q->size -= pkt_list->pkt.size;
            *out_pkt = pkt_list->pkt;

            // free the packet/packetlist item
            av_free(pkt_list);

            ret = 1;
            break;
        }
        else if(!block)
        {
            // don't block
            ret = 0;
            break;
        }
        else
        {
            // wait on conditon variable this won't result in a deadlock
            // since SDL_CondWait also unlocks the mutex and tries to re-lock
            // it once we get the signal for the cond variable
            // TODO will the thread lock here if there are no more packets that will
            // signal us to wake up?
            SDL_CondWait(q->cond, q->mutex);
        }
    }

    SDL_UnlockMutex(q->mutex);

    return ret;
}

static int audio_resampling(
        AVCodecContext * audio_decode_ctx,
        AVFrame * decoded_audio_frame,
        enum AVSampleFormat out_sample_fmt,
        int out_channels,
        int out_sample_rate,
        uint8_t * out_buf,
        int buf_size  // empty space left in buff
)
{
    // check global quit flag
    if (global_video_state->quit)
    {
        return -1;
    }

    // For C89/ANSI C, you must declare all of your variables at the beginning of a scope block.
    // C99 supports declaring them at other parts inside a scope block
    // we'll be using the latter while the original dranger tutorial and the updated one by
    // rambodrahmani on github still want to support the  former

    /* create resampler context */
    SwrContext * swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        return AVERROR(ENOMEM);
    }

    // get input audio channels
    int64_t in_channel_layout = (audio_decode_ctx->channels ==
        av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ?
        audio_decode_ctx->channel_layout :
        av_get_default_channel_layout(audio_decode_ctx->channels);

    // check input audio channels correctly retrieved
    if (in_channel_layout <= 0)
    {
        printf("in_channel_layout error.\n");
        return -1;
    }

    // set output audio channels based on the input audio channels
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    if (out_channels == 1)
    {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    }
    else if (out_channels == 2)
    {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else
    {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    // retrieve number of audio samples (per channel)
    int in_nb_samples = decoded_audio_frame->nb_samples;
    if (in_nb_samples <= 0)
    {
        printf("in_nb_samples error.\n");
        return -1;
    }

    // Set SwrContext parameters for resampling
    // obj ptr, name of field to set, value, search_flags
    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    // Once all values have been set for the SwrContext, it must be initialized
    // with swr_init().
    int ret = swr_init(swr_ctx);
    if (ret < 0)
    {
        printf("Failed to initialize the resampling context.\n");
        return -1;
    }

    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    max_out_nb_samples = out_nb_samples = av_rescale_rnd(
                                              in_nb_samples,
                                              out_sample_rate,
                                              audio_decode_ctx->sample_rate,
                                              AV_ROUND_UP
                                          );

    // check rescaling was successful
    if (max_out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error.\n");
        return -1;
    }

    // get number of output audio channels
    int out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    // alloc out buffer
    int out_linesize = 0;
    uint8_t ** resampled_data = NULL;
    ret = av_samples_alloc_array_and_samples(
              &resampled_data,
              &out_linesize,
              out_nb_channels,
              out_nb_samples,
              out_sample_fmt,
              0
          );

    if (ret < 0)
    {
        printf("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.\n");
        return -1;
    }

    // retrieve output samples number taking into account the progressive delay
    out_nb_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                        out_sample_rate,
                        audio_decode_ctx->sample_rate,
                        AV_ROUND_UP
                     );

    // check output samples number was correctly retrieved
    if (out_nb_samples <= 0)
    {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples)
    {
        // free memory block and set pointer to NULL
        av_freep(&resampled_data[0]);  // was using free instead freep so pointer was not freed

        // Allocate a samples buffer for out_nb_samples samples
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels,
                               out_nb_samples, out_sample_fmt, 1);

        // check samples buffer correctly allocated
        if (ret < 0)
        {
            printf("av_samples_alloc failed.\n");
            return -1;
        }

        max_out_nb_samples = out_nb_samples;
    }

    int resampled_data_size = 0;
    if (swr_ctx)
    {
        // do the actual audio data resampling
        ret = swr_convert(
                  swr_ctx,
                  resampled_data,
                  out_nb_samples,
                  (const uint8_t **) decoded_audio_frame->data,
                  decoded_audio_frame->nb_samples
              );

        // check audio conversion was successful
        if (ret < 0)
        {
            printf("swr_convert_error.\n");
            return -1;
        }

        // Get the required buffer size for the given audio parameters
        resampled_data_size = av_samples_get_buffer_size(
                &out_linesize, out_nb_channels,
                ret, out_sample_fmt, 1);

        // check audio buffer size
        if (resampled_data_size < 0)
        {
            printf("av_samples_get_buffer_size error.\n");
            return -1;
        }
    }
    else
    {
        printf("swr_ctx null error.\n");
        return -1;
    }

    // copy the resampled data to the output buffer
    assert(resampled_data_size <= buf_size);
    memcpy(out_buf, resampled_data[0], resampled_data_size);

    /*
     * Memory Cleanup.
     */
    if (resampled_data)
    {
        // free memory block and set pointer to NULL
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);  // freep already sets ptr to NULL

    if (swr_ctx)
    {
        // Free the given SwrContext and set the pointer to NULL
        swr_free(&swr_ctx);
    }

    return resampled_data_size;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size)
{
    // ffmpeg explicitly states that AVPacket can be allocated on the stack
    // uinitialized value for packet.data was some sdl memory which was
    // then freed by packet_unref -> zero init
#ifdef _WIN32
    AVPacket packet = {0};
#else
    AVPacket packet = {};
#endif
    /* uint8_t *audio_pkt_data = NULL; */
    /* int audio_pkt_size = 0; */
    static AVFrame* frame = NULL;
    
    // alloc frame that will hold the decoded audio output
    if(!frame)
        frame = av_frame_alloc();

    // get packet from q
    if(packet_queue_get(&is->audioq, &packet, 1) < 0)
        return -1;
    /* audio_pkt_data = packet.data; */
    /* audio_pkt_size = packet.size; */

    AVCodecContext *aCodecCtx = is->audio_ctx;
    // send packet to decoder so we can receive individual audio frames
    int response = avcodec_send_packet(aCodecCtx, &packet);
    if (response < 0)
    {
        fprintf(stderr, "Error while sending an audio packet to the decoder: %s",
                av_err2str(response));
        return response;
    }

    int data_size = 0;
    int offset = 0;

    // get a frame from the packet (which might contain multiple frames)
    // write the data to the audio_buf and return the data size
    // do the same for subsequent calls
    while (response >= 0)
    {
        // Return decoded output data (into a frame) from a decoder
        response = avcodec_receive_frame(aCodecCtx, frame);
        if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
            break;
        } else if (response < 0) {
            fprintf(stderr, "Error while receiving a frame from the decoder: %s",
                    av_err2str(response));
            return response;
        }

#if 1
        data_size = audio_resampling(
                aCodecCtx,
                frame,
                // what sdl2 uses on my pc even if we ask for s16, also interleaved
                AV_SAMPLE_FMT_FLT,
                // AV_SAMPLE_FMT_S16, // interleaved format LRLRLR for stereo; S16P has sep channels
                aCodecCtx->channels,
                aCodecCtx->sample_rate,
                audio_buf + offset,  // since we might call mutliple times advance ptr
                buf_size - offset);  // remaining space in buf
#else
        // amount of data in the decoded frame
        data_size = av_samples_get_buffer_size(
                NULL, aCodecCtx->channels, 
                frame->nb_samples, aCodecCtx->sample_fmt, 1);

        assert((offset + data_size) <= buf_size);
#if 0
        // original method that doesn't work since sdl expects interleaved
        // as in LRLRLR.. for stereo while ffmpeg has separate channels
        // and so doing this we just get LLLLL
        memcpy(audio_buf + offset, frame->data[0], data_size);
#endif
        // !!! this assumes now that we have 32bit input and 32bit output
        // only cast to int32_t after adding offset otherwise it will multiplty it
        // by the sizeof the type in bytes which is fine here since audio_buf is 1 byte
        // using int32_t but in/out values can also be float32
        int32_t *write_ptr = (int32_t *)(audio_buf + offset);
        // would be total size for each plane: frame->linesize[0];
        int bytes_per_sample = av_get_bytes_per_sample(aCodecCtx->sample_fmt);

        for(int sample_idx=0; sample_idx < frame->nb_samples; ++sample_idx)
        {
            for(int ch=0; ch < frame->channels; ++ch)
            {

                // could write one byte at a time bytes_per_sample times
                // but this would be considerably slower for larger bytes_per_sample
                // other solution could be always writing 32bits and masking the rest?
                // but then we'd have weird edge cases at the end of the buffer
                // ref: https://www.embedded.com/optimizing-memcpy-improves-speed/
                // ffmpeg uses a macrod function for this: see libswresample/audioconvert.c:38
                // with: *(out_type*)ptr_out = conversion_expr;
                //       ptr_in += in_step; ptr_out += out_step;
                *write_ptr = *(int32_t *)(frame->data[ch] + bytes_per_sample * sample_idx);
                ++write_ptr;
            }
        }
#endif
        // update new total written
        offset += data_size;
    }

    av_packet_unref(&packet);

    return offset;
}

// sdl audio callback
void audio_callback(void *userdata, Uint8 *stream, int len)
{
    // NOTE: IMPORTANT SDL stores the samples for the channels as
    // LRLRLR ordering whereas ffmpeg has separate buffers for each channel
    // https://wiki.libsdl.org/SDL_AudioSpec#callback
    // could either use swr_convert to convert the input format (planar? separate
    // data plane for each channel - see AVSampleFormat) to the output of
    // packed/interleaved (done by tutorial03-resampled, but it only produces silence)
    // or we could do it manually but then we'd just repeat the code and we'd have
    // to handle all input formats
    //
    // ptr we put on the audio spec
    VideoState *is = (VideoState *)userdata;
    int len_to_write, audio_size;

    uint8_t *audio_buf = is->audio_buf;
    // callback must completely initialize the buffer; as of SDL 2.0, this
    // buffer is not initialized before the callback is called. If there is
    // nothing to play, the callback should fill the buffer with silence
    while(len > 0)
    {
        if(global_video_state->quit)
            return;

        if(is->audio_buf_index >= is->audio_buf_size)
        {
            // ran out of data -> get new
            audio_size = audio_decode_frame(is, audio_buf, sizeof(is->audio_buf));
            if(audio_size < 0)
            {
                // output silence on err
                is->audio_buf_size = 1024;//len;
                memset(audio_buf, 0, is->audio_buf_size);
                fprintf(stderr, "audio_decode_frame() failed.\n");
            }
            else
            {
                is->audio_buf_size = audio_size;
            }

            is->audio_buf_index = 0;
        }

        len_to_write = is->audio_buf_size - is->audio_buf_index;
        // only write sound to end of buffer
        if(len_to_write > len)
            len_to_write = len;

        memcpy(stream, (uint8_t *)audio_buf + is->audio_buf_index, len_to_write);

        // update remaining size of audio buffer by amount written
        len -= len_to_write;
        // advance ptr
        stream += len_to_write;
        is->audio_buf_index += len_to_write;
    }
}

void alloc_picture(VideoState *is)
{
    VideoPicture *vp = &is->pictq[is->pictq_windex];

    // do we even need this?
    // However, now we have a mutex lock around it because two threads cannot
    // write information to the screen at the same time! This will prevent our
    // alloc_picture function from stepping on the toes of the function that
    // will display the picture.
    // but we're not using overlays but the frame itself the only case we'd be
    // in trouble is if the read idx catches up to the write idx
    // but that should not ever happen anyway?
    // no it can if we have a small q size like e.g. now only 1
    // lock global window_mutex
    SDL_LockMutex(window_mutex);

    // allocate a new frame, assuming width/height changed
    AVFrame *frame = vp->frame;
    if(frame)
    {
        // av_frame_unref or av_frame_free to "clean up" the frame or clean up
        // and release it (which might not free image data because it is used
        // by some other AVFrame or by an encoder using an AVBufferRef)
        // av_frame_free will do exactly that. It will release any AVBufferRefs
        // attached to AVFrame and then it will free AVFrame itself
        av_frame_unref(vp->frame);
        // TODO does this just take care of refcounted buffers or also of
        // manually allocated frame->data.. buffers?
    }
    else
    {
        frame = av_frame_alloc();  // av_frame_alloc does not alloc the data buffers
        vp->frame = frame;
    }

    if(!frame)
    {
        fprintf(stderr, "Failed to allocate frame!\n");
        SDL_UnlockMutex(window_mutex);
        return;
    }

    AVCodecContext *pCodecCtx = is->video_ctx;
    // buffer for image data
    int numBytes = av_image_get_buffer_size(
                AV_PIX_FMT_YUV420P,
                pCodecCtx->width,
                pCodecCtx->height,
                32
            );
    if(numBytes < 0)
    {
        fprintf(stderr, "Failed getting av buffer size!");
        SDL_UnlockMutex(window_mutex);
        return;
    }
    uint8_t *buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));

    // Setup the data pointers and linesizes based on the specified image
    // parameters and the provided array.
    av_image_fill_arrays(
        frame->data,
        frame->linesize,
        buffer,
        AV_PIX_FMT_YUV420P,
        pCodecCtx->width,
        pCodecCtx->height,
        32
    );

    // unlock global window mutex
    SDL_UnlockMutex(window_mutex);
    
    // update VideoPicture struct fields
    vp->width  = pCodecCtx->width;
    vp->height = pCodecCtx->height;
    vp->allocated = 1;
}

int queue_picture(VideoState *is, const AVFrame *pFrame)
{
    VideoPicture *vp;

    /* wait until we have space for a new pic */
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit)
    {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit)
        return -1;

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    AVCodecContext *pCodecCtx = is->video_ctx;
    /* allocate or resize the buffer! */
    if(!vp->allocated || vp->width != pCodecCtx->width || vp->height != pCodecCtx->height)
    {
        SDL_Event event;

        vp->allocated = 0;
        alloc_picture(is);
        if(is->quit) {
            return -1;
        }
    }

    if(vp->frame)
    {
        AVFrame *pFrameConverted = vp->frame;

        // copy metadata from decoded original frame to allocated
        // frame that will hold the converted frame; dst <- src
        // IMPORTANT
        // Metadata for the purpose of this function are those fields that do
        // not affect the data layout in the buffers
        // but not width/height or channel layout
        av_frame_copy_props(pFrameConverted, pFrame);
        pFrameConverted->channel_layout = pFrame->channel_layout;
        pFrameConverted->width = pFrame->width;
        pFrameConverted->height = pFrame->height;

        // Convert the image into YUV format that SDL uses
        sws_scale(is->video_sws_ctx, (uint8_t const * const *)pFrame->data,
                pFrame->linesize, 0, pCodecCtx->height,
                pFrameConverted->data, pFrameConverted->linesize);

        // ++is->pictq_windex;

        // :ReactivateWrap
        // wrap write index (using & instead of mod since we require
        // VIDEO_PICTURE_QUEUE_SIZE to be a power of 2
        is->pictq_windex &= (VIDEO_PICTURE_QUEUE_SIZE - 1);

        // The queue works by adding onto it until it is full, and reading from
        // it as long as there is something on it. Therefore everything depends
        // upon the is->pictq_size value, requiring us to lock it.
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }

    return 0;
}

int video_thread(void *arg)
{
    // read packets from video q, decode into frames and puts them in the
    // VideoPicture q

    VideoState *is = (VideoState *)arg;
#ifdef _WIN32
    AVPacket packet = {0};
#else
    AVPacket packet = {};
#endif
    // Allocate video frame
    AVFrame *pFrame = av_frame_alloc();
    if(pFrame == NULL)
        exit(1);
    AVCodecContext *pCodecCtx = is->video_ctx;

    int response = 0;
    PacketQueue *videoq = &is->videoq;
    for(;;)
    {
        if(packet_queue_get(videoq, &packet, 1) < 0)
            break;

        // Decode video frame
        // deprecated: avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
        // Supply raw packet data as input to a decoder
        response = avcodec_send_packet(pCodecCtx, &packet);
        if (response < 0)
        {
            fprintf(stderr, "Error while sending a packet to the decoder: %s",
                    av_err2str(response));
            goto error;
        }

        // a packet might contain multiple frames
        while (response >= 0)
        {
            // Return decoded output data (into a frame) from a decoder
            response = avcodec_receive_frame(pCodecCtx, pFrame);
            if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                break;
            } else if (response < 0) {
                fprintf(stderr, "Error while receiving a frame from the decoder: %s",
                        av_err2str(response));
                goto error;
            }

            // TODO queue_picture
            if(queue_picture(is, pFrame) < 0)
                goto error;

        }

        // Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
    }

    av_frame_free(&pFrame);
    return 0;

error:  // TODO packet doesnt get freed
    av_packet_unref(&packet);
    av_frame_free(&pFrame);
    return response;
}

int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecParameters *codecParameters = pFormatCtx->streams[stream_index]->codecpar;

    AVCodec *codec = avcodec_find_decoder(codecParameters->codec_id);
    if(codec == NULL)
    {
        fprintf(stderr, "ERROR: Unsupported codec!");
        return(-1); // Unsupported codec
    }

    // Copy context params
    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if(!codecCtx)
        return -1;

    // used codec params since avcodec_copy_context is deprecated
    if(avcodec_parameters_to_context(codecCtx, codecParameters) < 0) {
        fprintf(stderr, "Couldn't copy codec params to codec context");
        return -1; // Error copying codec params to context
    }

    // Open codec
    if(avcodec_open2(codecCtx, codec, NULL) < 0)
        return -1; // Could not open codec

    switch(codecParameters->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
        {
            is->audio_stream_index = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_ctx = codecCtx;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            // memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));

            // init audio PacketQueue
            packet_queue_init(&is->audioq);

            // SDL audio setup
            // we can ask for a spec but we might not get it
            SDL_AudioSpec wanted_spec, obtained_spec;

            wanted_spec.freq = codecCtx->sample_rate;
            // signed 16-bit samples in native byte order
            // we asked for AUDIO_S16SYS but on my system we get AUDIO_F32SYS
            wanted_spec.format = AUDIO_F32SYS;//AUDIO_S16SYS;
            wanted_spec.channels = codecCtx->channels;
            wanted_spec.silence = 0;
            // size of the audio buffer in sample frames. A sample frame is a chunk of
            // audio data of the size specified in format multiplied by the number of
            // channels; must be a power of 2
            wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
            // void SDL_AudioCallback(void*  userdata, Uint8* stream, int len)
            // userdata: application-specific parameter saved in the SDL_AudioSpec structure's userdata
            // stream: a pointer to the audio data buffer filled in by SDL_AudioCallback()
            // len: length of that buffer in bytes
            wanted_spec.callback = audio_callback;
            // so we have access to codec information in the audio callback
            wanted_spec.userdata = is;

            // use SDL_OpenAudioDevice instead of SDL_OpenAudio
            // uint32
            SDL_AudioDeviceID audio_device_id = SDL_OpenAudioDevice(
                    NULL, 0,
                    &wanted_spec, &obtained_spec,
                    SDL_AUDIO_ALLOW_FORMAT_CHANGE);
            if (audio_device_id == 0)
            {
                fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
                return -1;
            }
            // assert we got the output format we wanted otherwise our conversion in
            // audio_decode_frame will be wrong
            assert(wanted_spec.format == obtained_spec.format);

            // start playing audio on the given audio device
            SDL_PauseAudioDevice(audio_device_id, 0);
        } break;

        case AVMEDIA_TYPE_VIDEO:
        {
            is->video_stream_index = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            is->video_ctx = codecCtx;

            packet_queue_init(&is->videoq);

            /**
             * As we said before, we are using YV12 to display the image, and getting
             * YUV420 data from ffmpeg.
             */
            // initialize SWS context for software scaling
            // convert native format to YUV420P thats used by SDL YUV Overlay
            // NOTE: YUV420P is the same as YV12, except the U and V arrays are switched
            is->video_sws_ctx = sws_getContext(
                    codecCtx->width,
                    codecCtx->height,
                    codecCtx->pix_fmt,
                    codecCtx->width,
                    codecCtx->height,
                    AV_PIX_FMT_YUV420P,
                    SWS_BILINEAR,
                    NULL,
                    NULL,
                    NULL
                    );

            // Allocate a place to put our YUV image on that screen
            is->texture = SDL_CreateTexture(
                    is->renderer,
                    SDL_PIXELFORMAT_YV12,
                    SDL_TEXTUREACCESS_STREAMING,//SDL_TEXTUREACCESS_TARGET,  // important to not use static
                    codecCtx->width,
                    codecCtx->height
                    );
            if (!is->texture) {
                fprintf(stderr, "SDL: could not create texture - exiting\n");
                return -1;
            }

            is->video_tid = SDL_CreateThread(video_thread, "Video Thread", is);
        } break;

        default: break;
    }

    return 0;
}

// callback for ffmpeg's internal "quit"
int decode_interrupt_cb(void *opaque)
{
    // returning 1 aborts blocking operations inside ffmpeg 
    return (global_video_state && global_video_state->quit);
}

int decode_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;

    int ret = -1;

    // Open video file
    // allocate AVFormatContext avformat_alloc_context() could also pass the address
    // to a NULL ptr which will lead to avformat_open_input to alloc ate a AVFormatContext
    // AVFormatContext *pFormatContext = avformat_alloc_context();
    AVFormatContext *pFormatCtx = NULL;
    if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
    {
        fprintf(stderr, "Couldn't open file: %s\n", is->filename);
        goto fail;
    }
    // callback: func ptr, opaque: void ptr that will be passed to callback
    pFormatCtx->interrupt_callback.callback = decode_interrupt_cb;
    is->pFormatCtx = pFormatCtx;

    // print format name and duration
    printf("Format %s, duration %lld us\n", pFormatCtx->iformat->long_name,
           pFormatCtx->duration);

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0)
        goto fail; // Couldn't find stream information

    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    // Find the first video stream
    // this component describes the properties of a codec used by the stream i
    // https://ffmpeg.org/doxygen/trunk/structAVCodecParameters.html
    int video_stream_index = -1;
    int audio_stream_index = -1;

    for(int i=0; i<pFormatCtx->nb_streams; i++)
    {
        // using codec->codec_type directly is deprecated use AVCodec::type instead
        // u get AVCodec using avcodec_find_encoder with codec_id
        AVCodecParameters *testCodecParameters = pFormatCtx->streams[i]->codecpar;
        if((video_stream_index) == -1 &&
           (testCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO)) {
            video_stream_index = i;
        } else if ((audio_stream_index == -1) &&
                   (testCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO)) {
            audio_stream_index = i;
        }
    }
    if((video_stream_index == -1) || (audio_stream_index == -1))
    {
        fprintf(stderr, "Could not find video and/or audio stream!\n");
        goto fail;
    }

    if(stream_component_open(is, video_stream_index) < 0)
    {
        fprintf(stderr, "Could not open video stream\n");
        goto fail;
    }
    if(stream_component_open(is, audio_stream_index) < 0)
    {
        fprintf(stderr, "Could not open audio stream\n");
        goto fail;
    }

    PacketQueue *audioq = &is->audioq;
    PacketQueue *videoq = &is->videoq;
    AVPacket packet;
    // main decode loop: read in a packet and put it on the right queue
    for (;;)
    {
        // check global quit flag
        if (is->quit)
        {
            break;
        }

        // check audio and video packets queues size
        if (audioq->size > MAX_AUDIOQ_SIZE || videoq->size > MAX_VIDEOQ_SIZE)
        {
            SDL_Delay(10);
            continue;
        }

        // read data from the AVFormatContext by repeatedly calling av_read_frame()
        ret = av_read_frame(is->pFormatCtx, &packet);
        if (ret  < 0)
        {
            if(ret == AVERROR_EOF || avio_feof(pFormatCtx->pb))
            {
                printf("averr_eof %d, aviofeof %d\n", ret == AVERROR_EOF, avio_feof(pFormatCtx->pb));
                // END OF FILE
                // signal threads that we reached eof with a nullpacket
                /* packet_queue_put_nullpacket(videoq); */
                /* packet_queue_put_nullpacket(audioq); */
                break;
            }
            // ByteIOContext pb is the structure that basically keeps all the
            // low-level file information in it
            else if (pFormatCtx->pb->error == 0)
            {
                // no read error; wait for user input
                SDL_Delay(100);
                continue;
            }
            else
            {
                // exit for loop in case of error
                break;
            }
        }

        // put the packet in the appropriate queue
        if (packet.stream_index == is->video_stream_index)
        {
            packet_queue_put(videoq, &packet);
        }
        else if (packet.stream_index == is->audio_stream_index)
        {
            // put the AVPacket in the audio PacketQueue
            packet_queue_put(audioq, &packet);
            // packet not freed when we q it but later when we decode it
        }
        else
        {
            // otherwise free the memory
            av_packet_unref(&packet);
        }
    }

    // wait for the rest of the program to end
    // waiting on is->quit results in an infinite loop (unless other procedures
    // error out) since we are the only proc that knows that the are no more
    // frames in the video and we send the FF_QUIT_EVENT but is->quit
    // is set by the main loop IF it GETS a FF_QUIT_EVENT
    // while (!is->quit)
    // instead wait till all frames have been displayed
    // prob prolematic with a q size of 1 since we might check the moment
    // the vp is pulled off the q and just before the next one is put on
    // same/similar problem for packet qs
    while((is->pictq_size || is->audioq.nb_packets || is->videoq.nb_packets) && !is->quit)
    {
        SDL_Delay(100);
    }

    ret = 0;
fail:
    {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    };  // label needs to be followed by statement

    return ret;
}

void video_display(VideoState *is)
{
    VideoPicture *vp = &is->pictq[is->pictq_rindex];
    AVFrame *frame = vp->frame;

    if(!frame)
        return;

    // our window can be of any size
    // -> dynamically figure out how big we want our movie rectangle to be
    // figure out aspect ration
    // Some codecs will have an odd sample aspect ratio, which is simply the
    // width/height radio of a single pixel, or sample. Since the height and
    // width values in our codec context are measured in pixels, the actual
    // aspect ratio is equal to the aspect ratio times the sample aspect ratio
    float aspect_ratio;
    if (frame->sample_aspect_ratio.num == 0)
    {
        aspect_ratio = 0;
    }
    else
    {
        aspect_ratio = av_q2d(frame->sample_aspect_ratio) * frame->width / frame->height;
    }

    // do this here so we have a fallback if calculating it fails somehow
    if (aspect_ratio <= 0.0)
    {
        aspect_ratio = (float)frame->width / (float)frame->height;
    }

    // get the size of a window's client area
    int window_width;
    int window_height;
    SDL_GetWindowSize(window, &window_width, &window_height);

    // global SDL_Surface height
    int h = window_height;

    // retrieve width using the calculated aspect ratio and the window height
    // scale the movie to fit as big in our window as we can. The & -3
    // bit-twiddling in there simply rounds the value to the nearest multiple of 4
    int w = ((int) rint(h * aspect_ratio)) & -3;

    // if the new width is bigger than the window width
    if (w > window_width)
    {
        // set the width to the window width
        w = window_width;

        // recalculate height using the calculated aspect ratio and the window width
        h = ((int) rint(w / aspect_ratio)) & -3;
    }

    // center the video by dividing by 2
    int x = (window_width - w) / 2;
    int y = (window_height - h) / 2;
    
    SDL_Rect rect;
    // display the data
    // as a rect so SDL takes care of the scaling for us (can use GPU)
    // 0,0 upper left corner in SDL
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    // print frame info
    /* printf( */
    /*         "Frame %c (%d) pts %d dts %d key_frame %d " */
    /*         "[coded_picture_number %d, display_picture_number %d, %dx%d]\n", */
    /*         av_get_picture_type_char(frame->pict_type), */
    /*         pCodecCtx->frame_number, */
    /*         frame->pts, */
    /*         frame->pkt_dts, */
    /*         frame->key_frame, */
    /*         frame->coded_picture_number, */
    /*         frame->display_picture_number, */
    /*         pCodecCtx->width, */
    /*         pCodecCtx->height */
    /*       ); */

    SDL_LockMutex(window_mutex);

    // Use this function to update a rectangle within a planar
    // YV12 or IYUV texture with new pixel data.
    SDL_UpdateYUVTexture(
            is->texture,            // the texture to update
            // a pointer to the rectangle of pixels to update, or
            // NULL to update the entire texture
            NULL,//&rect,              
            // the raw pixel data for the Y plane
            frame->data[0],
            // the number of bytes between rows of pixel data for the Y plane
            frame->linesize[0],
            // the raw pixel data for the U plane
            frame->data[1],
            // the number of bytes between rows of pixel data for the U plane
            frame->linesize[1],
            // the raw pixel data for the V plane
            frame->data[2],
            // the number of bytes between rows of pixel data for the V plane
            frame->linesize[2]
            );

    // clear the current rendering target with the drawing color
    // SDL_SetRenderDrawColor(is->renderer, 0, 0, 0, 255);
    SDL_RenderClear(is->renderer);

    // copy a portion of the texture to the current rendering target
    SDL_RenderCopy(
            is->renderer,   // the rendering context
            is->texture,    // the source texture
            NULL,       // the source SDL_Rect structure or NULL for the entire texture
            &rect//NULL        // the destination SDL_Rect structure or NULL for the entire rendering
            // target; the texture will be stretched to fill the given rectangle
            );

    // we can already unlock here since we copied the data to the texture
    SDL_UnlockMutex(window_mutex);

    // update the window with any rendering performed since the previous call
    SDL_RenderPresent(is->renderer);
}

// trigger an event, which will have our main() function in turn call a
// function that pulls a frame from our picture queue and displays it
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)
{
    // create SDL_Event of type FF_REFRESH_EVENT
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;

    // push the event to the events queue
    SDL_PushEvent(&event);

    // return 0 so SDL stops the timer so the callback is not made again
    return 0;
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay)
{
    // makes a callback to the user-specfied function after a certain number of
    // milliseconds
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_refresh_timer(void *userdata)
{
    VideoState *is = (VideoState *)userdata;

    if(is->video_st)
    {
        if(is->pictq_size == 0)
        {
            schedule_refresh(is, 1);
        }
        else
        {
            VideoPicture *vp = &is->pictq[is->pictq_rindex];
            // TODO timing code

            // 80 placeholder for timing value derived from frame timing later
            // e.g. 33.33 for 30fps
            schedule_refresh(is, 33);

            video_display(is);

            // :ReactivateWrap
            is->pictq_rindex = (is->pictq_rindex + 1) & (VIDEO_PICTURE_QUEUE_SIZE - 1);
            
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            // signal queue_picture that we have space for a new vp
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    }
    else
    {
        schedule_refresh(is, 100);
    }
}

int main(int argc, char *argv[])
{
    // Initalizing these to NULL prevents segfaults!
    // e.g. AVFrame           *pFrame = NULL;

    if(argc < 2) {
        fprintf(stderr, "Usage: tutorial04 <file>\n");
        exit(1);
    }

    VideoState      *is;
    // z postfix means the memory will be zero initialized
    is = av_mallocz(sizeof(VideoState));
    global_video_state = is;

    // initialize sdl and tell what features we're going to use
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    window_mutex = SDL_CreateMutex();

    // copy filename into VideoState
    av_strlcpy(is->filename, argv[1], sizeof(is->filename));

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

    SDL_DisplayMode display_mode;
    if(SDL_GetDesktopDisplayMode(0, &display_mode) != 0)
    {
        fprintf(stderr, "SDL_GetDesktopDisplayMode failed: %s", SDL_GetError());
        av_free(is);
        exit(1);
    }
    
    // !!!
    // NOTE: IMPORTANT You should not expect to be able to create a window,
    // render, or receive events on any thread other than the main one.
    // see: https://wiki.libsdl.org/CategoryThread
    // this lead to the renderer (which was not created on the main thread, while
    // the window was) breaking when the window was resized
    // !!!
    // SDL_Overlay is not used in SDL2 anymore render to a texture with the appropriate
    // format instead
    // Create window
    // important to create before starting decode_thread otherwise global window ptr
    // will not be valid
    window = SDL_CreateWindow(
            "FFmpeg Video", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            display_mode.w / 2, display_mode.h / 2,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if(window == NULL)
    {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        av_free(is);
        SDL_Quit();
        exit(1);
    }

    // window, index of rendering driver -1=first that meets reqs, flags
    is->renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if(is->renderer == NULL)
    {
        fprintf(stderr, "Could not create renderer! SDL_Error: %s\n", SDL_GetError());
        av_free(is);
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }

    // push FF_REFRESH_EVENT after 40ms
    schedule_refresh(is, 40);

    // start decoding/parsing thread on procedure decode_thread
    // is will be passed as user data
    is->parse_tid = SDL_CreateThread(decode_thread, "Decoding Thread", is);
    if(!is->parse_tid) {
        av_free(is);
        exit(1);
    }

    SDL_Event event;
    for(;;)
    {
        // wait indefinitely for the next available event
        SDL_WaitEvent(&event);
        switch(event.type)
        {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
            {
                // signal other threads to quit
                is->quit = 1;

                // wake up threads waiting on a CondVariable
                // so they can actually check the is->quit condition and stop
                // this is what kept us running even after closing the window
                // signal only wakes up 1 thread broadcast all waiting threads
                SDL_CondBroadcast(is->audioq.cond);
                SDL_CondBroadcast(is->videoq.cond);
                SDL_CondBroadcast(is->pictq_cond);
                
                SDL_DestroyTexture(is->texture);
                SDL_DestroyRenderer(is->renderer);
                SDL_DestroyWindow(window);
                SDL_Quit();

                // Close the codecs
                // avcodec_close: Close a given AVCodecContext and free all the data
                // associated with it (but not the AVCodecContext itself). 
                // Do not use this function. Use avcodec_free_context() to destroy a codec
                // context (either open or closed)
                avcodec_free_context(&is->video_ctx);
                avcodec_free_context(&is->audio_ctx);

                // Close the video file
                avformat_close_input(&is->pFormatCtx);
                av_free(is);
                exit(0);
            } break;

            case FF_REFRESH_EVENT:
            {
                video_refresh_timer(event.user.data1);
            } break;

            case SDL_WINDOWEVENT:
            {
                // NOTE: need to resize render target textures (SDL_TEXTUREACCESS_TARGET)
                // on window resize; we don't have any of those currently
                
                // event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                // RESIZED sent if changed by external (to SDL) event
                // this event is always preceded by SDL_WINDOWEVENT_SIZE_CHANGED
                if(event.window.event == SDL_WINDOWEVENT_RESIZED)
                {
                    // this event is only fired once after the resizing is done or rather
                    // the user stopped resizing/dragging the window(corner)
                    // whereas SDL_WINDOWEVENT_SIZE_CHANGED is fired alot even without
                    // resizing the window
                    // TODO while in the process of resizing the audio keeps playing but
                    // the video rendering stops; it's the same in ffplay.c
                    SDL_RenderPresent(is->renderer);
                }
            } break;

            default: break;
        }
    }

    return 0;
}
