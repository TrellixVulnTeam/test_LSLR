UNITTEST(model_ut)



SRCS(
    formula_evaluator_ut.cpp
    model_serialization_ut.cpp
)

PEERDIR(
    catboost/libs/model
    catboost/libs/algo
)

END()
