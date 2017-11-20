#pragma once

#include "model.h"
#include <catboost/libs/helpers/exception.h>


inline void OneHotBinsFromTransposedCatFeatures(
    const TVector<TOneHotFeature>& OneHotFeatures,
    const yhash<int, int> catFeaturePackedIndex,
    const size_t docCount,
    TVector<ui8>& result,
    TVector<int>& transposedHash,
    size_t& currentBinIndex) {
    for (const auto& oheFeature : OneHotFeatures) {
        const auto catIdx = catFeaturePackedIndex.at(oheFeature.CatFeatureIndex);
        for (size_t docId = 0; docId < docCount; ++docId) {
            const auto val = transposedHash[catIdx * docCount + docId];
            auto writeIdx = docId + currentBinIndex;
            for (size_t borderIdx = 0; borderIdx < oheFeature.Values.size(); ++borderIdx) {
                result[writeIdx] = (ui8)(val == oheFeature.Values[borderIdx]);
                writeIdx += docCount;
            }
        }
        currentBinIndex += oheFeature.Values.size() * docCount;
    }
}

inline void BinarizeFloatCtrs(
    const TVector<TCtrFeature>& ctrFeatures,
    const size_t docCount,
    size_t currentBinIndex,
    const TVector<float>& ctrs,
    TVector<ui8>& result) {
    const auto docCount4 = (docCount | 0x3) ^0x3;
    for (size_t i = 0; i < ctrFeatures.size(); ++i) {
        const auto& ctr = ctrFeatures[i];
        auto ctrFloatsPtr = &ctrs[i * docCount];
        for (size_t docId = 0; docId < docCount4; docId += 4) {
            const float val[4] = {
                ctrFloatsPtr[docId + 0],
                ctrFloatsPtr[docId + 1],
                ctrFloatsPtr[docId + 2],
                ctrFloatsPtr[docId + 3]
            };

            auto writePtr = &result[docId + currentBinIndex];
            for (const auto border : ctr.Borders) {
                writePtr[0] = (ui8)(val[0] > border);
                writePtr[1] = (ui8)(val[1] > border);
                writePtr[2] = (ui8)(val[2] > border);
                writePtr[3] = (ui8)(val[3] > border);
                writePtr += docCount;
            }
        }
        for (size_t docId = docCount4; docId < docCount; ++docId) {
            const auto val = ctrFloatsPtr[docId];
            auto writePtr = &result[docId + currentBinIndex];
            for (const auto border : ctr.Borders) {
                *writePtr = (ui8)(val > border);
                writePtr += docCount;
            }
        }
        currentBinIndex += ctr.Borders.size() * docCount;
    }
}

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void BinarizeFeatures(
    const TFullModel& model,
    TFloatFeatureAccessor floatAccessor,
    TCatFeatureAccessor catFeatureAccessor,
    size_t start,
    size_t end,
    TVector<ui8>& result,
    TVector<int>& transposedHash,
    TVector<float>& ctrs
) {
    const auto docCount = end - start;
    const auto docCount4 = (docCount | 0x3) ^ 0x3;
    size_t currentBinIndex = 0;
    for (const auto& floatFeature : model.ObliviousTrees.FloatFeatures) {
        for (size_t docId = 0; docId < docCount4; docId += 4) {
            const float val[4] =
                {
                    floatAccessor(floatFeature, start + docId + 0),
                    floatAccessor(floatFeature, start + docId + 1),
                    floatAccessor(floatFeature, start + docId + 2),
                    floatAccessor(floatFeature, start + docId + 3)
                };
            auto writePtr = &result[docId + currentBinIndex];
            for (const auto border : floatFeature.Borders) {
                writePtr[0] = (ui8)(val[0] > border);
                writePtr[1] = (ui8)(val[1] > border);
                writePtr[2] = (ui8)(val[2] > border);
                writePtr[3] = (ui8)(val[3] > border);
                writePtr += docCount;
            }
        }
        for (size_t docId = docCount4; docId < docCount; ++docId) {
            const auto val = floatAccessor(floatFeature, start + docId);
            auto writePtr = &result[docId + currentBinIndex];
            for (const auto border : floatFeature.Borders) {
                *writePtr = (ui8)(val > border);
                writePtr += docCount;
            }
        }
        currentBinIndex += floatFeature.Borders.size() * docCount;
    }
    auto catFeatureCount = model.ObliviousTrees.CatFeatures.size();
    if (catFeatureCount > 0) {
        for (size_t docId = 0; docId < docCount; ++docId) {
            auto idx = docId;
            for (size_t i = 0; i < catFeatureCount; ++i) {
                transposedHash[idx] = catFeatureAccessor(i, start + docId);
                idx += docCount;
            }
        }
        yhash<int, int> catFeaturePackedIndexes;
        for (int i = 0; i < model.ObliviousTrees.CatFeatures.ysize(); ++i) {
            catFeaturePackedIndexes[model.ObliviousTrees.CatFeatures[i].FeatureIndex] = i;
        }
        OneHotBinsFromTransposedCatFeatures(model.ObliviousTrees.OneHotFeatures, catFeaturePackedIndexes, docCount, result, transposedHash, currentBinIndex);
        model.CtrProvider->CalcCtrs(
            model.ObliviousTrees.GetUsedModelCtrs(),
            result,
            transposedHash,
            docCount,
            ctrs
        );
        BinarizeFloatCtrs(model.ObliviousTrees.CtrFeatures, docCount, currentBinIndex, ctrs, result);
    }
}


