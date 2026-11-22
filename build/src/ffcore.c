// Streaming remuxer for the browser. Reads any container through JS range reads,
// passes video through untouched into fragmented MP4, passes audio through or
// transcodes it to AAC, and hands subtitle packets to JS.
#include <emscripten.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/intreadwrite.h>
#include <libswresample/swresample.h>

EM_JS(int, js_read, (double pos, uint8_t *buf, int size), { return Module.onRead(pos, buf, size); });
EM_JS(int, js_peek, (double pos, uint8_t *buf, int size), { return Module.onPeek(pos, buf, size); });
EM_JS(double, js_size, (void), { return Module.onSize(); });
EM_JS(void, js_out, (int kind, const uint8_t *buf, int size), { Module.onOut(kind, HEAPU8.slice(buf, buf + size)); });
EM_JS(void, js_text, (int idx, double start, double end, const uint8_t *buf, int size), {
  Module.onText(idx, start, end, HEAPU8.slice(buf, buf + size));
});
EM_JS(void, js_rap, (double t), { Module.onRap(t); });
EM_JS(void, js_bitmap, (int idx, double start, double end, int cw, int ch, int x, int y, int w, int h, const uint8_t *rgba), {
  Module.onBitmap(idx, start, end, cw, ch, x, y, w, h, w > 0 ? HEAPU8.slice(rgba, rgba + w * h * 4) : null);
});

#define MAX_REORDER 16

typedef struct Out {
    AVFormatContext *oc;
    AVBSFContext *bsf;               // ADTS to MP4 AAC; fragmented MP4 turns off automatic filters
    AVRational in_tb;
} Out;

typedef struct StreamState {
    int64_t off;                     // added to every timestamp, in stream time base
    int64_t last_dts;
    int64_t pts_buf[MAX_REORDER + 1];
    int reorder;                     // video reorder depth, used to rebuild missing DTS
    int64_t frame_dur;
} StreamState;

static AVFormatContext *ic;
static StreamState *ss;
static int regen_dts;                // container stores PTS only (Matroska)
static double shift;                 // seconds added to the source timeline so DTS never goes negative
static int64_t start_us;

static Out vout, aout;
static int vidx = -1, aidx = -1, bmp_idx = -1;
static int idr_only, need_idr, got_key, skip_rasl, strip_dv, nal_len_size = 4, at_eof, fresh;
static int annexb;                   // video packets use start codes (MPEG-TS), not length prefixes
static int64_t mkv_track; // Matroska track number of the video stream, once learned

static AVCodecContext *adec, *aenc, *sdec;
static SwrContext *swr;
static AVAudioFifo *fifo;
static AVFrame *dframe, *eframe;
static AVPacket *pkt, *opkt;
static uint8_t **cvt_buf;
static int cvt_cap;
static int64_t a_next_pts;

/* ---------- input ---------- */

static int64_t in_pos;

static int read_cb(void *opaque, uint8_t *buf, int size) {
    int n = js_read((double)in_pos, buf, size);
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    in_pos += n;
    return n;
}

static int64_t seek_cb(void *opaque, int64_t off, int whence) {
    double size = js_size();
    if (whence & AVSEEK_SIZE) return size < 0 ? -1 : (int64_t)size;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: in_pos = off; break;
    case SEEK_CUR: in_pos += off; break;
    case SEEK_END: if (size < 0) return -1; in_pos = (int64_t)size + off; break;
    default: return -1;
    }
    return in_pos;
}

/* ---------- SPS parsing, only to learn the B-frame reorder depth ---------- */

typedef struct BR { uint8_t *p; int bits, pos; } BR;

static unsigned rb(BR *b, int n) {
    unsigned v = 0;
    while (n--) {
        int bit = b->pos < b->bits ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0;
        v = (v << 1) | bit;
        b->pos++;
    }
    return v;
}

static unsigned ue(BR *b) {
    int z = 0;
    while (!rb(b, 1) && z < 31 && b->pos < b->bits) z++;
    return ((1u << z) - 1) + rb(b, z);
}

