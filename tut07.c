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

#include <libavutil/time.h>
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
#define VIDEO_PICTURE_QUEUE_SIZE 4

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

// 4 for AUDIO_F32SYS; 2 for AUDIO_S16SYS;
#define SDL_AUDIO_BYTES_PER_CHANNEL_SAMPLE 4

// max amount in percent we're willing to correct the audio size by
#define SAMPLE_CORRECTION_PERCENT_MAX 10
// threshold of weighted average at which we start correcting the audio sync
#define AUDIO_DIFF_AVG_NB 20

// https://www.programmersought.com/article/21844834744/
// There are generally three ways to solve the problem of audio and video synchronization:
// 1. Refer to an external clock to synchronize audio and video to this time
// 2. Based on the video, the audio time to synchronize the video
// 3. Based on audio, the time for video to synchronize audio  <-- usual method
//
// Because the human body is more sensitive to changes in audio, but not so
// sensitive to changes in the picture, the first and second methods need to
// constantly adjust the audio playback speed during the synchronization
// process, while the third method only needs to adjust The video playback
// speed, so in comparison, we generally choose the third method as the method
// of audio and video synchronization
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_AUDIO_MASTER
enum
{
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_MASTER,
};

// queued to signal receiving threads to flash internal ffmpeg buffers
AVPacket flush_pkt;


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
  double pts;
} VideoPicture;

typedef struct VideoState
{
    // File I/O Context
    AVFormatContext *pFormatCtx;

    int     av_sync_type;
    // seeking fields
    int     seek_req;
    int     seek_flags;
    int64_t seek_pos;

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
    // we're syncing the video to the audio clock -> video thread uses this value
    // to check if it needs to delay or speed up
    double audio_clock;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int    audio_diff_avg_count;

    // Video stream
    int video_stream_index;
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    struct SwsContext *video_sws_ctx;

    // pts of last decoded frame OR predicted pts of next decoded frame
    // already in seconds
    double video_clock;
    // last frame pts
    double video_current_pts;
    // last frame computer time in us when video_current_pts was set
    int64_t video_current_pts_time;
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;

    // sum of all calculated frame delays
    // -> time of when to display the next frame
    double frame_timer;
    double frame_last_delay;
    double frame_last_pts;

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

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt_next;

    SDL_LockMutex(q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt_next)
    {
        pkt_next = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }

    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;

    SDL_UnlockMutex(q->mutex);
}

double get_audio_clock(VideoState *is)
{
    // It's not as simple as getting the is->audio_clock value, though. Notice
    // that we set the audio PTS every time we process it, but if you look at
    // the audio_callback function, it takes time to move all the data from our
    // audio packet into our output buffer. That means that the value in our
    // audio clock could be too far ahead. So we have to check how much we have
    // left to write
    double pts = is->audio_clock;  // maintained in the audio thread

    int hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    int bytes_per_sec = 0;
    // hardcoded 2 bytes in original tutorial which depends on obtained SDL_AudioSpec
    int bytes_per_sample = is->audio_ctx->channels * SDL_AUDIO_BYTES_PER_CHANNEL_SAMPLE;
    if(is->audio_st)
    {
        bytes_per_sec = is->audio_ctx->sample_rate * bytes_per_sample;
    }
    if(bytes_per_sec)
    {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }
    return pts;
}

double get_video_clock(VideoState *is)
{
    // time since pts value was set
    double delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
    
    return is->video_current_pts + delta;
}

double get_external_clock()
{
    // av_gettime is in us
    return av_gettime() / 1000000.0;
}

