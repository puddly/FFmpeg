/*
 * Hichip security camera format demuxer
 * Copyright (c) 2020 puddly
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "internal.h"


enum FrameHeader
{
    START_FRAME = MKTAG('H', 'X', 'V', 'S'),

    VIDEO_FRAME = MKTAG('H', 'X', 'V', 'F'),
    AUDIO_FRAME = MKTAG('H', 'X', 'A', 'F'),

    INDEX_FRAME = MKTAG('H', 'X', 'F', 'I')
};


typedef struct HXVFDemuxContext
{
    int64_t video_start_timestamp;
    int64_t last_video_timestamp;
    int audio_frames_since_last_video_frame;

    uint32_t stream_duration;
    uint32_t index_size;
} HXVFDemuxContext;


static int hxvf_probe(const AVProbeData *p)
{
    if (p->buf_size < 20)
        return 0;

    if (AV_RL32(p->buf) != START_FRAME) {
        return 0;
    }

    /* Make sure that a valid audio or video frame exists after it */
    uint32_t first_frame = AV_RL32(p->buf + 16);

    if ((first_frame != AUDIO_FRAME) && (first_frame != VIDEO_FRAME))
        return 0;

    return AVPROBE_SCORE_MAX;
}


static int parse_frame_index(AVFormatContext *s, HXVFDemuxContext *ctx) {
    if (!s->pb->seekable & AVIO_SEEKABLE_NORMAL)
        return -1;

    if (avio_size(s->pb) < 16 + 16 + 200000)
        return -1;

    if (avio_seek(s->pb, avio_size(s->pb) - 200016, SEEK_SET) < 0)
        return -1;

    if (avio_rl32(s->pb) != INDEX_FRAME)
        return -1;

    int64_t size = avio_size(s->pb);

    /* There should be enough data to contain the header */
    if (size - avio_tell(s->pb) < 12)
        return -1;

    uint32_t index_size = avio_rl32(s->pb);
    if (index_size % 16 != 0)
        return -1;

    uint32_t stream_duration = avio_rl32(s->pb);  /* in 1/1000s */
    avio_skip(s->pb, 4);  /* unknown */

    if (size - avio_tell(s->pb) < index_size)
        return -1;

    ctx->index_size = index_size;
    ctx->stream_duration = stream_duration;

    return 0;
}


static int hxvf_read_header(AVFormatContext *s)
{
    HXVFDemuxContext *ctx = s->priv_data;
    ctx->video_start_timestamp = AV_NOPTS_VALUE;
    ctx->audio_frames_since_last_video_frame = 0;
    ctx->last_video_timestamp = 0;

    AVStream *video = avformat_new_stream(s, NULL);
    if (!video)
        return AVERROR(ENOMEM);

    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = AV_CODEC_ID_H264;
    video->need_parsing = AVSTREAM_PARSE_FULL_RAW;
    video->start_time = 0;

    avpriv_set_pts_info(video, 32, 1, 1000);

    /* There is no way to know if a stream contains audio without checking that no audio
       frames have been seen for 20ms. This can be hundreds of KiB into the file for
       low-resolution video. */
    AVStream *audio = avformat_new_stream(s, NULL);
    if (!audio)
        return AVERROR(ENOMEM);

    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_ALAW;
    audio->codecpar->channel_layout = AV_CH_LAYOUT_MONO;
    audio->codecpar->channels = 1;
    audio->codecpar->sample_rate = 8000;
    audio->codecpar->frame_size = 160;
    audio->start_time = 0;

    avpriv_set_pts_info(audio, 32, 1, 1000);


    /* Try parsing the frame index (for some reason) */
    int64_t old_pos = avio_tell(s->pb);

    if (parse_frame_index(s, ctx) < 0) {
        av_log(s, AV_LOG_WARNING, "Input is not seekable or corrupted frame index\n");
    } else {
        video->duration = ctx->stream_duration;
        audio->duration = ctx->stream_duration;

        for (uint64_t i = 0; i < ctx->index_size; i += 16) {
            uint32_t file_offset = avio_rl32(s->pb);

            if (file_offset == 0)
                break;

            uint32_t frame_timestamp = avio_rl32(s->pb);

            av_log(s, AV_LOG_DEBUG, "Timestamp %0.4f is at offset %d\n",
                frame_timestamp / 1000.0,
                file_offset);
        }
    }

    /* Reset our position in the file after we try to read the frame index */
    if (avio_tell(s->pb) != old_pos)
        avio_seek(s->pb, old_pos, SEEK_SET);

    return 0;
}


