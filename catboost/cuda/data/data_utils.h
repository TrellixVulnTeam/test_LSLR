#pragma once

#include "grid_creator.h"
#include "binarization_config.h"

#include <catboost/cuda/cuda_util/cpu_random.h>
#include <library/grid_creator/binarization.h>
#include <util/system/types.h>
#include <util/generic/vector.h>

namespace NCatboostCuda
{
    void GroupQueries(const TVector<ui32>& qid, TVector<TVector<ui32>>* qdata);

    template<class T>
    TVector<T> SampleVector(const TVector<T>& vec,
                            ui32 size,
                            ui64 seed)
    {
        TRandom random(seed);
        TVector<T> result(size);
        for (ui32 i = 0; i < size; ++i)
        {
            result[i] = vec[(random.NextUniformL() % vec.size())];
        }
        return result;
    };


    inline ui32 GetSampleSizeForBorderSelectionType(ui32 vecSize,
                                                    EBorderSelectionType borderSelectionType)
    {
        switch (borderSelectionType)
        {
            case EBorderSelectionType::MinEntropy:
            case EBorderSelectionType::MaxLogSum:
                return Min<ui32>(vecSize, 100000);
            default:
                return vecSize;
        }
    };

    template<class T>
    void ApplyPermutation(const TVector<ui32>& order,
                          TVector<T>& data)
    {
        if (data.size())
        {
            TVector<T> tmp(data.begin(), data.end());
            for (ui32 i = 0; i < order.size(); ++i)
            {
                data[i] = tmp[order[i]];
            }
        }
    };

    inline TVector<float> BuildBorders(const TVector<float>& floatFeature,
                                       const ui32 seed,
                                       const TBinarizationDescription& config)
    {
        TOnCpuGridBuilderFactory gridBuilderFactory;
        ui32 sampleSize = GetSampleSizeForBorderSelectionType(floatFeature.size(),
                                                              config.BorderSelectionType);
        if (sampleSize < floatFeature.size())
        {
            auto sampledValues = SampleVector(floatFeature, sampleSize, TRandom::GenerateSeed(seed));
            return TBordersBuilder(gridBuilderFactory, sampledValues)(config);
        } else
        {
            return TBordersBuilder(gridBuilderFactory, floatFeature)(config);
        }
    };
}