// abstract clock so we can choose what clock we sync to
double get_master_clock(VideoState *is)
{
    switch(is->av_sync_type)
    {
        case AV_SYNC_VIDEO_MASTER:
            return get_video_clock(is);
        case AV_SYNC_AUDIO_MASTER:
            return get_audio_clock(is);
        case AV_SYNC_EXTERNAL_MASTER:
            return get_external_clock();
    }
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

/* Add or subtract samples to get a better sync, return new
   audio buffer size */
int synchronize_audio(VideoState *is, uint8_t *samples, int samples_size, double pts)
{
    // samples_size is IN BYTES!!

    // we don't want to sync every single time it's off because process audio a
    // lot more often than video packets
    // set a minimum number of consecutive calls to the synchronize_audio
    // function that have to be out of sync (> sync_threshold) before we bother doing anything

    int bytes_per_sample = SDL_AUDIO_BYTES_PER_CHANNEL_SAMPLE * is->audio_ctx->channels;

    if(is->av_sync_type != AV_SYNC_AUDIO_MASTER)
    {
        double ref_clock = get_master_clock(is);
        double diff = get_audio_clock(is) - ref_clock;

        if(fabs(diff) < AV_NOSYNC_THRESHOLD)
        {
            //  we've gotten N audio sample sets that have been out of sync.
            //  The amount we are out of sync can also vary a good deal, so
            //  we're going to take an average of how far each of those have
            //  been out of sync. E.g. the first call was out of sync by 40ms,
            //  the next by 50ms, and so on.
            //  But we're not going to take a simple average because the most
            //  recent values are more important. So we're going to use a
            //  fractional coefficient _c_, and sum the  differences like this:
            //  diff_sum = new_diff + diff_sum*c
            //  When we are ready to find the average difference, we simply calculate
            //  avg_diff = diff_sum * (1-c)
            
            // accumulate the diffs
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;

            if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
            {
                is->audio_diff_avg_count++;
            }
            else
            {
                double avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                // shrink/expand buffer
                // calculate how many samples we need to add or lop off
                if(fabs(avg_diff) >= is->audio_diff_threshold)
                {
                    // number of samples we want is going to be the number of
                    // samples we already have plus or minus the number of
                    // samples that correspond to the amount of time the audio
                    // has drifted
                    
                    // amount of bytes we're off by
                    int wanted_size = samples_size + ((int)(diff * is->audio_ctx->sample_rate) *
                            bytes_per_sample);

                    // set a limit on how big or small our correction can be
                    // because if we change our buffer too much, it'll be too
                    // jarring to the user
                    int min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    int max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);

                    if(wanted_size < min_size)
                    {
                        wanted_size = min_size;
                    } else if(wanted_size > max_size)
                    {
                        wanted_size = max_size;
                    }

                    // synchronize_audio function returns a sample size, which
                    // will then tell us how many bytes to send to the stream.
                    // So we just have to adjust the sample size to the
                    // wanted_size. This works for making the sample size
                    // smaller
                    // we can't just make the sample size larger because
                    // there's no more data in the buffer! So we have to add
                    // it. But what should we add? It would be foolish to try
                    // and extrapolate audio, so let's just use the audio we
                    // already have by padding out the buffer with the value of
                    // the last sample.

                    if(wanted_size < samples_size)
                    {
                        // remove samples
                        samples_size = wanted_size;
                    }
                    else if(wanted_size > samples_size)
                    {
                        // add samples by copying final samples
                        int bytes_to_copy = (samples_size - wanted_size);
                        // ptr to last sample in samples
                        uint8_t *samples_end = (uint8_t *)samples + samples_size - bytes_per_sample;
                        // ptr to one past last sample in samples
                        uint8_t *q = samples_end + bytes_per_sample;
                        // fill bytes_to_copy with last sample
                        while(bytes_to_copy > 0)
                        {
                            // dest, src, bytes to copy
                            memcpy(q, samples_end, bytes_per_sample);
                            q += bytes_per_sample;
                            bytes_to_copy -= bytes_per_sample;
                        }
                        samples_size = wanted_size;
                    }
                }
            }
        }
        else
        {
            /* difference is TOO big; reset diff stuff */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    // in bytes!! not in samples like erroneously stated in the tutorial
    return samples_size;
}

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr)
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

    // handle flush_pkt
    if(packet.data == flush_pkt.data)
    {
        avcodec_flush_buffers(is->audio_ctx);
        // output silence here or wait for a new packet?
        return -1;  // output silence
    }

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

        // setting pts_ptr for synching audio to video in audio_callback
        *pts_ptr = is->audio_clock;
#if 0
        // computing pts manually based on size, channels and sample rate
        int bytes_per_sample = is->audio_st->codecpar->channels * SDL_AUDIO_BYTES_PER_CHANNEL_SAMPLE;
        printf("Audio clock frame by pts %f\n", frame->pts * av_q2d(is->audio_st->time_base));
        is->audio_clock += (double)data_size /
            (double)(bytes_per_sample * is->audio_st->codecpar->sample_rate);
#else
        // using frame's pts provided by ffmpeg; can this be an invalid value?
        // time at which the audio frame should play or is done playing?
        is->audio_clock = frame->pts * av_q2d(is->audio_st->time_base);
#endif
        // printf("Audio clock frame %f\n", is->audio_clock);

        // update new total written
        offset += data_size;
    }

