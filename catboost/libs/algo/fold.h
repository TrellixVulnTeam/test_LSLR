#pragma once

#include "approx_util.h"
#include "restorable_rng.h"
#include "target_classifier.h"
#include "train_data.h"
#include "online_ctr.h"
#include <catboost/libs/model/online_ctr.h>
#include <catboost/libs/helpers/clear_array.h>
#include <catboost/libs/model/projection.h>
#include <catboost/libs/data/pair.h>

#include <util/generic/vector.h>
#include <util/random/shuffle.h>
#include <util/generic/ymath.h>

static yvector<int> InvertPermutation(const yvector<int>& permutation) {
    yvector<int> result(permutation.size());
    for (size_t i = 0; i < permutation.size(); ++i) {
        result[permutation[i]] = i;
    }
    return result;
}

static int UpdateSizeForQueries(int size, const yvector<int>& queriesFinishIndex) {
    if (!queriesFinishIndex.empty()) {
        double meanQuerySize = queriesFinishIndex.back() / (queriesFinishIndex.ysize() + 1);
        int queryIndex = Min(static_cast<int>(ceil(size / meanQuerySize)) - 1, queriesFinishIndex.ysize() - 1);
        while (queriesFinishIndex[queryIndex] < size && queryIndex < queriesFinishIndex.ysize() - 1) {
            ++queryIndex;
        }
        size = queriesFinishIndex[queryIndex];
    }
    return size;
}

static int SelectMinBatchSize(const int sampleCount, const yvector<int>& queriesFinishIndex) {
    int size = sampleCount > 500 ? Min<int>(100, sampleCount / 50) : 1;
    return UpdateSizeForQueries(size, queriesFinishIndex);
}

static double SelectTailSize(const int oldSize, const double multiplier, const yvector<int>& queriesFinishIndex) {
    int size = ceil(oldSize * multiplier);
    return UpdateSizeForQueries(size, queriesFinishIndex);
}

struct TFold {
    struct TBodyTail {
        yvector<yvector<double>> Approx;
        yvector<yvector<double>> Derivatives;
        // TODO(annaveronika): make a single vector<vector> for all BodyTail
        yvector<yvector<double>> WeightedDer;
        yvector<yvector<TCompetitor>> Competitors;

        int BodyFinish;
        int TailFinish;

        TBodyTail()
            : BodyFinish(0)
            , TailFinish(0)
        {
        }
    };

    yvector<float> LearnWeights;
    yvector<ui32> LearnQueryId;
    yhash<ui32, ui32> LearnQuerySize;
    yvector<int> LearnPermutation; // index in original array
    yvector<TBodyTail> BodyTailArr;
    yvector<float> LearnTarget;
    yvector<float> SampleWeights;
    yvector<yvector<int>> LearnTargetClass;
    yvector<int> TargetClassesCount;
    size_t EffectiveDocCount = 0;

    TOnlineCTRHash& GetCtrs(const TProjection& proj) {
        return HasSingleFeature(proj) ? OnlineSingleCtrs : OnlineCTR;
    }

    const TOnlineCTRHash& GetCtrs(const TProjection& proj) const {
        return HasSingleFeature(proj) ? OnlineSingleCtrs : OnlineCTR;
    }

    TOnlineCTR& GetCtrRef(const TProjection& proj) {
        return GetCtrs(proj)[proj];
    }

    const TOnlineCTR& GetCtr(const TProjection& proj) const {
        return GetCtrs(proj).at(proj);
    }

    void DropEmptyCTRs() {
        yvector<TProjection> emptyProjections;
        for (auto& projCtr : OnlineSingleCtrs) {
            if (projCtr.second.Feature.empty()) {
                emptyProjections.emplace_back(projCtr.first);
            }
        }
        for (auto& projCtr : OnlineCTR) {
            if (projCtr.second.Feature.empty()) {
                emptyProjections.emplace_back(projCtr.first);
            }
        }
        for (const auto& proj : emptyProjections) {
            GetCtrs(proj).erase(proj);
        }
    }

    void AssignTarget(const yvector<float>& target,
                      const yvector<TTargetClassifier>& targetClassifiers) {
        AssignPermuted(target, &LearnTarget);
        int learnSampleCount = LearnPermutation.ysize();

        int ctrCount = targetClassifiers.ysize();
        LearnTargetClass.assign(ctrCount, yvector<int>(learnSampleCount));
        TargetClassesCount.resize(ctrCount);
        for (int ctrIdx = 0; ctrIdx < ctrCount; ++ctrIdx) {
            for (int z = 0; z < learnSampleCount; ++z) {
                LearnTargetClass[ctrIdx][z] = targetClassifiers[ctrIdx].GetTargetClass(LearnTarget[z]);
            }
            TargetClassesCount[ctrIdx] = targetClassifiers[ctrIdx].GetClassesCount();
        }
    }

