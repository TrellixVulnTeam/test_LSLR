#pragma once

#include <catboost/cuda/cuda_util/compression_helpers.h>
#include <cmath>
#include <util/system/types.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/yexception.h>
#include <util/stream/buffer.h>
namespace NCatboostCuda
{
//feature values storage optimized for memory usage
    enum class EFeatureValuesType
    {
        Float,          //32 bits per feature value
        BinarizedFloat, //at most 8 bits per feature value. Contains grid
        Categorical,    //after perfect hashing.
        Zero,           //fake empty features.
    };

    class IFeatureValuesHolder
    {
    public:
        virtual ~IFeatureValuesHolder() = default;

        IFeatureValuesHolder(EFeatureValuesType type,
                             ui32 featureId,
                             ui64 size,
                             TString featureName = "")
                : Type(type)
                  , FeatureId(featureId)
                  , FeatureName(featureName)
                  , Size(size)
        {
        }

        EFeatureValuesType GetType() const
        {
            return Type;
        }

        ui32 GetSize() const
        {
            return Size;
        }

        const TString& GetName() const
        {
            return FeatureName;
        }

        ui32 GetId() const
        {
            return FeatureId;
        }

    private:
        EFeatureValuesType Type;
        ui32 FeatureId;
        TString FeatureName;
        ui64 Size;
    };

    using TFeatureColumnPtr = THolder<IFeatureValuesHolder>;

    class TZeroFeature: public IFeatureValuesHolder
    {
    public:
        explicit TZeroFeature(ui32 featureId, TString featureName = "")
                : IFeatureValuesHolder(EFeatureValuesType::Zero, featureId, 0, featureName)
        {
        }

        ui32 Discretization() const
        {
            return 0;
        }
    };


    class TCompressedValuesHolderImpl: public IFeatureValuesHolder
    {
    public:
        TCompressedValuesHolderImpl(EFeatureValuesType type,
                                    ui32 featureId,
                                    ui64 size,
                                    ui32 bitsPerKey,
                                    TVector<ui64>&& data,
                                    TString featureName = "")
                : IFeatureValuesHolder(type, featureId, size, std::move(featureName))
                  , Values(std::move(data))
                  , IndexHelper(bitsPerKey)
        {
        }

        ui32 GetValue(ui32 docId) const
        {
            return IndexHelper.Extract(Values, docId);
        }

        TVector<ui32> ExtractValues() const
        {
            TVector<ui32> dst;
            dst.clear();
            dst.resize(GetSize());

            NPar::ParallelFor(0, GetSize(), [&](int i)
            {
                dst[i] = GetValue(i);
            });

            return dst;
        }

    private:
        TVector<ui64> Values;
        TIndexHelper<ui64> IndexHelper;
    };

    class TBinarizedFloatValuesHolder: public TCompressedValuesHolderImpl
    {
    public:
        TBinarizedFloatValuesHolder(ui32 featureId,
                                    ui64 size,
                                    const TVector<float>& borders,
                                    TVector<ui64>&& data,
                                    TString featureName)
                : TCompressedValuesHolderImpl(EFeatureValuesType::BinarizedFloat,
                                              featureId,
                                              size,
                                              IntLog2(borders.size() + 1),
                                              std::move(data),
                                              std::move(featureName))
                  , Borders(borders)
        {
        }

        ui32 Discretization() const
        {
            return (ui32) Borders.size();
        }

        const TVector<float>& GetBorders() const
        {
            return Borders;
        }

    private:
        TVector<float> Borders;
    };


    class TFloatValuesHolder: public IFeatureValuesHolder
    {
    public:
        TFloatValuesHolder(ui32 featureId,
                           TVector<float>&& values,
                           TString featureName = "")
                : IFeatureValuesHolder(EFeatureValuesType::Float,
                                       featureId,
                                       values.size(),
                                       std::move(featureName))
                  , Values(MakeHolder<TVector<float>>(std::move(values)))
                  , ValuesPtr(Values->data())
        {
        }

        TFloatValuesHolder(ui32 featureId,
                           float* valuesPtr,
                           ui64 valuesCount,
                           TString featureName = "")
                : IFeatureValuesHolder(EFeatureValuesType::Float,
                                       featureId, valuesCount, std::move(featureName))
                  , ValuesPtr(valuesPtr)
        {
        }

        float GetValue(ui32 line) const
        {
            return ValuesPtr[line];
        }

        const float* GetValuesPtr() const
        {
            return ValuesPtr;
        }

        const TVector<float>& GetValues() const
        {
            CB_ENSURE(Values, "Error: this values holder contains only reference for external features");
            return *Values;
        }

    private:
        THolder<TVector<float>> Values;
        float* ValuesPtr;
    };


    class ICatFeatureValuesHolder: public IFeatureValuesHolder
    {
    public:

        ICatFeatureValuesHolder(const ui32 featureId,
                                ui64 size,
                                const TString& featureName)
                : IFeatureValuesHolder(EFeatureValuesType::Categorical,
                                       featureId,
                                       size,
                                       featureName)
        {
        }

        virtual ui32 GetUniqueValues() const = 0;

        virtual ui32 GetValue(ui32 line) const = 0;

        virtual TVector<ui32> ExtractValues() const = 0;
    };


    class TCatFeatureValuesHolder: public ICatFeatureValuesHolder
    {
    public:
        TCatFeatureValuesHolder(ui32 featureId,
                                ui64 size,
                                TVector<ui64>&& compressedValues,
                                ui32 uniqueValues,
                                TString featureName = "")
                : ICatFeatureValuesHolder(featureId, size, std::move(featureName))
                  , UniqueValues(uniqueValues)
                  , IndexHelper(IntLog2(uniqueValues))
                  , Values(std::move(compressedValues))
        {
        }

        ui32 GetUniqueValues() const override
        {
            return UniqueValues;
        }

        ui32 GetValue(ui32 line) const override
        {
            return IndexHelper.Extract(Values, line);
        }

        TVector<ui32> ExtractValues() const override
        {
            return DecompressVector<ui64, ui32>(Values,
                                                GetSize(),
                                                IndexHelper.GetBitsPerKey());
        }

    private:
        ui32 UniqueValues;
        TIndexHelper<ui64> IndexHelper;
        TVector<ui64> Values;
    };


    inline TFeatureColumnPtr FloatToBinarizedColumn(const TFloatValuesHolder& floatValuesHolder,
                                                    const TVector<float>& borders)
    {
        if (!borders.empty())
        {
            const ui32 bitsPerKey = IntLog2(borders.size() + 1);
            const auto& floatValues = floatValuesHolder.GetValues();
            auto binarizedFeature = BinarizeLine(floatValues.data(), floatValues.size(), borders);
            auto compressed = CompressVector<ui64>(binarizedFeature, bitsPerKey);
            return MakeHolder<TBinarizedFloatValuesHolder>(floatValuesHolder.GetId(),
                                                           floatValues.size(),
                                                           borders,
                                                           std::move(compressed),
                                                           floatValuesHolder.GetName());
        } else
        {
            return MakeHolder<TZeroFeature>(floatValuesHolder.GetId());
        }
    }
}
