PROGRAM(cb_cuda)

NO_WERROR()



SRCS(
    mode_fit.cpp
    main.cpp
)

PEERDIR(
    library/getopt
    catboost/cuda/train_lib
    catboost/cuda/cuda_lib
    catboost/cuda/cuda_util
    catboost/cuda/data
    catboost/cuda/ctrs
    catboost/cuda/gpu_data
    catboost/cuda/methods
    catboost/cuda/models
    catboost/cuda/targets
    catboost/cuda/cpu_compatibility_helpers
    catboost/libs/model
    catboost/libs/logging
    catboost/libs/data
    catboost/libs/algo
    library/grid_creator
)

ALLOCATOR(LF)

CUDA_NVCC_FLAGS(
    --expt-relaxed-constexpr
    -std=c++11
    -gencode arch=compute_35,code=sm_35
    -gencode arch=compute_52,code=sm_52
    -gencode arch=compute_60,code=sm_60
    -gencode arch=compute_60,code=sm_60
    -gencode arch=compute_61,code=compute_61
    --ptxas-options=-v
)

END()