    template <typename T>
    void AssignPermuted(const yvector<T>& source, yvector<T>* dest) const {
        int learnSampleCount = LearnPermutation.ysize();
        yvector<T>& destination = *dest;
        destination.resize(learnSampleCount);
        for (int z = 0; z < learnSampleCount; ++z) {
            int i = LearnPermutation[z];
            destination[z] = source[i];
        }
    }

    void AssignCompetitors(const yvector<TPair>& pairs,
                           const yvector<int>& invertPermutation,
                           TFold::TBodyTail* bt) {
        int learnSampleCount = LearnPermutation.ysize();
        int bodyFinish = bt->BodyFinish;
        int tailFinish = bt->TailFinish;
        yvector<yvector<TCompetitor>>& competitors = bt->Competitors;
        competitors.resize(tailFinish);
        for (const auto& pair : pairs) {
            if (pair.WinnerId >= learnSampleCount || pair.LoserId >= learnSampleCount) {
                continue;
            }

            int winnerId = invertPermutation[pair.WinnerId];
            int loserId = invertPermutation[pair.LoserId];

            if (winnerId >= tailFinish || loserId >= tailFinish) {
                continue;
            }

            float weight = 1;

            if (winnerId < bodyFinish || winnerId > loserId) {
                competitors[winnerId].emplace_back(loserId, weight);
            }
            if (loserId < bodyFinish || loserId > winnerId) {
                competitors[loserId].emplace_back(winnerId, -weight);
            }
        }
    }

    int GetApproxDimension() const {
        return BodyTailArr[0].Approx.ysize();
    }

    void TrimOnlineCTR(size_t maxOnlineCTRFeatures) {
        if (OnlineCTR.size() > maxOnlineCTRFeatures) {
            OnlineCTR.clear();
        }
    }

    void SaveApproxes(IOutputStream* s) const {
        const ui64 bodyTailCount = BodyTailArr.size();
        ::Save(s, bodyTailCount);
        for (ui64 i = 0; i < bodyTailCount; ++i) {
            ::Save(s, BodyTailArr[i].Approx);
        }
    }
    void LoadApproxes(IInputStream* s) {
        ui64 bodyTailCount;
        ::Load(s, bodyTailCount);
        CB_ENSURE(bodyTailCount == BodyTailArr.size());
        for (ui64 i = 0; i < bodyTailCount; ++i) {
            ::Load(s, BodyTailArr[i].Approx);
        }
    }
private:
    TOnlineCTRHash OnlineSingleCtrs;
    TOnlineCTRHash OnlineCTR;
    static bool HasSingleFeature(const TProjection& proj) {
        return proj.BinFeatures.ysize() + proj.CatFeatures.ysize() == 1;
    }
};

static void InitFromBaseline(const int beginIdx, const int endIdx,
                        const yvector<yvector<double>>& baseline,
                        const yvector<int>& learnPermutation,
                        bool storeExpApproxes,
                        yvector<yvector<double>>* approx) {
    const int learnSampleCount = learnPermutation.ysize();
    const int approxDimension = approx->ysize();
    for (int dim = 0; dim < approxDimension; ++dim) {
        yvector<double> tempBaseline(baseline[dim]);
        ExpApproxIf(storeExpApproxes, &tempBaseline);
        for (int docId = beginIdx; docId < endIdx; ++docId) {
            int initialIdx = docId;
            if (docId < learnSampleCount) {
                initialIdx = learnPermutation[docId];
            }
            (*approx)[dim][docId] = tempBaseline[initialIdx];
        }
    }
}

static yvector<int> CalcQueriesFinishIndex(const yvector<ui32>& queriesId) {
    yvector<int> queriesFinishIndex;
    ui32 lastQueryId = queriesId[0];
    for (int docId = 0; docId < queriesId.ysize(); ++docId) {
        if (queriesId[docId] != lastQueryId) {
            queriesFinishIndex.push_back(docId);
            lastQueryId = queriesId[docId];
        }
    }
    queriesFinishIndex.push_back(queriesId.ysize());
    return queriesFinishIndex;
}

