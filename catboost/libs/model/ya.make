LIBRARY()



SRCS(
    coreml_helpers.cpp
    ctr_data.cpp
    ctr_provider.cpp
    ctr_value_table.cpp
    features.cpp
    model.cpp
    online_ctr.cpp
    static_ctr_provider.cpp
    formula_evaluator.cpp
)

PEERDIR(
    catboost/libs/cat_feature
    catboost/libs/ctr_description
    catboost/libs/helpers
    contrib/libs/coreml
    library/binsaver
    library/containers/dense_hash
    catboost/libs/model/flatbuffers
    library/json
)

GENERATE_ENUM_SERIALIZATION(split.h)

END()