static int se(BR *b) {
    unsigned v = ue(b);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static int unescape(const uint8_t *src, int len, uint8_t *dst) {
    int n = 0, zeros = 0;
    for (int i = 0; i < len; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        zeros = src[i] ? 0 : zeros + 1;
        dst[n++] = src[i];
    }
    return n;
}

static int hevc_reorder(const uint8_t *nal, int len) {
    uint8_t buf[1024];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    BR b = { buf, unescape(nal + 2, len - 2, buf) * 8, 0 };
    rb(&b, 4);
    int max_sub = rb(&b, 3);
    rb(&b, 1);
    rb(&b, 8); rb(&b, 32); rb(&b, 24); rb(&b, 24); rb(&b, 8);
    int prof[8], lvl[8];
    for (int i = 0; i < max_sub; i++) { prof[i] = rb(&b, 1); lvl[i] = rb(&b, 1); }
    if (max_sub > 0) for (int i = max_sub; i < 8; i++) rb(&b, 2);
    for (int i = 0; i < max_sub; i++) {
        if (prof[i]) { rb(&b, 32); rb(&b, 32); rb(&b, 24); }
        if (lvl[i]) rb(&b, 8);
    }
    ue(&b);
    if (ue(&b) == 3) rb(&b, 1);
    ue(&b); ue(&b);
    if (rb(&b, 1)) { ue(&b); ue(&b); ue(&b); ue(&b); }
    ue(&b); ue(&b); ue(&b);
    int present = rb(&b, 1), r = 0;
    for (int i = present ? 0 : max_sub; i <= max_sub; i++) { ue(&b); r = ue(&b); ue(&b); }
    return r;
}

static void h264_hrd(BR *b) {
    int cnt = ue(b) + 1;
    rb(b, 8);
    for (int i = 0; i < cnt && i < 32; i++) { ue(b); ue(b); rb(b, 1); }
    rb(b, 20);
}

static int h264_reorder(const uint8_t *nal, int len) {
    uint8_t buf[1024];
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    BR b = { buf, unescape(nal + 1, len - 1, buf) * 8, 0 };
    int profile = rb(&b, 8);
    rb(&b, 16);
    ue(&b);
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        int chroma = ue(&b);
        if (chroma == 3) rb(&b, 1);
        ue(&b); ue(&b); rb(&b, 1);
        if (rb(&b, 1)) {
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) {
                if (!rb(&b, 1)) continue;
                int last = 8, next = 8, size = i < 6 ? 16 : 64;
                for (int j = 0; j < size; j++) {
                    if (next) next = (last + se(&b) + 256) % 256;
                    if (next) last = next;
                }
            }
        }
    }
    ue(&b);
    int poc_type = ue(&b);
    if (poc_type == 0) ue(&b);
    else if (poc_type == 1) {
        rb(&b, 1); se(&b); se(&b);
        int n = ue(&b);
        for (int i = 0; i < n && i < 256; i++) se(&b);
    }
    ue(&b); rb(&b, 1); ue(&b); ue(&b);
    if (!rb(&b, 1)) rb(&b, 1);
    rb(&b, 1);
    if (rb(&b, 1)) { ue(&b); ue(&b); ue(&b); ue(&b); }
    int fallback = profile == 66 ? 0 : 2;
    if (!rb(&b, 1)) return fallback;
    if (rb(&b, 1) && rb(&b, 8) == 255) rb(&b, 32);
    if (rb(&b, 1)) rb(&b, 1);
    if (rb(&b, 1)) { rb(&b, 4); if (rb(&b, 1)) rb(&b, 24); }
    if (rb(&b, 1)) { ue(&b); ue(&b); }
    if (rb(&b, 1)) { rb(&b, 32); rb(&b, 32); rb(&b, 1); }
    int nal_hrd = rb(&b, 1);
    if (nal_hrd) h264_hrd(&b);
    int vcl_hrd = rb(&b, 1);
    if (vcl_hrd) h264_hrd(&b);
    if (nal_hrd || vcl_hrd) rb(&b, 1);
    rb(&b, 1);
    if (!rb(&b, 1)) return fallback;
    rb(&b, 1); ue(&b); ue(&b); ue(&b); ue(&b);
    return ue(&b);
}

static int reorder_depth(AVCodecParameters *par) {
    const uint8_t *p = par->extradata;
    int n = par->extradata_size, r = 2;
    if (par->codec_id == AV_CODEC_ID_HEVC && n > 23 && p[0] == 1) {
        int off = 23;
        for (int a = 0, arrays = p[22]; a < arrays && off + 3 <= n; a++) {
            int type = p[off] & 0x3f, cnt = AV_RB16(p + off + 1);
            off += 3;
            for (int j = 0; j < cnt && off + 2 <= n; j++) {
                int len = AV_RB16(p + off);
                off += 2;
                if (type == 33 && off + len <= n) return FFMIN(hevc_reorder(p + off, len), MAX_REORDER);
                off += len;
            }
        }
    } else if (par->codec_id == AV_CODEC_ID_H264 && n > 8 && p[0] == 1 && (p[5] & 0x1f)) {
        int len = AV_RB16(p + 6);
        if (8 + len <= n) return FFMIN(h264_reorder(p + 8, len), MAX_REORDER);
    } else if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC) {
        r = 0;
    }
    return r;
}

/* ---------- timestamps ---------- */

static void reset_ts(void) {
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        ss[i].last_dts = AV_NOPTS_VALUE;
        for (int j = 0; j <= MAX_REORDER; j++) ss[i].pts_buf[j] = AV_NOPTS_VALUE;
    }
}

// Rebuilds DTS from PTS where the container lacks it, enforces monotonic DTS,
// then moves the packet onto the output timeline. Returns 0 if the packet is unusable.
static int fix_ts(AVPacket *p) {
    AVStream *st = ic->streams[p->stream_index];
    StreamState *s = &ss[p->stream_index];
    if (p->pts == AV_NOPTS_VALUE && p->dts == AV_NOPTS_VALUE) return 0;
    if (p->pts == AV_NOPTS_VALUE) p->pts = p->dts;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (regen_dts || p->dts == AV_NOPTS_VALUE)) {
        int d = s->reorder;
        int64_t *b = s->pts_buf;
        int64_t dur = p->duration > 0 ? p->duration : s->frame_dur;
        b[0] = p->pts;
        for (int i = 1; i < d + 1 && b[i] == AV_NOPTS_VALUE; i++) b[i] = p->pts + (i - d - 1) * dur;
        for (int i = 0; i < d && b[i] > b[i + 1]; i++) FFSWAP(int64_t, b[i], b[i + 1]);
        p->dts = b[0];
    }
    if (p->dts == AV_NOPTS_VALUE) p->dts = p->pts;
    if (s->last_dts != AV_NOPTS_VALUE && p->dts <= s->last_dts) p->dts = s->last_dts + 1;
    if (p->pts < p->dts) p->pts = p->dts;
    s->last_dts = p->dts;
    p->pts += s->off;
    p->dts += s->off;
    return p->dts >= 0;
}

static double ts_sec(AVPacket *p) {
    return p->pts * av_q2d(ic->streams[p->stream_index]->time_base);
}

/* ---------- output ---------- */

static int write_cb(void *opaque, const uint8_t *buf, int size) {
    js_out((int)(intptr_t)opaque, buf, size);
    return size;
}

