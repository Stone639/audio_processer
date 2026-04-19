idf_component_register(
    SRCS "voice_recognition.c" "wav_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)