#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <stdio.h>

int main(int argc, char *argv[]) {
    AVFormatContext *input_ctx = NULL, *output_ctx = NULL;
    int ret;

    // Input and output file paths
    const char *input_file = "input.mp4";
    const char *output_file = "output.mp4";
    double trim_duration = 10.0; // Trim to 10 seconds

    // Initialize FFmpeg
    av_register_all();

    // Open input file
    if ((ret = avformat_open_input(&input_ctx, input_file, NULL, NULL)) < 0) {
        fprintf(stderr, "Could not open input file: %s\n", av_err2str(ret));
        return 1;
    }

    // Retrieve stream information
    if ((ret = avformat_find_stream_info(input_ctx, NULL)) < 0) {
        fprintf(stderr, "Could not find stream info: %s\n", av_err2str(ret));
        goto end;
    }

    // Create output context
    if ((ret = avformat_alloc_output_context2(&output_ctx, NULL, NULL, output_file)) < 0) {
        fprintf(stderr, "Could not create output context: %s\n", av_err2str(ret));
        goto end;
    }

    // Copy streams to output
    for (int i = 0; i < input_ctx->nb_streams; i++) {
        AVStream *in_stream = input_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(output_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed to allocate output stream\n");
            goto end;
        }
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        out_stream->codecpar->codec_tag = 0;
    }

    // Open output file
    if (!(output_ctx->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&output_ctx->pb, output_file, AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "Could not open output file: %s\n", av_err2str(ret));
            goto end;
        }
    }

    // Write header
    if ((ret = avformat_write_header(output_ctx, NULL)) < 0) {
        fprintf(stderr, "Error writing header: %s\n", av_err2str(ret));
        goto end;
    }

    // Process packets
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while (av_read_frame(input_ctx, &pkt) >= 0) {
        AVStream *in_stream = input_ctx->streams[pkt.stream_index];
        AVStream *out_stream = output_ctx->streams[pkt.stream_index];

        // Convert timestamps
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        // Stop after trim_duration
        if (av_q2d(in_stream->time_base) * pkt.pts > trim_duration) {
            av_packet_unref(&pkt);
            break;
        }

        // Write packet
        if ((ret = av_interleaved_write_frame(output_ctx, &pkt)) < 0) {
            fprintf(stderr, "Error writing packet: %s\n", av_err2str(ret));
            break;
        }
        av_packet_unref(&pkt);
    }

    // Write trailer
    av_write_trailer(output_ctx);

    printf("Video trimmed and saved as %s\n", output_file);

end:
    if (input_ctx)
        avformat_close_input(&input_ctx);
    if (output_ctx && !(output_ctx->flags & AVFMT_NOFILE))
        avio_closep(&output_ctx->pb);
    if (output_ctx)
        avformat_free_context(output_ctx);

    return ret < 0 ? 1 : 0;
}