#if 0
    // udpate audio clock with pts from packet whene we're DONE with it
    // don't need this? since we always get all the frames in a packet directly anyway
    // the orignial tutorial might only get one frame if at all
    // NOTE: seems to work without this since the frame timing should be the same?
    // mb only needed when computing the pts of a frame manually?
    if(packet.pts != AV_NOPTS_VALUE)
    {
        // pts is in AVStream::time_base units
        is->audio_clock = av_q2d(is->audio_st->time_base)*packet.pts;
        // printf("Audio packet pts %d clock %f\n", packet.pts, is->audio_clock);
    }
#endif

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
            double pts;
            // ran out of data -> get new
            audio_size = audio_decode_frame(is, audio_buf, sizeof(is->audio_buf), &pts);
            if(audio_size < 0)
            {
                // output silence on err
                is->audio_buf_size = 1024;//len;
                memset(audio_buf, 0, is->audio_buf_size);
                fprintf(stderr, "audio_decode_frame() failed.\n");
            }
            else
            {
                audio_size = synchronize_audio(is, audio_buf, audio_size, pts);
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

int queue_picture(VideoState *is, const AVFrame *pFrame, double pts)
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
        // set presentation time stamp
        vp->pts = pts;

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

        // :ReactivateWrap
        // inc and wrap write index (using & instead of mod since we require
        // VIDEO_PICTURE_QUEUE_SIZE to be a power of 2
        is->pictq_windex = (is->pictq_windex + 1) & (VIDEO_PICTURE_QUEUE_SIZE - 1);

        // The queue works by adding onto it until it is full, and reading from
        // it as long as there is something on it. Therefore everything depends
        // upon the is->pictq_size value, requiring us to lock it.
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    else
    {
        return -1;
    }

    return 0;
}

/* static int64_t guess_correct_pts(AVCodecContext * ctx, int64_t reordered_pts, int64_t dts) */
/* { */
/*     int64_t pts = AV_NOPTS_VALUE; */

/*     if (dts != AV_NOPTS_VALUE) */
/*     { */
/*         ctx->pts_correction_num_faulty_dts += dts <= ctx->pts_correction_last_dts; */
/*         ctx->pts_correction_last_dts = dts; */
/*     } */
/*     else if (reordered_pts != AV_NOPTS_VALUE) */
/*     { */
/*         ctx->pts_correction_last_dts = reordered_pts; */
/*     } */

/*     if (reordered_pts != AV_NOPTS_VALUE) */
/*     { */
/*         ctx->pts_correction_num_faulty_pts += reordered_pts <= ctx->pts_correction_last_pts; */
/*         ctx->pts_correction_last_pts = reordered_pts; */
/*     } */
/*     else if (dts != AV_NOPTS_VALUE) */
/*     { */
/*         ctx->pts_correction_last_pts = dts; */
/*     } */

/*     if ((ctx->pts_correction_num_faulty_pts <= ctx->pts_correction_num_faulty_dts || dts == AV_NOPTS_VALUE) && reordered_pts != AV_NOPTS_VALUE) */
/*     { */
/*         pts = reordered_pts; */
/*     } */
/*     else */
/*     { */
/*         pts = dts; */
/*     } */

