LIBRARY()

NO_WERROR()



SRCS(
    kernel/ctr_calcers.cu
    ctr_bins_builder.cpp
    ctr_calcers.cpp
    ctr.cpp
)

PEERDIR(
    contrib/libs/nvidia/cudalib
    catboost/cuda/cuda_lib
    catboost/libs/ctr_description
    catboost/cuda/cuda_util
)

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

GENERATE_ENUM_SERIALIZATION(ctr.h)


END()
