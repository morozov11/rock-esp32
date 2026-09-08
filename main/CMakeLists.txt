# The vendored esp-idf-sys generates bindings for a broad slice of the
# ESP-IDF API, and its build script parses those headers with the include
# directories of THIS component. Every component named here exists so that
# bindgen can resolve the driver/adc/wifi/nvs headers the bindings pull in;
# the Rust static library is the actual consumer.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        "heap"
        "pthread"
        "driver"
        "esp_adc"
        "esp_netif"
        "esp_wifi"
        "wpa_supplicant"
        "nvs_flash"
        "sdmmc"
        "esp_driver_ana_cmpr"
        "esp_driver_dac"
        "esp_driver_gpio"
        "esp_driver_gptimer"
        "esp_driver_i2c"
        "esp_driver_i2s"
        "esp_driver_isp"
        "esp_driver_jpeg"
        "esp_driver_ledc"
        "esp_driver_mcpwm"
        "esp_driver_parlio"
        "esp_driver_pcnt"
        "esp_driver_rmt"
        "esp_driver_sdio"
        "esp_driver_sdmmc"
        "esp_driver_sdspi"
        "esp_driver_sdm"
        "esp_driver_touch_sens"
        "esp_driver_tsens"
        "esp_driver_twai"
        "esp_driver_uart"
        "esp_driver_usb_serial_jtag"
)

include(ExternalProject)

set(RUST_TARGET "riscv32imafc-esp-espidf")
set(RUST_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
# esp-idf-sys rejects long Windows output paths; keep generated Cargo files short.
set(RUST_TARGET_DIR "C:/r")
set(RUST_LIBRARY "${RUST_TARGET_DIR}/${RUST_TARGET}/release/librock_esp32.a")

idf_build_get_property(RUST_SDKCONFIG SDKCONFIG)
idf_build_get_property(RUST_IDF_PATH IDF_PATH)
get_filename_component(RUST_LIBCLANG_PATH "$ENV{ESP_CLANG_LIBS_PATH}/../bin" ABSOLUTE)

# libclang.dll only ships in the esp-clang-libs package, which the IDF
# activation profile does not put on PATH. cargo runs through the
# rust-build.bat wrapper, which prepends the clang bin dir to PATH: passing a
# semicolon-separated PATH value straight through ExternalProject is not
# possible because it splits arguments on semicolons when writing the batch
# file, so everything cargo needs is passed as individual env vars instead.
set(RUST_CARGO_ARGS "--manifest-path ${RUST_PROJECT_DIR}/Cargo.toml --target ${RUST_TARGET} --target-dir ${RUST_TARGET_DIR} --release -Zbuild-std=std,panic_abort")

ExternalProject_Add(
    rock_rust_build
    PREFIX "${RUST_TARGET_DIR}"
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TARGET_DIR=${RUST_TARGET_DIR}
        CARGO_CMAKE_BUILD_INCLUDES=$<TARGET_PROPERTY:${COMPONENT_LIB},INCLUDE_DIRECTORIES>
        CARGO_CMAKE_BUILD_LINK_LIBRARIES=$<TARGET_PROPERTY:${COMPONENT_LIB},LINK_LIBRARIES>
        CARGO_CMAKE_BUILD_SDKCONFIG=${RUST_SDKCONFIG}
        CARGO_CMAKE_BUILD_ESP_IDF=${RUST_IDF_PATH}
        CARGO_CMAKE_BUILD_COMPILER=${CMAKE_C_COMPILER}
        LIBCLANG_PATH=${RUST_LIBCLANG_PATH}
        RUSTFLAGS=--cfg\ espidf_time64
        MCU=${CONFIG_IDF_TARGET}
        RUST_CLANG_BIN=${RUST_LIBCLANG_PATH}
        "RUST_CARGO_ARGS=${RUST_CARGO_ARGS}"
        cmd /c "${RUST_PROJECT_DIR}/main/rust-build.bat"
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE
    SOURCE_DIR "${RUST_PROJECT_DIR}"
    BINARY_DIR "${RUST_PROJECT_DIR}"
    USES_TERMINAL
    BUILD_BYPRODUCTS "${RUST_LIBRARY}"
)

add_prebuilt_library(rock_rust "${RUST_LIBRARY}")
add_dependencies(rock_rust rock_rust_build)
target_link_libraries(${COMPONENT_LIB} PRIVATE rock_rust)
