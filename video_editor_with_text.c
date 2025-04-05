#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define PREVIEW_WIDTH 320
#define PREVIEW_HEIGHT 240

typedef struct {
    char input_file[256];
    char output_file[256];
    char trim_str[32];
    char text[256];
    char filter_str[32];
    char res_str[32];
    int active_field; // 0-5 for each field
} EditorState;

int process_video(const char *input_file, const char *output_file, double trim_duration, 
                  const char *text, const char *filter_preset, int width, int height) {
    AVFormatContext *input_fmt_ctx = NULL, *output_fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL, *enc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL, *buffersrc_ctx = NULL;
    AVFilterGraph *filter_graph = NULL;
    AVPacket packet;
    AVFrame *frame = NULL, *filt_frame = NULL;
    int video_stream_index = -1;
    int ret;

    char filter_descr[512];
    snprintf(filter_descr, sizeof(filter_descr), 
             "%s,drawtext=text='%s':fontcolor=white:fontsize=24:x=(w-tw)/2:y=(h-th)/2", 
             filter_preset, text);

    const AVInputFormat *input_format = av_find_input_format("mp4");
    if ((ret = avformat_open_input(&input_fmt_ctx, input_file, input_format, NULL)) < 0) {
        fprintf(stderr, "Could not open input file: %s\n", av_err2str(ret));
        goto end;
    }
    if ((ret = avformat_find_stream_info(input_fmt_ctx, NULL)) < 0) goto end;

    video_stream_index = av_find_best_stream(input_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_index < 0) goto end;

    AVStream *in_stream = input_fmt_ctx->streams[video_stream_index];
    const AVCodec *decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0) goto end;

    if ((ret = avformat_alloc_output_context2(&output_fmt_ctx, NULL, NULL, output_file)) < 0) goto end;

    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVStream *out_stream = avformat_new_stream(output_fmt_ctx, encoder);
    enc_ctx = avcodec_alloc_context3(encoder);
    enc_ctx->height = height;
    enc_ctx->width = width;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = (AVRational){1, 25};
    out_stream->time_base = enc_ctx->time_base;

    if ((ret = avcodec_open2(enc_ctx, encoder, NULL)) < 0) goto end;
    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    if (!(output_fmt_ctx->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE)) < 0) goto end;
    }
    if ((ret = avformat_write_header(output_fmt_ctx, NULL)) < 0) goto end;

    filter_graph = avfilter_graph_alloc();
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    char args[512];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d",
             dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
             in_stream->time_base.num, in_stream->time_base.den);

    if ((ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph)) < 0) goto end;
    if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph)) < 0) goto end;

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL)) < 0) goto end;
    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0) goto end;

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!frame || !filt_frame) goto end;

    while (1) {
        if ((ret = av_read_frame(input_fmt_ctx, &packet)) < 0) break;
        if (packet.stream_index != video_stream_index) {
            av_packet_unref(&packet);
            continue;
        }

        if ((ret = avcodec_send_packet(dec_ctx, &packet)) < 0) break;
        while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
            double pts_time = frame->pts * av_q2d(in_stream->time_base);
            if (pts_time > trim_duration) {
                ret = AVERROR_EOF;
                break;
            }

            if ((ret = av_buffersrc_add_frame(buffersrc_ctx, frame)) < 0) break;
            while ((ret = av_buffersink_get_frame(buffersink_ctx, filt_frame)) >= 0) {
                filt_frame->pts = av_rescale_q(filt_frame->pts, buffersink_ctx->inputs[0]->time_base, out_stream->time_base);
                if ((ret = avcodec_send_frame(enc_ctx, filt_frame)) < 0) break;
                while ((ret = avcodec_receive_packet(enc_ctx, &packet)) >= 0) {
                    packet.stream_index = 0;
                    av_packet_rescale_ts(&packet, enc_ctx->time_base, out_stream->time_base);
                    if ((ret = av_interleaved_write_frame(output_fmt_ctx, &packet)) < 0) break;
                    av_packet_unref(&packet);
                }
                av_frame_unref(filt_frame);
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ret = 0;
            av_frame_unref(frame);
        }
        av_packet_unref(&packet);
    }

    avcodec_send_frame(enc_ctx, NULL);
    while (avcodec_receive_packet(enc_ctx, &packet) >= 0) {
        packet.stream_index = 0;
        av_packet_rescale_ts(&packet, enc_ctx->time_base, out_stream->time_base);
        av_interleaved_write_frame(output_fmt_ctx, &packet);
        av_packet_unref(&packet);
    }

    av_write_trailer(output_fmt_ctx);
    printf("Video processing complete. Saved as %s\n", output_file);