static void close_out(Out *o) {
    av_bsf_free(&o->bsf);
    if (!o->oc) return;
    if (o->oc->pb) {
        av_freep(&o->oc->pb->buffer);
        avio_context_free(&o->oc->pb);
    }
    avformat_free_context(o->oc);
    o->oc = NULL;
}

static int open_out(Out *o, int kind, const AVCodecParameters *par, AVRational tb, int dv) {
    int ret = avformat_alloc_output_context2(&o->oc, NULL, "mp4", NULL);
    if (ret < 0) return ret;
    uint8_t *buf = av_malloc(1 << 16);
    o->oc->pb = avio_alloc_context(buf, 1 << 16, 1, (void *)(intptr_t)kind, NULL, write_cb, NULL);
    o->oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    AVStream *st = avformat_new_stream(o->oc, NULL);
    avcodec_parameters_copy(st->codecpar, par);
    st->codecpar->codec_tag = 0;
    st->time_base = tb;
    o->in_tb = tb;
    if (par->codec_id == AV_CODEC_ID_HEVC)
        st->codecpar->codec_tag = dv ? MKTAG('d', 'v', 'h', '1') : MKTAG('h', 'v', 'c', '1');
    if (par->codec_id == AV_CODEC_ID_VP9 && st->codecpar->format < 0) {
        // No VP9 decoder here to report it, but vpcC needs it. The profile pins it down.
        static const enum AVPixelFormat fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV444P10 };
        st->codecpar->format = fmts[par->profile >= 0 && par->profile <= 3 ? par->profile : 0];
    }
    if (par->codec_id == AV_CODEC_ID_AAC && !par->extradata_size) {
        const AVBitStreamFilter *f = av_bsf_get_by_name("aac_adtstoasc");
        if (f && av_bsf_alloc(f, &o->bsf) >= 0) {
            avcodec_parameters_copy(o->bsf->par_in, par);
            o->bsf->time_base_in = tb;
            if (av_bsf_init(o->bsf) < 0) av_bsf_free(&o->bsf);
        }
    }
    if (dv) o->oc->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    else av_packet_side_data_remove(st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_DOVI_CONF);
    // delay_moov writes the init segment with the first fragment. AC-3, E-AC-3, TrueHD
    // and Annex B H.264/HEVC need a real packet before their sample entry can be built.
    AVDictionary *opts = NULL;
    if (kind == 0) {
        av_dict_set(&opts, "movflags", "delay_moov+default_base_moof+frag_keyframe+frag_discont+skip_trailer", 0);
    } else {
        av_dict_set(&opts, "movflags", "delay_moov+default_base_moof+frag_discont+skip_trailer", 0);
        av_dict_set(&opts, "frag_duration", "1000000", 0);
    }
    av_dict_set(&opts, "use_editlist", "0", 0);
    ret = avformat_write_header(o->oc, &opts);
    av_dict_free(&opts);
    if (ret < 0) { close_out(o); return ret; }
    avio_flush(o->oc->pb);
    return 0;
}

static void mux(Out *o, AVPacket *p) {
    p->stream_index = 0;
    if (o->bsf) {
        if (av_bsf_send_packet(o->bsf, p) < 0) return;
        while (av_bsf_receive_packet(o->bsf, p) == 0) {
            av_packet_rescale_ts(p, o->in_tb, o->oc->streams[0]->time_base);
            av_write_frame(o->oc, p);
            av_packet_unref(p);
        }
        return;
    }
    av_packet_rescale_ts(p, o->in_tb, o->oc->streams[0]->time_base);
    av_write_frame(o->oc, p);
}

/* ---------- Dolby Vision ---------- */

// Finds the NAL unit at or after p and sets *nal to its header. Returns where the following
// unit starts, or NULL when no complete unit is left.
static const uint8_t *next_nal(const uint8_t *p, const uint8_t *end, const uint8_t **nal) {
    if (annexb) {
        while (p + 3 < end && (p[0] || p[1] || p[2] != 1)) p++;
        if (p + 3 >= end) return NULL;
        *nal = p + 3;
        for (p += 3; p + 2 < end; p++) if (!p[0] && !p[1] && p[2] == 1) return p;
        return end;
    }
    if (p + nal_len_size >= end) return NULL;
    uint32_t len = 0;
    for (int i = 0; i < nal_len_size; i++) len = (len << 8) | p[i];
    if (len > (uint32_t)(end - p - nal_len_size)) return NULL;
    *nal = p + nal_len_size;
    return *nal + len;
}

// Drops RPU (62) and enhancement layer (63) NAL units so a profile 7 stream decodes as plain HDR10.
static void strip_dv_nals(AVPacket *p) {
    if (av_packet_make_writable(p) < 0) return;
    const uint8_t *src = p->data, *end = p->data + p->size, *nal, *next;
    uint8_t *dst = p->data;
    while ((next = next_nal(src, end, &nal))) {
        int type = (nal[0] >> 1) & 0x3f;
        if (type != 62 && type != 63) {
            if (dst != src) memmove(dst, src, next - src);
            dst += next - src;
        }
        src = next;
    }
    p->size = dst - p->data;
}

// Type of the first slice NAL in an HEVC packet, or -1
static int hevc_first_vcl(const AVPacket *p) {
    const uint8_t *src = p->data, *end = p->data + p->size, *nal;
    while ((src = next_nal(src, end, &nal))) {
        int type = (nal[0] >> 1) & 0x3f;
        if (type < 32) return type;
    }
    return -1;
}

// After starting on an open-GOP CRA frame, the RASL frames that follow reference
// the previous GOP. Hardware decoders often fail on them, so drop them.
static int drop_rasl(const AVPacket *p) {
    int type = hevc_first_vcl(p);
    if (type == 8 || type == 9) return 1;
    if (type >= 0 && type <= 5 && !(p->flags & AV_PKT_FLAG_KEY)) skip_rasl = 0;
    return 0;
}