inline void CalcTrees(
    const TFullModel& model,
    size_t blockStart,
    const TVector<ui8>& binFeatures,
    const unsigned long docCountInBlock,
    TVector<ui32>& indexesVec,
    size_t treeStart,
    size_t treeEnd,
    TArrayRef<double>& results)
{
    const auto docCountInBlock4 = (docCountInBlock | 0x3) ^0x3;
    const int* treeSplitsCurPtr =
        model.ObliviousTrees.TreeSplits.data() +
            model.ObliviousTrees.TreeStartOffsets[treeStart];
    for (size_t treeId = treeStart; treeId < treeEnd; ++treeId) {
        auto curTreeSize = model.ObliviousTrees.TreeSizes[treeId];
        memset(indexesVec.data(), 0, sizeof(ui32) * docCountInBlock);
        for (int depth = 0; depth < curTreeSize; ++depth) {
            auto indexesPtr = indexesVec.data();
            const auto bin = treeSplitsCurPtr[depth];
            auto binFeaturePtr = &binFeatures[bin * docCountInBlock];
            for (size_t docId = 0; docId < docCountInBlock; ++docId) {
                indexesPtr[docId] |= binFeaturePtr[docId] << depth;
            }
        }
        auto treeLeafPtr = model.ObliviousTrees.LeafValues[treeId].data();
        if (model.ObliviousTrees.ApproxDimension == 1) { // single class model
            auto indexesPtr = indexesVec.data();
            auto writePtr = &results[blockStart];
            for (size_t docId = 0; docId < docCountInBlock4; docId += 4) {
                writePtr[0] += treeLeafPtr[indexesPtr[0]];
                writePtr[1] += treeLeafPtr[indexesPtr[1]];
                writePtr[2] += treeLeafPtr[indexesPtr[2]];
                writePtr[3] += treeLeafPtr[indexesPtr[3]];
                writePtr += 4;
                indexesPtr += 4;
            }
            for (size_t docId = docCountInBlock4; docId < docCountInBlock; ++docId) {
                *writePtr += treeLeafPtr[*indexesPtr];
                ++writePtr;
                ++indexesPtr;
            }
        } else { // mutliclass model
            auto indexesPtr = indexesVec.data();
            auto docResultPtr = &results[blockStart * model.ObliviousTrees.ApproxDimension];
            for (size_t docId = 0; docId < docCountInBlock; ++docId) {
                auto leafValuePtr = treeLeafPtr + indexesPtr[docId] * model.ObliviousTrees.ApproxDimension;
                for (int classId = 0; classId < model.ObliviousTrees.ApproxDimension; ++classId) {
                    docResultPtr[classId] += leafValuePtr[classId];
                }
                docResultPtr += model.ObliviousTrees.ApproxDimension;
            }
        }
        treeSplitsCurPtr += curTreeSize;
    }
}

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline void CalcGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t treeStart,
    size_t treeEnd,
    TArrayRef<double> results)
{
    size_t blockSize;
    if (model.ObliviousTrees.CtrFeatures.empty()) {
        blockSize = 128;
    } else {
        blockSize = 4096;
    }
    blockSize = Min(blockSize, docCount);
    CB_ENSURE(results.size() == docCount * model.ObliviousTrees.ApproxDimension);
    std::fill(results.begin(), results.end(), 0.0);
    TVector<ui8> binFeatures(model.ObliviousTrees.GetBinaryFeaturesCount() * blockSize);
    TVector<ui32> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        CalcTrees(
            model,
            blockStart,
            binFeatures,
            docCountInBlock,
            indexesVec,
            treeStart,
            treeEnd,
            results
        );
    }
}

template<typename TFloatFeatureAccessor, typename TCatFeatureAccessor>
inline TVector<TVector<double>> CalcTreeIntervalsGeneric(
    const TFullModel& model,
    TFloatFeatureAccessor floatFeatureAccessor,
    TCatFeatureAccessor catFeaturesAccessor,
    size_t docCount,
    size_t incrementStep)
{
    size_t blockSize;
    if (model.ObliviousTrees.CtrFeatures.empty()) {
        blockSize = 128;
    } else {
        blockSize = 4096;
    }
    blockSize = Min(blockSize, docCount);
    auto treeStepCount = (model.ObliviousTrees.TreeSizes.size() + incrementStep - 1) / incrementStep;
    TVector<TVector<double>> results(docCount, TVector<double>(treeStepCount));
    CB_ENSURE(model.ObliviousTrees.ApproxDimension == 1);
    TVector<ui8> binFeatures(model.ObliviousTrees.GetBinaryFeaturesCount() * blockSize);
    TVector<ui32> indexesVec(blockSize);
    TVector<int> transposedHash(blockSize * model.ObliviousTrees.CatFeatures.size());
    TVector<float> ctrs(model.ObliviousTrees.GetUsedModelCtrs().size() * blockSize);
    TVector<double> tmpResult(docCount);
    TArrayRef<double> tmpResultRef(tmpResult);
    for (size_t blockStart = 0; blockStart < docCount; blockStart += blockSize) {
        const auto docCountInBlock = Min(blockSize, docCount - blockStart);
        BinarizeFeatures(
            model,
            floatFeatureAccessor,
            catFeaturesAccessor,
            blockStart,
            blockStart + docCountInBlock,
            binFeatures,
            transposedHash,
            ctrs
        );
        for (size_t stepIdx = 0; stepIdx < treeStepCount; ++stepIdx) {
            CalcTrees(
                model,
                blockStart,
                binFeatures,
                docCountInBlock,
                indexesVec,
                stepIdx * incrementStep,
                Min((stepIdx + 1) * incrementStep, model.ObliviousTrees.TreeSizes.size()),
                tmpResultRef
            );
            for (size_t i = 0; i < docCountInBlock; ++i) {
                results[blockStart + i][stepIdx] = tmpResult[i];
            }
        }
    }
    return results;
}
