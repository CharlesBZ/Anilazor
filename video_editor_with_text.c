#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 400

// Function prototype
int process_video(const char *input_file, const char *output_file, double trim_duration, 
                  const char *text, const char *filter_preset, int width, int height);

// Process video function (unchanged from previous version)
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

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Video Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16); // Adjust font path
    if (!window || !renderer || !font) {
        fprintf(stderr, "SDL/TTF setup failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    char input_file[256] = "input.mp4";
    char output_file[256] = "output.mp4";
    char text[256] = "Hello World";
    char trim_str[32] = "10.0";
    char filter_str[32] = "None";
    char res_str[32] = "1080p";
    int filter_choice = 1, res_choice = 1;
    int processing = 0;

    SDL_Event event;
    int quit = 0;
    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit = 1;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN && !processing) {
                double trim_duration = atof(trim_str);
                const char *filter_preset = (filter_choice == 1) ? "null" : 
                                            (filter_choice == 2) ? "eq=brightness=0.1" : 
                                            "colorchannelmixer=.393:.769:.189:0:.349:.686:.168:0:.272:.534:.131";
                int width = (res_choice == 1) ? 1920 : 1280;
                int height = (res_choice == 1) ? 1080 : 720;
                processing = 1;
                if (process_video(input_file, output_file, trim_duration, text, filter_preset, width, height) == 0) {
                    strcpy(res_str, "Done!");
                } else {
                    strcpy(res_str, "Error");
                }
                processing = 0;
            }
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderClear(renderer);

        SDL_Color color = {0, 0, 0, 255};
        char display_text[512];
        snprintf(display_text, sizeof(display_text), "Input File: %s", input_file);
        SDL_Surface *surface = TTF_RenderText_Solid(font, display_text, color);
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_Rect rect = {10, 10, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_FreeSurface(surface);
        SDL_DestroyTexture(texture);

        snprintf(display_text, sizeof(display_text), "Output File: %s", output_file);
        surface = TTF_RenderText_Solid(font, display_text, color);
        texture = SDL_CreateTextureFromSurface(renderer, surface);
        rect.y = 40;
        rect.w = surface->w;
        rect.h = surface->h;
        SDL_RenderCopy(renderer, texture, NULL, &rect);
        SDL_FreeSurface(surface);
        SDL_DestroyTexture(texture);

        // Add more fields as needed...

        SDL_RenderPresent(renderer);
    }

    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}