/*     return pts; */
/* } */

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

        // handle flush_pkt
        // Reset the internal decoder state / flush internal buffers.
        // Should be called e.g. when seeking or when switching to a different stream.
        if(packet.data == flush_pkt.data)
        {
            avcodec_flush_buffers(is->video_ctx);
            continue;
        }

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

            // pts is a timestamp that corresponds to a measurement of time in that stream's
            // time_base unit. For example, if a stream has 24 frames per second, a
            // PTS of 42 is going to indicate that the frame should go where the
            // 42nd frame would be if there we had a frame every 1/24 of a second
            // (certainly not necessarily true).
            // convert this value to seconds by dividing by the framerate. The
            // time_base value of the stream is going to be 1/framerate (for
            // fixed-fps content)
            double frame_pts = pFrame->pts * av_q2d(is->video_st->time_base);
            /* double frame_pts = guess_correct_pts(is->video_ctx, pFrame->pts, pFrame->pkt_dts); */
            /* // in case we get an undefined timestamp value */
            /* if (frame_pts == AV_NOPTS_VALUE) */
            /* { */
            /*     // set pts to the default value of 0 */
            /*     frame_pts = 0; */
            /* } */
            /* frame_pts *= av_q2d(is->video_st->time_base); */

            // begin synchronize_video(..)
            if(frame_pts != 0)
            {
                is->video_clock = frame_pts;
            }
            else
            {
                // use video clock if we don't get a pts
                frame_pts = is->video_clock;
            }

            double frame_delay = av_q2d(is->video_st->time_base);
            // account for repeating the same frame
            // ffmpeg docs: repeat_pic: When decoding, this signals how much the picture must be delayed.
            //              extra_delay = repeat_pict / (2*fps) 
            frame_delay += pFrame->repeat_pict * (frame_delay * 0.5);
            is->video_clock += frame_delay;
            // end synchronize_video(..)

            if(queue_picture(is, pFrame, frame_pts) < 0)
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
            // gettime is in us (microseconds)
            is->frame_timer = (double)av_gettime() / 1000000.0;
            is->frame_last_delay = 40e-3;
            is->video_current_pts_time = av_gettime();

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

        // seeking
        if(is->seek_req)
        {
            // timestamp is in time_base of the stream you pass av_seek_frame
            // passing -1 as stream index:
            // default stream is selected, and timestamp is automatically
            // converted from AV_TIME_BASE units to the stream specific time_base
            //
            // sometimes you can (rarely) run into problems with some files if
            // you pass av_seek_frame -1 for a stream, so we're going to pick
            // the first stream in our file
            // -> have to convert from AV_TIME_BASE to stream's time_base ourselves
            int stream_index= -1;
            int64_t seek_target = is->seek_pos;

            // check if we can satisfy the seek request first
            // seek_target still in AV_TIME_BASE, as is pFormatCtx->duration
            if((seek_target < 0) || (seek_target > pFormatCtx->duration))
            {
                // fprintf(stderr, "Seek would've went off the file bounds!\n", is->pFormatCtx->url);
                is->seek_req = 0;  // mark seek request as finsished
                continue;
            }
                

            if     (is->video_stream_index >= 0) stream_index = is->video_stream_index;
            else if(is->audio_stream_index >= 0) stream_index = is->audio_stream_index;

            if(stream_index >= 0)
            {
                // av_rescale_q(a,b,c) is a function that will rescale a timestamp from
                // one base to another. It basically computes a*b/c but this function is
                // required because that calculation could overflow
                // AVRational AV_TIME_BASE_Q is the fractional version of AV_TIME_BASE
                // AV_TIME_BASE * time_in_seconds = avcodec_timestamp
                // AV_TIME_BASE_Q * avcodec_timestamp = time_in_seconds
                seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                        pFormatCtx->streams[stream_index]->time_base);
            }

            if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, is->seek_flags) < 0)
            {
                fprintf(stderr, "%s: error while seeking\n", is->pFormatCtx->url);
            }
            else
            {
                // need to remove all queued packets from the q / flush the q
                // each thread (video/audio) needs to flush internal ffmpeg buffers
                if(is->audio_stream_index >= 0)
                {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if(is->video_stream_index >= 0)
                {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
            }

            // seek request done
            is->seek_req = 0;
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

    // TODO set renderer scaling method that does a better job than the current one
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

            // for the video clock can't just update with the frame's pts, since frames
            // can be pretty long (compared to audio and in case of repeated frames)
            // use different time instead:
            // PTS_of_last_frame + (current_time - time_elapsed_since_PTS_value_was_set)
            is->video_current_pts = vp->pts;
            is->video_current_pts_time = av_gettime();

            // adjust value for next refresh based on being ahead or behind the audio_clock
            // refresh as fast as possible if behind else increase/double delay

            printf("Current Frame PTS:\t\t%f\n", vp->pts);
            printf("Last Frame PTS:\t\t\t%f\n", is->frame_last_pts);

            // make sure that the delay between the PTS and the previous PTS make sense
            double delay = vp->pts - is->frame_last_pts;
            printf("PTS-Delay %f\n", delay);
            if(delay <= 0 || delay >= 1)
            {
                // invalid delay, use previous as a guess
                delay = is->frame_last_delay;
            }
            else
            {
                is->frame_last_delay = delay;
            }
            is->frame_last_pts = vp->pts;
            printf("Corrected PTS-Delay %f\n", delay);

            if(is->av_sync_type != AV_SYNC_VIDEO_MASTER)
            {
                // update delay to sync to audio
                double ref_clock = get_audio_clock(is);
                printf("Audio Ref Clock:\t\t%f\n", ref_clock);
                // pts is when we should display the frame
                // so diff > 0: frame should not be displayed yet (within a threshold)
                // so diff < 0: frame should've been displayed already (within a threshold)
                double diff = vp->pts - ref_clock;
                printf("Diff %f\n", diff);

                // Skip or repeat the frame. Take delay into account
                // synch threshold since things are never going to be perfectly in synch
                // ffplay uses 0.01
                double sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
                printf("Sync Threshold:\t\t\t%f\n", sync_threshold);
                // make sure that the synch threshold is never smaller than the
                // gaps in between PTS values
                if(fabs(diff) < AV_NOSYNC_THRESHOLD)
                {
                    if(diff <= -sync_threshold)
                    {
                        // pts < ref_clock -> show as fast as possible since
                        // the frame should've already been displayed
                        delay = 0;
                    } else if(diff >= sync_threshold)
                    {
                        // pts > ref_clock -> increase delay
                        delay = 2 * delay;
                    }
                }
                printf("Corrected PTS delay:\t%f\n", delay);
#if 0
                // NOTE: only works with AV_SYNC_AUDIO_MASTER
                // NOTE: syncing to the computer's clock is not needed
                // it works (or at least seems to) just as well to just sync based on
                // the audio clock; probably done that way to ease the transition to syncing
                // both audio and video to the external clock
                if(delay < 0.010)
                    delay = 0.010;
                printf("Limit PTS delay:\t%f\n", delay);
                schedule_refresh(is, (int)(delay * 1000 + 0.5));
                printf("Next refresh: %d\n\n", (int)(delay * 1000 + 0.5));

            }
#else
            }
            // frame timer will sum up all of our calculated delays
            // -> what time it should be when we display the next frame
            // add the new delay to the frame timer, compare it to the time on
            // our computer's clock, and use that value to schedule the next refresh
            is->frame_timer += delay;

            // floating point literal wihout suffix -> double
            // f suffix -> float; l suffix -> long double
            double actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            printf("Actual delay %f\n", actual_delay);
            // minimum refresh value 10 milliseconds
            if(actual_delay < 0.010)
            {
                // Really it should skip the picture instead
                actual_delay = 0.010;
            }
            printf("Corrected Actual delay %f\n", actual_delay);
            // * 1000 to convert to milliseconds; +.5 to get rounding when truncating to int
            // schedule next frame
            printf("Next refresh: %d\n\n", (int)(actual_delay * 1000 + 0.5));
            schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));