end:
    if (input_fmt_ctx) avformat_close_input(&input_fmt_ctx);
    if (output_fmt_ctx && !(output_fmt_ctx->flags & AVFMT_NOFILE)) avio_closep(&output_fmt_ctx->pb);
    if (output_fmt_ctx) avformat_free_context(output_fmt_ctx);
    if (dec_ctx) avcodec_free_context(&dec_ctx);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (frame) av_frame_free(&frame);
    if (filt_frame) av_frame_free(&filt_frame);
    if (filter_graph) avfilter_graph_free(&filter_graph);
    if (outputs) avfilter_inout_free(&outputs);
    if (inputs) avfilter_inout_free(&inputs);
    return ret < 0 ? -1 : 0;
}

void render_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    SDL_Surface *surface = TTF_RenderText_Solid(font, text, color);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect rect = {x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, NULL, &rect);
    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) {
        fprintf(stderr, "Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Video Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
    if (!window || !renderer || !font) {
        fprintf(stderr, "Setup failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    EditorState state = {
        .input_file = "input.mp4",
        .output_file = "output.mp4",
        .trim_str = "10.0",
        .text = "Hello World",
        .filter_str = "None",
        .res_str = "1080p",
        .active_field = -1
    };

    // Preview setup
    AVFormatContext *preview_ctx = NULL;
    AVCodecContext *preview_dec_ctx = NULL;
    struct SwsContext *sws_ctx = NULL;
    SDL_Texture *preview_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, PREVIEW_WIDTH, PREVIEW_HEIGHT);
    int video_stream_index = -1;
    AVPacket packet;
    AVFrame *frame = av_frame_alloc();

    if (avformat_open_input(&preview_ctx, state.input_file, NULL, NULL) >= 0 &&
        avformat_find_stream_info(preview_ctx, NULL) >= 0) {
        video_stream_index = av_find_best_stream(preview_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (video_stream_index >= 0) {
            AVStream *stream = preview_ctx->streams[video_stream_index];
            const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
            preview_dec_ctx = avcodec_alloc_context3(decoder);
            avcodec_parameters_to_context(preview_dec_ctx, stream->codecpar);
            avcodec_open2(preview_dec_ctx, decoder, NULL);
            sws_ctx = sws_getContext(preview_dec_ctx->width, preview_dec_ctx->height, preview_dec_ctx->pix_fmt,
                                     PREVIEW_WIDTH, PREVIEW_HEIGHT, AV_PIX_FMT_YUY2, SWS_BILINEAR, NULL, NULL, NULL);
        }
    }

    SDL_Event event;
    int quit = 0, processing = 0;
    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit = 1;
            if (event.type == SDL_MOUSEBUTTONDOWN) {
                int x = event.button.x, y = event.button.y;
                if (y >= 10 && y < 30) state.active_field = 0; // Input file
                else if (y >= 40 && y < 60) state.active_field = 1; // Output file
                else if (y >= 70 && y < 90) state.active_field = 2; // Trim duration
                else if (y >= 100 && y < 120) state.active_field = 3; // Text
                else if (y >= 130 && y < 150) state.active_field = 4; // Filter
                else if (y >= 160 && y < 180) state.active_field = 5; // Resolution
                else if (x >= 500 && x < 580 && y >= 500 && y < 540) { // Process button
                    if (!processing) {
                        double trim_duration = atof(state.trim_str);
                        const char *filter_preset = strcmp(state.filter_str, "Brighten") == 0 ? "eq=brightness=0.1" :
                                                    strcmp(state.filter_str, "Sepia") == 0 ? "colorchannelmixer=.393:.769:.189:0:.349:.686:.168:0:.272:.534:.131" : "null";
                        int width = strcmp(state.res_str, "1080p") == 0 ? 1920 : 1280;
                        int height = strcmp(state.res_str, "1080p") == 0 ? 1080 : 720;
                        processing = 1;
                        process_video(state.input_file, state.output_file, trim_duration, state.text, filter_preset, width, height);
                        processing = 0;
                    }
                }
            }
            if (event.type == SDL_TEXTINPUT && state.active_field >= 0) {
                char *target = state.active_field == 0 ? state.input_file :
                               state.active_field == 1 ? state.output_file :
                               state.active_field == 2 ? state.trim_str :
                               state.active_field == 3 ? state.text :
                               state.active_field == 4 ? state.filter_str : state.res_str;
                strncat(target, event.text.text, sizeof(state.input_file) - strlen(target) - 1);
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_BACKSPACE && state.active_field >= 0) {
                char *target = state.active_field == 0 ? state.input_file :
                               state.active_field == 1 ? state.output_file :
                               state.active_field == 2 ? state.trim_str :
                               state.active_field == 3 ? state.text :
                               state.active_field == 4 ? state.filter_str : state.res_str;
                target[strlen(target) - 1] = '\0';
            }
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderClear(renderer);

        SDL_Color color = {0, 0, 0, 255};
        render_text(renderer, font, "Input File: ", 10, 10, color);
        render_text(renderer, font, state.input_file, 100, 10, color);
        render_text(renderer, font, "Output File: ", 10, 40, color);
        render_text(renderer, font, state.output_file, 100, 40, color);
        render_text(renderer, font, "Trim Duration: ", 10, 70, color);
        render_text(renderer, font, state.trim_str, 100, 70, color);
        render_text(renderer, font, "Text: ", 10, 100, color);
        render_text(renderer, font, state.text, 100, 100, color);
        render_text(renderer, font, "Filter: ", 10, 130, color);
        render_text(renderer, font, state.filter_str, 100, 130, color);
        render_text(renderer, font, "Resolution: ", 10, 160, color);
        render_text(renderer, font, state.res_str, 100, 160, color);

        SDL_Rect button = {500, 500, 80, 40};
        SDL_SetRenderDrawColor(renderer, 0, 128, 0, 255);
        SDL_RenderFillRect(renderer, &button);
        render_text(renderer, font, "Process", 510, 510, color);

        // Preview rendering
        if (preview_dec_ctx && av_read_frame(preview_ctx, &packet) >= 0) {
            if (packet.stream_index == video_stream_index) {
                avcodec_send_packet(preview_dec_ctx, &packet);
                if (avcodec_receive_frame(preview_dec_ctx, frame) >= 0) {
                    uint8_t *data[4];
                    int linesize[4];
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, preview_dec_ctx->height, data, linesize);
                    SDL_UpdateTexture(preview_texture, NULL, data[0], linesize[0]);
                    SDL_Rect preview_rect = {WINDOW_WIDTH - PREVIEW_WIDTH - 10, 10, PREVIEW_WIDTH, PREVIEW_HEIGHT};
                    SDL_RenderCopy(renderer, preview_texture, NULL, &preview_rect);
                }
            }
            av_packet_unref(&packet);
        }

        SDL_RenderPresent(renderer);
    }

    if (preview_ctx) avformat_close_input(&preview_ctx);
    if (preview_dec_ctx) avcodec_free_context(&preview_dec_ctx);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (frame) av_frame_free(&frame);
    SDL_DestroyTexture(preview_texture);
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