inline TFold BuildLearnFold(const TTrainData& data,
                            const yvector<TTargetClassifier>& targetClassifiers,
                            bool shuffle,
                            int permuteBlockSize,
                            int approxDimension,
                            double multiplier,
                            bool storeExpApproxes,
                            bool isQuerywiseError,
                            TRestorableFastRng64& rand) {
    TFold ff;
    ff.LearnPermutation.resize(data.LearnSampleCount);
    std::iota(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), 0);
    if (shuffle) {
        if (permuteBlockSize == 1) { // shortcut for speed
            Shuffle(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), rand);
        } else {
            const int blocksCount = (data.LearnSampleCount + permuteBlockSize - 1) / permuteBlockSize;
            yvector<int> blockedPermute(blocksCount);
            std::iota(blockedPermute.begin(), blockedPermute.end(), 0);
            Shuffle(blockedPermute.begin(), blockedPermute.end(), rand);

            int currentIdx = 0;
            for (int i = 0; i < blocksCount; ++i) {
                const int blockStartIdx = blockedPermute[i] * permuteBlockSize;
                const int blockEndIndx = Min(blockStartIdx + permuteBlockSize, data.LearnSampleCount);
                for (int j = blockStartIdx; j < blockEndIndx; ++j) {
                    ff.LearnPermutation[currentIdx + j - blockStartIdx] = j;
                }
                currentIdx += blockEndIndx - blockStartIdx;
            }
        }
    }

    ff.AssignTarget(data.Target, targetClassifiers);

    if (!data.Weights.empty()) {
        ff.AssignPermuted(data.Weights, &ff.LearnWeights);
    }

    if (!data.QueryId.empty()) {
        ff.AssignPermuted(data.QueryId, &ff.LearnQueryId);
        ff.LearnQuerySize = data.QuerySize;
    }

    yvector<int> queriesFinishIndex;
    if (isQuerywiseError) {
        CB_ENSURE(!ff.LearnQueryId.empty(), "If loss-function is Querywise then pool should have column with QueryId");
        queriesFinishIndex = CalcQueriesFinishIndex(ff.LearnQueryId);
    }

    ff.EffectiveDocCount = data.LearnSampleCount;

    yvector<int> invertPermutation = InvertPermutation(ff.LearnPermutation);

    int leftPartLen = SelectMinBatchSize(data.LearnSampleCount, queriesFinishIndex);
    while (ff.BodyTailArr.empty() || leftPartLen < data.LearnSampleCount) {
        TFold::TBodyTail bt;
        bt.BodyFinish = leftPartLen;
        bt.TailFinish = Min(SelectTailSize(leftPartLen, multiplier, queriesFinishIndex), ff.LearnPermutation.ysize() + 0.);
        bt.Approx.resize(approxDimension, yvector<double>(bt.TailFinish, GetNeutralApprox(storeExpApproxes)));
        if (!data.Baseline.empty()) {
            InitFromBaseline(leftPartLen, bt.TailFinish, data.Baseline, ff.LearnPermutation, storeExpApproxes, &bt.Approx);
        }
        bt.Derivatives.resize(approxDimension, yvector<double>(bt.TailFinish));
        bt.WeightedDer.resize(approxDimension, yvector<double>(bt.TailFinish));
        ff.AssignCompetitors(data.Pairs, invertPermutation, &bt);
        ff.BodyTailArr.emplace_back(std::move(bt));
        leftPartLen = bt.TailFinish;
    }
    return ff;
}

inline TFold BuildAveragingFold(const TTrainData& data,
                                const yvector<TTargetClassifier>& targetClassifiers,
                                bool shuffle,
                                int approxDimension,
                                bool storeExpApproxes,
                                TRestorableFastRng64& rand) {
    TFold ff;
    ff.LearnPermutation.resize(data.LearnSampleCount);
    std::iota(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), 0);

    if (shuffle) {
        Shuffle(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), rand);
    }

    ff.AssignTarget(data.Target, targetClassifiers);

    if (!data.Weights.empty()) {
        ff.AssignPermuted(data.Weights, &ff.LearnWeights);
    }

    if (!data.QueryId.empty()) {
        ff.AssignPermuted(data.QueryId, &ff.LearnQueryId);
        ff.LearnQuerySize = data.QuerySize;
    }

    ff.EffectiveDocCount = data.GetSampleCount();

    yvector<int> invertPermutation = InvertPermutation(ff.LearnPermutation);

    TFold::TBodyTail bt;
    bt.BodyFinish = data.LearnSampleCount;
    bt.TailFinish = data.LearnSampleCount;
    bt.Approx.resize(approxDimension, yvector<double>(data.GetSampleCount(), GetNeutralApprox(storeExpApproxes)));
    bt.WeightedDer.resize(approxDimension, yvector<double>(data.GetSampleCount()));
    if (!data.Baseline.empty()) {
        InitFromBaseline(0, data.GetSampleCount(), data.Baseline, ff.LearnPermutation, storeExpApproxes, &bt.Approx);
    }
    ff.AssignCompetitors(data.Pairs, invertPermutation, &bt);
    ff.BodyTailArr.emplace_back(std::move(bt));
    return ff;
}
