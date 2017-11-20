IF (PYTHON_CONFIG MATCHES "python3")
    PYMODULE(_catboost EXPORTS catboost3.exports PREFIX "")
ELSE()
    PYMODULE(_catboost EXPORTS catboost.exports PREFIX "")
ENDIF()

USE_LINKER_GOLD()



PYTHON_ADDINCL()

IF(HAVE_CUDA)
    PEERDIR(
        catboost/cuda/train_lib
    )
ENDIF()

PEERDIR(
    catboost/libs/algo
    catboost/libs/data
    catboost/libs/fstr
    catboost/libs/helpers
    catboost/libs/logging
    catboost/libs/metrics
    catboost/libs/model
    catboost/libs/params
    library/containers/2d_array
    library/json/writer
)

SRCS(helpers.cpp)

BUILDWITH_CYTHON_CPP(
    _catboost.pyx
    --module-name _catboost
)

END()
