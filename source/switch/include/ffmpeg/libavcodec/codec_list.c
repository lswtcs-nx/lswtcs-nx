// Switch build: Xenia's audio decoders (xenia-canary/third_party/ffmpeg-xenia/libavcodec/codec_list.c) plus the
// WMV3/VC-1 video decoder for the title movie (log129).
static const FFCodec * const codec_list[] = {
    &ff_mp3float_decoder,
    &ff_mp3_decoder,
    &ff_wmapro_decoder,
    &ff_wmav2_decoder,
    &ff_xmaframes_decoder,
    &ff_vc1_decoder,
    &ff_wmv3_decoder,
    NULL };
