#pragma once

#include <catboost/libs/params/params.h>
#include <catboost/libs/model/target_classifier.h>

#include <util/generic/vector.h>

#include <library/grid_creator/binarization.h>

TTargetClassifier BuildTargetClassifier(const TVector<float>& target,
                                        int learnSampleCount,
                                        ELossFunction loss,
                                        const TMaybe<TCustomObjectiveDescriptor>& objectiveDescriptor,
                                        int targetBorderCount,
                                        EBorderSelectionType targetBorderType);
