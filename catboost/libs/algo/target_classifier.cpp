#include "target_classifier.h"

#include <library/grid_creator/binarization.h>

#include <util/generic/algorithm.h>

static TVector<float> GetMultiClassBorders(int cnt) {
    TVector<float> borders(cnt);
    for (int i = 0; i < cnt; ++i) {
        borders[i] = 0.5 + i;
    }
    return borders;
}

static TVector<float> SelectBorders(const TVector<float>& target, int learnSampleCount,
                                    int targetBorderCount, EBorderSelectionType targetBorderType) {
    TVector<float> learnTarget(target.begin(), target.begin() + learnSampleCount);

    THashSet<float> borderSet = BestSplit(learnTarget, targetBorderCount, targetBorderType);
    TVector<float> borders(borderSet.begin(), borderSet.end());
    CB_ENSURE(borders.ysize() > 0, "0 target borders");

    Sort(borders.begin(), borders.end());

    return borders;
}

TTargetClassifier BuildTargetClassifier(const TVector<float>& target,
                                        int learnSampleCount,
                                        ELossFunction loss,
                                        const TMaybe<TCustomObjectiveDescriptor>& objectiveDescriptor,
                                        int targetBorderCount,
                                        EBorderSelectionType targetBorderType) {
    if (targetBorderCount == 0) {
        return TTargetClassifier();
    }

    CB_ENSURE(learnSampleCount > 0, "train should not be empty");

    float min = *MinElement(target.begin(), target.begin() + learnSampleCount);
    float max = *MaxElement(target.begin(), target.begin() + learnSampleCount);
    CB_ENSURE(min != max, "target should not be constant");

    switch (loss) {
        case ELossFunction::RMSE:
        case ELossFunction::Quantile:
        case ELossFunction::LogLinQuantile:
        case ELossFunction::Poisson:
        case ELossFunction::MAE:
        case ELossFunction::MAPE:
        case ELossFunction::PairLogit:
        case ELossFunction::QueryRMSE:
        case ELossFunction::Logloss:
        case ELossFunction::CrossEntropy:
            return TTargetClassifier(SelectBorders(
                target,
                learnSampleCount,
                targetBorderCount,
                targetBorderType));

        case ELossFunction::MultiClass:
        case ELossFunction::MultiClassOneVsAll:
            return TTargetClassifier(GetMultiClassBorders(targetBorderCount));

        case ELossFunction::Custom: {
            Y_ASSERT(objectiveDescriptor.Defined());
            return TTargetClassifier(SelectBorders(
                target,
                learnSampleCount,
                targetBorderCount,
                targetBorderType));
        }

        default:
            CB_ENSURE(false, "provided error function is not supported");
    }
}