/* ---------- audio transcode ---------- */

static void close_audio(void) {
    avcodec_free_context(&adec);
    avcodec_free_context(&aenc);
    swr_free(&swr);
    if (fifo) av_audio_fifo_free(fifo);
    fifo = NULL;
    if (cvt_buf) av_freep(&cvt_buf[0]);
    av_freep(&cvt_buf);
    cvt_cap = 0;
}

static int open_audio_transcode(AVStream *st, int max_ch) {
    const AVCodec *dc = avcodec_find_decoder(st->codecpar->codec_id);
    const AVCodec *ec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!dc || !ec) return AVERROR_DECODER_NOT_FOUND;
    adec = avcodec_alloc_context3(dc);
    avcodec_parameters_to_context(adec, st->codecpar);
    adec->pkt_timebase = st->time_base;
    int ret = avcodec_open2(adec, dc, NULL);
    if (ret < 0) return ret;

    aenc = avcodec_alloc_context3(ec);
    int in_ch = st->codecpar->ch_layout.nb_channels;
    if (in_ch >= 8 && max_ch >= 8) aenc->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1;
    else if (in_ch >= 6 && max_ch >= 6) aenc->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK;
    else if (in_ch == 1) aenc->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    else aenc->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    int rate = st->codecpar->sample_rate;
    aenc->sample_rate = rate == 44100 || rate == 48000 ? rate : 48000;
    aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aenc->time_base = (AVRational){ 1, aenc->sample_rate };
    aenc->bit_rate = aenc->ch_layout.nb_channels == 1 ? 96000 : 80000 * aenc->ch_layout.nb_channels;
    aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(aenc->priv_data, "aac_coder", "fast", 0);
    ret = avcodec_open2(aenc, ec, NULL);
    if (ret < 0) return ret;

    fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, aenc->ch_layout.nb_channels, 8192);
    a_next_pts = AV_NOPTS_VALUE;
    AVCodecParameters *par = avcodec_parameters_alloc();
    avcodec_parameters_from_context(par, aenc);
    ret = open_out(&aout, 1, par, aenc->time_base, 0);
    avcodec_parameters_free(&par);
    return ret;
}

static void drain_encoder(void) {
    while (avcodec_receive_packet(aenc, opkt) == 0) {
        if (opkt->dts >= 0) mux(&aout, opkt);
        av_packet_unref(opkt);
    }
}

static void encode_fifo(int flush) {
    int fs = aenc->frame_size;
    while (av_audio_fifo_size(fifo) >= fs || (flush && av_audio_fifo_size(fifo) > 0)) {
        int n = FFMIN(av_audio_fifo_size(fifo), fs);
        av_frame_unref(eframe);
        eframe->nb_samples = n;
        eframe->format = AV_SAMPLE_FMT_FLTP;
        eframe->sample_rate = aenc->sample_rate;
        av_channel_layout_copy(&eframe->ch_layout, &aenc->ch_layout);
        if (av_frame_get_buffer(eframe, 0) < 0) return;
        av_audio_fifo_read(fifo, (void **)eframe->data, n);
        eframe->pts = a_next_pts;
        a_next_pts += n;
        avcodec_send_frame(aenc, eframe);
        drain_encoder();
    }
    if (flush) {
        avcodec_send_frame(aenc, NULL);
        drain_encoder();
    }
}

static void audio_frame(AVFrame *f) {
    if (!swr) {
        if (swr_alloc_set_opts2(&swr, &aenc->ch_layout, AV_SAMPLE_FMT_FLTP, aenc->sample_rate,
                                &f->ch_layout, f->format, f->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
            swr_free(&swr);
            return;
        }
    }
    if (a_next_pts == AV_NOPTS_VALUE) {
        int64_t p = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (p == AV_NOPTS_VALUE) return;
        a_next_pts = av_rescale_q(p, adec->pkt_timebase, aenc->time_base);
    }
    int max_out = swr_get_out_samples(swr, f->nb_samples);
    if (max_out > cvt_cap) {
        if (cvt_buf) av_freep(&cvt_buf[0]);
        av_freep(&cvt_buf);
        if (av_samples_alloc_array_and_samples(&cvt_buf, NULL, aenc->ch_layout.nb_channels, max_out,
                                               AV_SAMPLE_FMT_FLTP, 0) < 0) {
            cvt_cap = 0;
            return;
        }
        cvt_cap = max_out;
    }
    int n = swr_convert(swr, cvt_buf, max_out, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n > 0) av_audio_fifo_write(fifo, (void **)cvt_buf, n);
    encode_fifo(0);
}

static void decode_audio(AVPacket *p) {
    // p already carries output timestamps; the decoder just forwards them to frames
    if (avcodec_send_packet(adec, p) < 0) return;
    while (avcodec_receive_frame(adec, dframe) == 0) {
        audio_frame(dframe);
        av_frame_unref(dframe);
    }
}

/* ---------- subtitles ---------- */

static int is_bitmap(enum AVCodecID id) {
    return id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || id == AV_CODEC_ID_DVD_SUBTITLE || id == AV_CODEC_ID_DVB_SUBTITLE;
}

static int is_text(enum AVCodecID id) {
    return id == AV_CODEC_ID_SUBRIP || id == AV_CODEC_ID_ASS || id == AV_CODEC_ID_SSA || id == AV_CODEC_ID_WEBVTT ||
           id == AV_CODEC_ID_MOV_TEXT || id == AV_CODEC_ID_TEXT;
}

static void decode_bitmap(AVPacket *p) {
    AVSubtitle sub;
    int got = 0;
    double t = ts_sec(p);
    if (avcodec_decode_subtitle2(sdec, &sub, &got, p) < 0 || !got) return;
    double start = t + sub.start_display_time / 1000.0;
    double end = sub.end_display_time && sub.end_display_time != UINT32_MAX ? t + sub.end_display_time / 1000.0 : -1;
    int cw = sdec->width, ch = sdec->height;
    if (sub.num_rects == 0) js_bitmap(p->stream_index, start, end, cw, ch, 0, 0, 0, 0, NULL);
    for (unsigned i = 0; i < sub.num_rects; i++) {
        AVSubtitleRect *r = sub.rects[i];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0) continue;
        uint8_t *rgba = av_malloc(r->w * r->h * 4);
        const uint32_t *pal = (const uint32_t *)r->data[1];
        for (int y = 0; y < r->h; y++) {
            for (int x = 0; x < r->w; x++) {
                uint32_t c = pal[r->data[0][y * r->linesize[0] + x]];
                uint8_t *d = rgba + (y * r->w + x) * 4;
                d[0] = c >> 16; d[1] = c >> 8; d[2] = c; d[3] = c >> 24;
            }
        }
        js_bitmap(p->stream_index, start, end, cw, ch, r->x, r->y, r->w, r->h, rgba);
        av_free(rgba);
    }
    avsubtitle_free(&sub);
}

