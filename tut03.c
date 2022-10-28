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

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

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

// audio PacketQueue instance
PacketQueue audioq;

// global quit flag
int quit = 0;

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
        if(quit)
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
    if (quit)
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

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size)
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
    if(packet_queue_get(&audioq, &packet, 1) < 0)
        return -1;
    /* audio_pkt_data = packet.data; */
    /* audio_pkt_size = packet.size; */

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
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len_to_write, audio_size;

    // The size of audio_buf is 1.5 times the size of the largest audio frame
    // that FFmpeg will give us, which gives us a nice cushion.
    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    // callback must completely initialize the buffer; as of SDL 2.0, this
    // buffer is not initialized before the callback is called. If there is
    // nothing to play, the callback should fill the buffer with silence
    while(len > 0)
    {
        if(quit)
            return;

        if(audio_buf_index >= audio_buf_size)
        {
            // ran out of data -> get new
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if(audio_size < 0)
            {
                // output silence on err
                audio_buf_size = 1024;//len;
                memset(audio_buf, 0, audio_buf_size);
                fprintf(stderr, "audio_decode_frame() failed.\n");
            }
            else
            {
                audio_buf_size = audio_size;
            }

            audio_buf_index = 0;
        }

        len_to_write = audio_buf_size - audio_buf_index;
        // only write sound to end of buffer
        if(len_to_write > len)
            len_to_write = len;

        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len_to_write);

        // update remaining size of audio buffer by amount written
        len -= len_to_write;
        // advance ptr
        stream += len_to_write;
        audio_buf_index += len_to_write;
    }
}

int main(int argc, char *argv[])
{
    // Initalizing these to NULL prevents segfaults!
    // e.g. AVFrame           *pFrame = NULL;

    if(argc < 2) {
        fprintf(stderr, "Usage: tut03 <file>\n");
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
    AVCodecParameters *aCodecParameters =  NULL;
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
            pCodecParameters = testCodecParameters;
        } else if ((audio_stream_index == -1) &&
                   (testCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO)) {
            audio_stream_index = i;
            aCodecParameters = testCodecParameters;
        }
    }
    if((video_stream_index == -1) || (audio_stream_index == -1))
    {
        fprintf(stderr, "Could not find video and/or audio stream!\n");
        exit(1); // Didn't find a video stream
    }

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

    //
    // Audio Codec
    //
    AVCodec *aCodec = NULL;
    aCodec = avcodec_find_decoder(aCodecParameters->codec_id);
    if(aCodec == NULL)
    {
        fprintf(stderr, "ERROR: Unsupported audio codec!");
        exit(1); // Unsupported codec
    }

    // Copy context params
    AVCodecContext *aCodecCtx = avcodec_alloc_context3(aCodec);
    if(!aCodecCtx)
        exit(1); // failed to alloc

    // used codec params since avcodec_copy_context is deprecated
    if(avcodec_parameters_to_context(aCodecCtx, aCodecParameters) < 0) {
        fprintf(stderr, "Couldn't copy codec params to codec context");
        exit(1); // Error copying codec params to context
    }

    // Open codec
    if(avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
        exit(1); // Could not open codec
    // -- END Audio Codec find+copy

    // SDL audio setup
    // we can ask for a spec but we might not get it
    SDL_AudioSpec wanted_spec, obtained_spec;

    wanted_spec.freq = aCodecCtx->sample_rate;
    // signed 16-bit samples in native byte order
    // we asked for AUDIO_S16SYS but on my system we get AUDIO_F32SYS
    wanted_spec.format = AUDIO_F32SYS;//AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
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
    wanted_spec.userdata = aCodecCtx;

    // use SDL_OpenAudioDevice instead of SDL_OpenAudio
    // uint32
    SDL_AudioDeviceID audio_device_id = SDL_OpenAudioDevice(
            NULL, 0,
            &wanted_spec, &obtained_spec,
            SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (audio_device_id == 0)
    {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        exit(1);
    }
    // assert we got the output format we wanted otherwise our conversion in
    // audio_decode_frame will be wrong
    assert(wanted_spec.format == obtained_spec.format);

    // initialize the audio AVCodecContext to use the given audio AVCodec
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
    {
        fprintf(stderr, "Could not open audio codec.\n");
        return -1;
    }

    // init audio PacketQueue
    packet_queue_init(&audioq);

    // start playing audio on the given audio device
    SDL_PauseAudioDevice(audio_device_id, 0);


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
                /* printf( */
                /*         "Frame %c (%d) pts %d dts %d key_frame %d " */
                /*         "[coded_picture_number %d, display_picture_number %d, %dx%d]\n", */
                /*         av_get_picture_type_char(pFrame->pict_type), */
                /*         pCodecCtx->frame_number, */
                /*         pFrame->pts, */
                /*         pFrame->pkt_dts, */
                /*         pFrame->key_frame, */
                /*         pFrame->coded_picture_number, */
                /*         pFrame->display_picture_number, */
                /*         pCodecCtx->width, */
                /*         pCodecCtx->height */
                /*       ); */

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

            // Free the packet that was allocated by av_read_frame
            av_packet_unref(&packet);
        }
        else if(packet.stream_index == audio_stream_index)
        {
            // put the AVPacket in the audio PacketQueue
            packet_queue_put(&audioq, &packet);
            // packet not freed when we q it but later when we decode it
        }
        else
        {
            // wipe the packet
            av_packet_unref(&packet);
        }

        // use SDL event system to check if window should be closed
        SDL_PollEvent(&event);
        switch(event.type)
        {
            case SDL_QUIT:
            {
                // signal other threads to quit
                quit = 1;
                SDL_DestroyTexture(texture);
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(window);
                SDL_Quit();
                exit(0);
            } break;
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
