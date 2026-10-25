set(frontend "third_party/pymicro/tensorflow/lite/experimental/microfrontend/lib")
set(frontend_sources
    kiss_fft_int16.cc fft.cc fft_util.cc filterbank.cc filterbank_util.cc
    frontend.cc frontend_util.cc log_lut.cc log_scale.cc log_scale_util.cc
    noise_reduction.cc noise_reduction_util.cc pcan_gain_control.cc
    pcan_gain_control_util.cc window.cc window_util.cc
)
list(TRANSFORM frontend_sources PREPEND "${frontend}/")

idf_component_register(
    SRCS "rock_wake_probe.cc" "rock_wake_features.cc" "rock_wake_runtime.cc"
         ${frontend_sources}
         "third_party/pymicro/kissfft/kiss_fft.cc"
         "third_party/pymicro/kissfft/tools/kiss_fftr.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "third_party/pymicro" "third_party/pymicro/kissfft"
    REQUIRES esp-tflite-micro
    PRIV_REQUIRES esp_timer heap rock_audio rock_platform
    EMBED_FILES "model/re8-rockserver.tflite"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE FIXED_POINT=16)