/* ---------- JSON info ---------- */

static void json_str(AVBPrint *bp, const char *key, const char *s) {
    if (!s) return;
    av_bprintf(bp, ",\"%s\":\"", key);
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') av_bprintf(bp, "\\%c", c);
        else if (c < 0x20) av_bprintf(bp, "\\u%04x", c);
        else av_bprint_chars(bp, c, 1);
    }
    av_bprint_chars(bp, '"', 1);
}

static void video_codec_string(AVCodecParameters *par, char *out, int n) {
    const uint8_t *p = par->extradata;
    int sz = par->extradata_size;
    out[0] = 0;
    if (par->codec_id == AV_CODEC_ID_H264) {
        if (sz > 4 && p[0] == 1) snprintf(out, n, "avc1.%02x%02x%02x", p[1], p[2], p[3]);
        else snprintf(out, n, "avc1.%02x00%02x", par->profile > 0 ? par->profile : 100, par->level > 0 ? par->level : 40);
    } else if (par->codec_id == AV_CODEC_ID_HEVC) {
        if (sz > 22 && p[0] == 1) {
            uint32_t compat = AV_RB32(p + 2), rev = 0;
            for (int i = 0; i < 32; i++) rev |= ((compat >> i) & 1) << (31 - i);
            int space = p[1] >> 6, last = 11;
            int len = snprintf(out, n, "hvc1.%s%d.%X.%c%d", space ? (const char *[]){ "", "A", "B", "C" }[space] : "",
                               p[1] & 0x1f, rev, (p[1] & 0x20) ? 'H' : 'L', p[12]);
            while (last >= 6 && !p[last]) last--;
            for (int i = 6; i <= last && len < n - 4; i++) len += snprintf(out + len, n - len, ".%02X", p[i]);
        } else {
            int prof = par->profile > 0 ? par->profile : 1;
            snprintf(out, n, "hvc1.%d.%d.L%d.B0", prof, prof == 2 ? 4 : 6, par->level > 0 ? par->level : 153);
        }
    } else if (par->codec_id == AV_CODEC_ID_AV1) {
        if (sz >= 4 && (p[0] & 0x80)) {
            int bd = (p[2] & 0x40) ? ((p[2] & 0x20) ? 12 : 10) : 8;
            snprintf(out, n, "av01.%d.%02d%c.%02d", p[1] >> 5, p[1] & 0x1f, (p[2] & 0x80) ? 'H' : 'M', bd);
        } else snprintf(out, n, "av01.0.08M.08");
    } else if (par->codec_id == AV_CODEC_ID_VP9) {
        int prof = par->profile > 0 ? par->profile : 0;
        int level = par->level > 0 ? par->level : (par->height > 1088 ? 51 : 41);
        snprintf(out, n, "vp09.%02d.%02d.%02d", prof, level, prof >= 2 ? 10 : 8);
    }
}

static char *info_json;

static void build_info(void) {
    AVBPrint bp;
    av_bprint_init(&bp, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprintf(&bp, "{\"duration\":%f,\"shift\":%f", ic->duration > 0 ? ic->duration / 1e6 : 0, shift);
    json_str(&bp, "format", ic->iformat->name);
    av_bprintf(&bp, ",\"streams\":[");
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        AVCodecParameters *par = st->codecpar;
        const char *type = av_get_media_type_string(par->codec_type);
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        av_bprintf(&bp, "%s{\"index\":%u,\"default\":%d,\"forced\":%d", i ? "," : "", i,
                   !!(st->disposition & AV_DISPOSITION_DEFAULT), !!(st->disposition & AV_DISPOSITION_FORCED));
        json_str(&bp, "type", type ? type : "unknown");
        json_str(&bp, "codec", avcodec_get_name(par->codec_id));
        int profile = par->profile;
        if (profile < 0 && par->extradata_size > 4 && par->extradata[0] == 1) {
            if (par->codec_id == AV_CODEC_ID_HEVC) profile = par->extradata[1] & 0x1f;
            if (par->codec_id == AV_CODEC_ID_H264) profile = par->extradata[1];
        }
        av_bprintf(&bp, ",\"profile\":%d", profile); // names are stripped by --enable-small
        json_str(&bp, "lang", lang ? lang->value : NULL);
        json_str(&bp, "title", title ? title->value : NULL);
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            char cs[64];
            video_codec_string(par, cs, sizeof(cs));
            AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            av_bprintf(&bp, ",\"width\":%d,\"height\":%d,\"fps\":%.3f", par->width, par->height, fr.den ? av_q2d(fr) : 0);
            json_str(&bp, "mse", cs);
            json_str(&bp, "trc", av_color_transfer_name(par->color_trc));
            json_str(&bp, "primaries", av_color_primaries_name(par->color_primaries));
            const AVPacketSideData *sd = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                                 AV_PKT_DATA_DOVI_CONF);
            if (sd) {
                const AVDOVIDecoderConfigurationRecord *dv = (const void *)sd->data;
                av_bprintf(&bp, ",\"dv\":{\"profile\":%d,\"level\":%d,\"compat\":%d,\"el\":%d,\"rpu\":%d}",
                           dv->dv_profile, dv->dv_level, dv->dv_bl_signal_compatibility_id, dv->el_present_flag,
                           dv->rpu_present_flag);
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            char layout[64] = "";
            av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
            av_bprintf(&bp, ",\"channels\":%d,\"rate\":%d", par->ch_layout.nb_channels, par->sample_rate);
            json_str(&bp, "layout", layout);
        } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            av_bprintf(&bp, ",\"bitmap\":%d,\"text\":%d", is_bitmap(par->codec_id), is_text(par->codec_id));
        }
        av_bprint_chars(&bp, '}', 1);
    }
    av_bprintf(&bp, "]}");
    av_bprint_finalize(&bp, &info_json);
}