static int hxvf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    HXVFDemuxContext *ctx = s->priv_data;

    while (1) {
        int stream_index;
        uint64_t pos = avio_tell(s->pb);

        /* All frames start with a four byte ASCII header: HX?? */
        uint32_t frame_type = avio_rl32(s->pb);

        /* This is the only time we have to make sure we haven't reached EOF */
        if (avio_feof(s->pb)) {
            return AVERROR_EOF;
        }

        switch (frame_type) {
            /* The start frame has a fixed size and can be skipped */
            case START_FRAME:
                av_log(s, AV_LOG_VERBOSE, "new file started at offset %lld\n", pos);
                avio_skip(s->pb, 12);

                continue;

            /* Video frames go to the first stream */
            case VIDEO_FRAME:
                stream_index = 0;
                break;

            /* Audio frames go to the second stream */
            case AUDIO_FRAME:
                stream_index = 1;
                break;

            /* At the very end of the file is a frame index */
            case INDEX_FRAME:
                break;

            /* No other frame types are known to exist */
            default:
                av_log(s, AV_LOG_ERROR, "unexpected frame type at offset %lld: 0x%08x\n",
                    avio_tell(s->pb), frame_type);
                return AVERROR_INVALIDDATA;
        }

        /* Every frame but the start frame has a variable size */
        uint32_t size = avio_rl32(s->pb);

        /* The index frame has two 32-bit fields after the size but they are ignored */
        if (frame_type == INDEX_FRAME) {
            avio_skip(s->pb, 8 + size);
            continue;
        }

        /* Audio and video frames contain a timestamp and a subtype */
        uint32_t timestamp = avio_rl32(s->pb);

        /* Subtype is not used but it's 0 for audio, 1, 2 for video */
        avio_skip(s->pb, 4);

        if (ctx->video_start_timestamp == AV_NOPTS_VALUE) {
            ctx->video_start_timestamp = timestamp;
            av_log(s, AV_LOG_VERBOSE, "video stream start time is %u\n", timestamp);
        }

        if (frame_type == AUDIO_FRAME) {
            /* The audio stream within these files is not as simple as it seems:

               1. The audio timestamps just increase by 20ms every frame but 
                  fall behind the video timestamps by about six seconds every 10 
                  minutes, making them useless even for short files. It's more accurate 
                  to compute the timestamp based on file position and the most recent 
                  video frame's.

               2. The audio stream is longer than the video stream, even though the 
                  video timestamps show that the video stream is the correct length.
                  There are no duplicated frames either so the microphone might be
                  sampled concurrently by multiple processes.

               3. Sometimes we receive up to 30(!) 20ms audio frames in a row between 
                  two 100ms video frames. Accurate decoding without a second pass does
                  not seem possible.
            */

            timestamp = ctx->last_video_timestamp + 20 * ctx->audio_frames_since_last_video_frame;
            ctx->audio_frames_since_last_video_frame++;

            uint32_t prefix = avio_rl32(s->pb);

            /* The audio packets have a seemingly static four byte prefix */
            if (prefix != 0x00500100) {
                av_log(s, AV_LOG_ERROR, "Unknown audio frame prefix: 0x%08x\n", prefix);
                return AVERROR_PATCHWELCOME;
            }

            size -= 4;
        } else {
            timestamp -= ctx->video_start_timestamp;

            /* Reset the consecutive-audio-frames counter */
            if (timestamp != ctx->last_video_timestamp) {
                ctx->audio_frames_since_last_video_frame = 0;
                ctx->last_video_timestamp = timestamp;
            }
        }

        int ret = av_get_packet(s->pb, pkt, size);

        if (ret < 0) {
            return ret;
        }

        pkt->stream_index = stream_index;
        pkt->pts = timestamp;
        pkt->dts = AV_NOPTS_VALUE;

        return ret;
    }

    return AVERROR_EOF;
}


AVInputFormat ff_hxvf_demuxer = {
    .name           = "hxvf",
    .long_name      = NULL_IF_CONFIG_SMALL("Hichip security camera video"),
    .flags          = AVFMT_GENERIC_INDEX | AVFMT_VARIABLE_FPS,
    .priv_data_size = sizeof(HXVFDemuxContext),
    .read_probe     = hxvf_probe,
    .read_header    = hxvf_read_header,
    .read_packet    = hxvf_read_packet,
    .extensions     = "264",
};
