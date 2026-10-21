# libwebp v1.6.0, decoder only: https://chromium.googlesource.com/webm/libwebp/+/refs/tags/v1.6.0
file(GLOB WEBP_DEC_SOURCES "upstream/src/dec/*.c")
idf_component_register(
    SRCS ${WEBP_DEC_SOURCES}
        "upstream/src/dsp/alpha_processing.c"
        "upstream/src/dsp/cpu.c"
        "upstream/src/dsp/dec.c"
        "upstream/src/dsp/dec_clip_tables.c"
        "upstream/src/dsp/filters.c"
        "upstream/src/dsp/lossless.c"
        "upstream/src/dsp/rescaler.c"
        "upstream/src/dsp/upsampling.c"
        "upstream/src/dsp/yuv.c"
        "upstream/src/utils/bit_reader_utils.c"
        "upstream/src/utils/color_cache_utils.c"
        "upstream/src/utils/filters_utils.c"
        "upstream/src/utils/huffman_utils.c"
        "upstream/src/utils/palette.c"
        "upstream/src/utils/quant_levels_dec_utils.c"
        "upstream/src/utils/rescaler_utils.c"
        "upstream/src/utils/random_utils.c"
        "upstream/src/utils/thread_utils.c"
        "upstream/src/utils/utils.c"
    INCLUDE_DIRS "upstream/src"
    PRIV_INCLUDE_DIRS "upstream"
)