/* ---------- API ---------- */

EMSCRIPTEN_KEEPALIVE const char *fp_open(void) {
    av_log_set_level(AV_LOG_ERROR);
    pkt = av_packet_alloc();
    opkt = av_packet_alloc();
    dframe = av_frame_alloc();
    eframe = av_frame_alloc();
    uint8_t *buf = av_malloc(1 << 16);
    AVIOContext *pb = avio_alloc_context(buf, 1 << 16, 0, NULL, read_cb, NULL, seek_cb);
    ic = avformat_alloc_context();
    ic->pb = pb;
    ic->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&ic, NULL, NULL, NULL) < 0) return NULL;
    ic->max_analyze_duration = 3 * AV_TIME_BASE;
    if (avformat_find_stream_info(ic, NULL) < 0) return NULL;

    regen_dts = strstr(ic->iformat->name, "matroska") != NULL;
    start_us = ic->start_time != AV_NOPTS_VALUE ? ic->start_time : 0;
    // Matroska reads its cues on the first seek and never again, even if that read failed.
    // A seek during a network drop would leave every later seek scanning the file, so read
    // them now. This seek goes to the start, where the first play begins anyway.
    if (regen_dts) avformat_seek_file(ic, -1, INT64_MIN, start_us, start_us, 0);
    ss = av_calloc(ic->nb_streams, sizeof(*ss));
    double max_lead = 0;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
        if (!fr.num) fr = (AVRational){ 24, 1 };
        ss[i].reorder = reorder_depth(st->codecpar);
        ss[i].frame_dur = FFMAX(1, av_rescale_q(1, av_inv_q(fr), st->time_base));
        max_lead = FFMAX(max_lead, (ss[i].reorder + 1) / av_q2d(fr));
    }
    // Headroom for B-frame DTS lead and AAC priming, so nothing lands before zero
    shift = ceil((max_lead + 0.05) * 1000) / 1000;
    for (unsigned i = 0; i < ic->nb_streams; i++)
        ss[i].off = av_rescale_q((int64_t)(shift * 1e6) - start_us, AV_TIME_BASE_Q, ic->streams[i]->time_base);
    build_info();
    fresh = 1;
    mkv_track = 0;
    return info_json;
}

// Time of the first video keyframe after the current read position, or -1
static double next_keyframe(int v) {
    for (int n = 0; n < 2000; n++) {
        if (av_read_frame(ic, pkt) < 0) return -1;
        int hit = pkt->stream_index == v && (pkt->flags & AV_PKT_FLAG_KEY) && pkt->pts != AV_NOPTS_VALUE;
        double t = hit ? pkt->pts * av_q2d(ic->streams[v]->time_base) : 0;
        av_packet_unref(pkt);
        if (hit) return t;
    }
    return -1;
}

// MPEG-TS has no index, so a timestamp seek lands anywhere inside a GOP. Step back
// until the first keyframe after the landing point is at or before the target.
static int seek_ts_keyframe(int v, int64_t target) {
    int64_t try_ts = target;
    double want = target / 1e6;
    for (int i = 0; i < 5; i++) {
        if (try_ts <= start_us) return avformat_seek_file(ic, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BYTE);
        if (avformat_seek_file(ic, -1, INT64_MIN, try_ts, try_ts, 0) < 0) return -1;
        double kf = next_keyframe(v);
        if (kf < 0 || kf <= want + 0.05) break;
        try_ts -= (int64_t)((kf - want) * 1e6) + 2000000;
    }
    return avformat_seek_file(ic, -1, INT64_MIN, try_ts, try_ts, 0);
}

// Reads an EBML ID (with its length marker) or size (without), or returns UINT64_MAX
static uint64_t ebml_num(const uint8_t **p, const uint8_t *end, int id) {
    if (*p >= end) return UINT64_MAX;
    int len = 1;
    while (len <= 8 && !(**p & (0x80 >> (len - 1)))) len++;
    if (len > 8 || *p + len > end) return UINT64_MAX;
    uint64_t v = id ? **p : **p & (0xFF >> len);
    for (int i = 1; i < len; i++) v = (v << 8) | (*p)[i];
    *p += len;
    return v;
}

