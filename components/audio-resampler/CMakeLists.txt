idf_component_register(
    SRCS "resampler.c"
    INCLUDE_DIRS "."
)

# Optimise for ESP32-S3: single-precision FPU, fast math, max optimisation
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-calloc-transposed-args
    -O3
    -ffast-math
    -fsingle-precision-constant
    -fno-math-errno
)
