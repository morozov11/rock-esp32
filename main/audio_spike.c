// RE-3 build-only component spike. The audio and speech recognition
// components pinned in idf_component.yml are prebuilt archives: without a
// symbol reference the linker's garbage collection drops them and the image
// size would not prove they fit the OTA slot. This translation unit roots one
// public symbol per component through a `used` constant table. It defines no
// constructor and no code path ever reads it, so firmware behavior is
// unchanged; the audio path itself is RE-4 and wake word is RE-8.
#include <stddef.h>

#include "esp_afe_sr_models.h"
#include "model_path.h"

#include "esp_codec_dev.h"

#include "esp_audio_dec.h"
#include "esp_audio_simple_dec.h"

// Global (not static) on purpose: main/CMakeLists.txt passes
// `-u rock_re3_component_roots` to the linker, which is what actually pulls
// this translation unit out of libmain.a and roots the table below.
const void *const rock_re3_component_roots[] = {
    (const void *)esp_afe_handle_from_config,
    (const void *)esp_srmodel_init,
    (const void *)esp_codec_dev_new,
    (const void *)esp_codec_dev_read,
    (const void *)esp_codec_dev_write,
    (const void *)esp_audio_dec_open,
    (const void *)esp_audio_simple_dec_open,
};