// Walks the blocks at the start of a Matroska cluster, from 64 KB read at pos. With a track number,
// returns the frame of that track's first block. With *track 0, returns the first frame that starts
// with the bytes in want and stores its track number. Returns NULL if nothing matches.
static const uint8_t *mkv_block(int64_t pos, int64_t *track, const uint8_t *want, int want_len, const uint8_t **fend) {
    static uint8_t buf[1 << 16];
    int n = js_peek(pos, buf, sizeof buf);
    const uint8_t *p = buf, *end = buf + (n > 0 ? n : 0);
    if (ebml_num(&p, end, 1) != 0x1F43B675 || ebml_num(&p, end, 0) == UINT64_MAX) return NULL;
    while (p < end) {
        uint64_t id = ebml_num(&p, end, 1), len = ebml_num(&p, end, 0);
        if (id == UINT64_MAX || len == UINT64_MAX) return NULL;
        if (id == 0xA0) continue; // BlockGroup, so read its children
        const uint8_t *b = p, *bend = len < (uint64_t)(end - p) ? p + len : end;
        p = bend;
        if (id != 0xA3 && id != 0xA1) continue; // not a SimpleBlock or Block
        uint64_t num = ebml_num(&b, bend, 0);
        if (num == UINT64_MAX || b + 3 > bend || (b[2] & 0x06)) continue; // laced
        b += 3;
        if (*track ? (int64_t)num != *track : bend - b < want_len || memcmp(b, want, want_len)) continue;
        *track = num;
        *fend = bend;
        return b;
    }
    return NULL;
}

// Type of the first slice NAL in the first video block of a Matroska cluster, read without
// the rest of the frame. Returns -1 if it is not in the first 64 KB.
static int mkv_first_vcl(int64_t pos) {
    const uint8_t *end, *b = mkv_track && !annexb ? mkv_block(pos, &mkv_track, NULL, 0, &end) : NULL;
    for (; b && b + nal_len_size < end;) {
        uint32_t l = 0;
        for (int i = 0; i < nal_len_size; i++) l = (l << 8) | b[i];
        if (!l || (b[nal_len_size] & 0x80)) return -1; // not length-prefixed NALs
        int type = (b[nal_len_size] >> 1) & 0x3f;
        if (type < 32) return type;
        b += nal_len_size + l;
    }
    return -1;
}