#endif

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

void stream_seek(VideoState *is, int64_t pos, int rel)
{
    // do we already have a seek request?
    if(!is->seek_req)
    {
        is->seek_pos = pos;
        is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
        is->seek_req = 1;
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
    is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
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

    // init flush_pkt
    av_init_packet(&flush_pkt);
    flush_pkt.data = "FLUSH";

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

            case SDL_KEYDOWN:
            {
                double incr, pos;
                // SDL_KeyboardEvent is a member of the SDL_Event union and is
                // used when an event of type SDL_KEYDOWN or SDL_KEYUP is
                // reported. You would access it through the event's key field. 
                // SDL_Keysym keysym inside SDL_KeyboardEvent
                // SDL_KeyCode sym inside SDL_Keysym
                switch(event.key.keysym.sym)
                {
                    case SDLK_LEFT:
                        incr = -10.0;
                        goto do_seek;
                    case SDLK_RIGHT:
                        incr =  10.0;
                        goto do_seek;
                    case SDLK_UP:
                        incr =  60.0;
                        goto do_seek;
                    case SDLK_DOWN:
                        incr = -60.0;
                        goto do_seek;

                    do_seek:
                        if(global_video_state)
                        {
                            pos = get_master_clock(global_video_state);
                            pos += incr;
                            // AV_TIME_BASE: convert our new time to avcodec's internal timestamp unit
                            // timestamps in streams are measured in frames
                            // rather than seconds, with the formula
                            // seconds = frames * time_base (fps)
                            // avcodec defaults to a value of 1,000,000 fps
                            stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
                        }
                        break;

                    default: break;
                }
            } break;

            default: break;
        }
    }

    return 0;
}