// Type of the first slice NAL of the first video keyframe at or after kts, read in full.
// In Matroska it also learns the video track number from the cluster at pos.
static int probe_vcl(int v, int64_t kts, int64_t pos) {
    if (av_seek_frame(ic, v, kts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    int type = -1;
    for (int n = 0; n < 500 && av_read_frame(ic, pkt) >= 0; n++) {
        int hit = pkt->stream_index == v && (pkt->flags & AV_PKT_FLAG_KEY);
        if (hit) {
            type = hevc_first_vcl(pkt);
            const uint8_t *end;
            if (pos >= 0 && !mkv_track && pkt->size >= 32) mkv_block(pos, &mkv_track, pkt->data, 32, &end);
        }
        av_packet_unref(pkt);
        if (hit) break;
    }
    return type;
}

// Firefox resumes HEVC only from an IDR picture and drops video that starts on a CRA.
// Walk the keyframe index back from the target and seek to the nearest IDR keyframe.
// Without an IDR in the 30 seconds before the target, seek normally and let fp_pump
// start video at the next IDR instead.
static int seek_idr(int v, int64_t target) {
    // Matroska reads its cues on the first seek, so seek once before searching the index
    if (avformat_seek_file(ic, -1, INT64_MIN, target, target, 0) < 0) return -1;
    AVStream *st = ic->streams[v];
    int64_t ts = av_rescale_q(target, AV_TIME_BASE_Q, st->time_base);
    int64_t floor_ts = ts - av_rescale_q(30 * AV_TIME_BASE, AV_TIME_BASE_Q, st->time_base);
    int i = av_index_search_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    // Matroska cues point at clusters, so the frame header can be read without the frame.
    // A header IDR is confirmed with a full read, which also caches the bytes playback needs.
    int peek = strstr(ic->iformat->name, "matroska") != NULL;
    for (int tries = 0; i >= 0 && tries < 60; i--) {
        const AVIndexEntry *e = avformat_index_get_entry(st, i);
        if (!e || !(e->flags & AVINDEX_KEYFRAME)) continue;
        int64_t kts = e->timestamp;
        if (kts < floor_ts) break;
        tries++;
        int type = peek ? mkv_first_vcl(e->pos) : -1;
        if (type >= 0 && type != 19 && type != 20) continue;
        type = probe_vcl(v, kts, peek ? e->pos : -1);
        if (type == 19 || type == 20) return av_seek_frame(ic, v, kts, AVSEEK_FLAG_BACKWARD);
    }
    need_idr = 1;
    return avformat_seek_file(ic, -1, INT64_MIN, target, target, 0);
}

static void close_all(void) {
    close_out(&vout);
    close_out(&aout);
    close_audio();
    avcodec_free_context(&sdec);
}

// t: seconds on the output timeline. a_mode: 0 copy, 1 transcode to AAC. idr: start HEVC on an IDR.
// Returns 0, or -1 for video, -2 for audio, -3 for seek failures.
EMSCRIPTEN_KEEPALIVE int fp_start(double t, int v, int a, int a_mode, int max_ch, int sub, int dv_mode, int idr) {
    close_all();
    // A read error sticks to the AVIOContext, and lavf would later report the real end of
    // file as that error. This start seeks and reads afresh, so forget it.
    ic->pb->error = 0;
    vidx = v; aidx = a; bmp_idx = -1;
    got_key = 0; at_eof = 0;
    skip_rasl = v >= 0 && ic->streams[v]->codecpar->codec_id == AV_CODEC_ID_HEVC;
    idr_only = idr && skip_rasl;
    need_idr = 0;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVCodecParameters *par = ic->streams[i]->codecpar;
        int keep = (int)i == v || (int)i == a || ((int)i == sub && is_bitmap(par->codec_id)) ||
                   (par->codec_type == AVMEDIA_TYPE_SUBTITLE && is_text(par->codec_id));
        ic->streams[i]->discard = keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    if (v >= 0) {
        AVStream *st = ic->streams[v];
        const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                             st->codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
        strip_dv = sd && !dv_mode && ((const AVDOVIDecoderConfigurationRecord *)sd->data)->dv_profile == 7;
        annexb = !(st->codecpar->extradata_size > 22 && st->codecpar->extradata[0] == 1);
        if (!annexb) nal_len_size = (st->codecpar->extradata[21] & 3) + 1;
        if (open_out(&vout, 0, st->codecpar, st->time_base, dv_mode) < 0) { vidx = aidx = -1; return -1; }
    }
    if (a >= 0) {
        AVStream *st = ic->streams[a];
        int ret = a_mode ? open_audio_transcode(st, max_ch) : open_out(&aout, 1, st->codecpar, st->time_base, 0);
        if (ret < 0) { close_audio(); close_out(&aout); vidx = aidx = -1; return -2; }
    }
    if (sub >= 0 && is_bitmap(ic->streams[sub]->codecpar->codec_id)) {
        AVStream *st = ic->streams[sub];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        sdec = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(sdec, st->codecpar);
        sdec->pkt_timebase = st->time_base;
        if (avcodec_open2(sdec, c, NULL) < 0) avcodec_free_context(&sdec);
        else bmp_idx = sub;
    }
    int64_t target = (int64_t)(t * 1e6) - (int64_t)(shift * 1e6) + start_us;
    int ret = 0;
    int is_ts = strstr(ic->iformat->name, "mpeg") != NULL;
    if (target <= start_us + AV_TIME_BASE / 2) {
        // After opening, the demuxer already sits at the start, so the first play needs no seek. A
        // timestamp seek in MPEG-TS/PS lands mid-GOP and loses the first keyframe, so rewind by byte there.
        if (!fresh && is_ts)
            ret = avformat_seek_file(ic, -1, INT64_MIN, 0, 0, AVSEEK_FLAG_BYTE);
        else if (!fresh)
            ret = avformat_seek_file(ic, -1, INT64_MIN, start_us, start_us, 0);
    } else {
        if (is_ts && v >= 0) ret = seek_ts_keyframe(v, target);
        else if (idr_only) ret = seek_idr(v, target);
        else ret = avformat_seek_file(ic, -1, INT64_MIN, target, target, 0);
        if (ret < 0) return -3;
    }
    fresh = 0;
    reset_ts();
    return 0;
}

static void finish(void) {
    if (adec) {
        avcodec_send_packet(adec, NULL);
        while (avcodec_receive_frame(adec, dframe) == 0) { audio_frame(dframe); av_frame_unref(dframe); }
        encode_fifo(1);
    }
    if (vout.oc) { av_write_trailer(vout.oc); avio_flush(vout.oc->pb); }
    if (aout.oc) { av_write_trailer(aout.oc); avio_flush(aout.oc->pb); }
    at_eof = 1;
}

// Demuxes until max_bytes of input were consumed or max_sec of media passed.
// Returns 0 when more data follows, 1 at end of file, negative on read errors.
// After a read error the demuxer stays failed until fp_start seeks again.
EMSCRIPTEN_KEEPALIVE int fp_pump(int max_bytes, double max_sec) {
    if (at_eof) return 1;
    int64_t pos0 = avio_tell(ic->pb);
    double t0 = -1;
    int err = 0;
    for (;;) {
        int ret = av_read_frame(ic, pkt);
        if (ret == AVERROR(EAGAIN)) continue;
        // Flush first, so the page never holds half a fragment when it restarts
        if (ret == AVERROR(EIO)) { err = -1; break; }
        if (ret < 0) { finish(); return 1; }
        int si = pkt->stream_index;
        AVCodecParameters *par = ic->streams[si]->codecpar;
        if (!fix_ts(pkt)) { av_packet_unref(pkt); continue; }
        double t = ts_sec(pkt);
        if (si == vidx) {
            if (!got_key && !(pkt->flags & AV_PKT_FLAG_KEY)) { av_packet_unref(pkt); continue; }
            if (!got_key && need_idr && (unsigned)(hevc_first_vcl(pkt) - 19) > 1) { av_packet_unref(pkt); continue; }
            got_key = 1;
            // Report random access points, so the page can drop played data up to one of them.
            // A SourceBuffer remove also drops everything up to the next point it recognizes.
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                int type = idr_only ? hevc_first_vcl(pkt) : 19;
                if (type == 19 || type == 20) js_rap(t);
            }
            if (skip_rasl && drop_rasl(pkt)) { av_packet_unref(pkt); continue; }
            if (strip_dv) strip_dv_nals(pkt);
            mux(&vout, pkt);
        } else if (si == aidx) {
            if (adec) decode_audio(pkt);
            else mux(&aout, pkt);
        } else if (si == bmp_idx) {
            decode_bitmap(pkt);
        } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE && is_text(par->codec_id)) {
            double dur = pkt->duration > 0 ? pkt->duration * av_q2d(ic->streams[si]->time_base) : 5;
            js_text(si, t, t + dur, pkt->data, pkt->size);
        }
        av_packet_unref(pkt);
        // Only the main stream's clock counts; subtitles can be stored far from their timestamps
        if (si == (vidx >= 0 ? vidx : aidx)) {
            if (t0 < 0) t0 = t;
            else if (t - t0 >= max_sec) break;
        }
        if (avio_tell(ic->pb) - pos0 >= max_bytes) break;
    }
    if (vout.oc) avio_flush(vout.oc->pb);
    if (aout.oc) avio_flush(aout.oc->pb);
    return err;
}
