#pragma once

#include <catboost/cuda/data/feature.h>
#include <catboost/cuda/data/binarizations_manager.h>
#include <catboost/cuda/gpu_data/fold_based_dataset.h>

#include <util/generic/map.h>
#include <util/generic/hash.h>
#include <util/generic/set.h>

namespace NCatboostCuda
{
/*
 * TreeCtrs dataSet are cached based on baseTensor, from which they we generated
 * If we don't have enough gpu-ram, then cache in batchs (for one baseTensor generate several dataSets with ctrs)
 * TreeCtrs dataSets always for catFeature i stores all ctrs with this catFeature - perFeatures batch instead of perCtr
 *
 */
    class TTreeCtrDataSet: public TGuidHolder
    {
    public:
        using TGpuDataSet = TGpuBinarizedDataSet<THalfByteFeatureGridPolicy, TSingleDevPoolLayout>;
        using TFeaturesMapping = typename TSingleDevPoolLayout::TFeaturesMapping;
        using TSampleMapping = typename TSingleDevPoolLayout::TSampleMapping;
        using TVec = TCudaBuffer<float, TFeaturesMapping>;
        using TCompressedIndexMapping = typename TGpuDataSet::TCompressedIndexMapping;

    public:
        template<class TUi32>
        TTreeCtrDataSet(const TBinarizedFeaturesManager& featuresManager,
                        const TFeatureTensor& baseTensor,
                        const TCudaBuffer<TUi32, TSampleMapping>& baseTensorIndices)
                : FeaturesManager(featuresManager)
                  , BaseFeatureTensor(baseTensor)
                  , BaseTensorIndices(baseTensorIndices.ConstCopyView())
                  , CacheHolder(new TScopedCacheHolder)
        {
        }

        const yvector<TCtr>& GetCtrs() const
        {
            return Ctrs;
        }

        const TCtr& GetCtr(ui32 featureId) const
        {
            return Ctrs[featureId];
        }

        TScopedCacheHolder& GetCacheHolder() const
        {
            CB_ENSURE(CacheHolder);
            return *CacheHolder;
        }

        bool HasDataSet() const
        {
            return BinarizedDataSet != nullptr;
        }

        const TGpuDataSet& GetDataSet() const
        {
            CB_ENSURE(BinarizedDataSet != nullptr);
            return *BinarizedDataSet;
        }

        ui32 GetDeviceId() const
        {
            return BaseTensorIndices.GetMapping().GetDeviceId();
        }

        bool HasCompressedIndex() const
        {
            return BinarizedDataSet != nullptr && (BinarizedDataSet->GetCompressedIndex().GetObjectsSlice().Size() > 0);
        }

        ui32 GetCompressedIndexPermutationKey() const
        {
            return PermutationKey;
        }

        ymap<TCtr, yvector<float>> ReadBorders(const yvector<ui32>& ids) const
        {
            yvector<float> allBorders;
            CtrBorders.Read(allBorders);
            ymap<TCtr, yvector<float>> result;

            for (auto id : ids)
            {
                TSlice readSlice = CtrBorderSlices[id];
                result[Ctrs[id]] = ExtractBorders(allBorders.data() + readSlice.Left);
            }
            return result;
        };

        yvector<float> ReadBorders(const ui32 featureId) const
        {
            yvector<float> borders;
            TSlice readSlice = CtrBorderSlices[featureId];
            CtrBorders.CreateReader().SetReadSlice(readSlice).Read(borders);
            return ExtractBorders(borders.data());
        }

        const TFeatureTensor& GetBaseTensor() const
        {
            return BaseFeatureTensor;
        }

        const TCudaBuffer<const ui32, TSampleMapping>& GetBaseTensorIndices() const
        {
            return BaseTensorIndices;
        }

        void SetPermutationKey(ui32 permutationKey)
        {
            PermutationKey = permutationKey;
        }

        bool HasCatFeature(ui32 featureId) const
        {
            return CatFeatures.has(featureId);
        }

        const yset<ui32>& GetCatFeatures() const
        {
            return CatFeatures;
        }

        const yhash<TFeatureTensor, yvector<TCtrConfig>>& GetCtrConfigs() const
        {
            return CtrConfigs;
        }

    private:
        yvector<float> ExtractBorders(const float* bordersAndSize) const
        {
            const ui32 borderCount = static_cast<ui32>(bordersAndSize[0]);
            yvector<float> borders(borderCount);
            for (ui32 i = 0; i < borderCount; ++i)
            {
                borders[i] = bordersAndSize[i + 1];
            }
            return borders;
        }

        const yvector<TCtrConfig>& GetCtrsConfigsForTensor(const TFeatureTensor& featureTensor)
        {
            if (CtrConfigs.count(featureTensor) == 0)
            {
                CtrConfigs[featureTensor] = FeaturesManager.CreateCtrsConfigsForTensor(featureTensor);
            }
            return CtrConfigs[featureTensor];
        }

        void AddCatFeature(const ui32 catFeature)
        {
            {
                TFeatureTensor tensor = BaseFeatureTensor;
                tensor.AddCatFeature(catFeature);
                CB_ENSURE(tensor != BaseFeatureTensor, "Error: expect new tensor");
            }
            CatFeatures.insert(catFeature);
        }

        void BuildFeatureIndex()
        {
            CB_ENSURE(InverseCtrIndex.size() == 0, "Error: build could be done only once");

            for (const ui32 feature : CatFeatures)
            {
                TFeatureTensor tensor = BaseFeatureTensor;
                tensor.AddCatFeature(feature);
                const auto& configs = GetCtrsConfigsForTensor(tensor);
                for (auto& config : configs)
                {
                    TCtr ctr;
                    ctr.FeatureTensor = tensor;
                    ctr.Configuration = config;
                    const ui32 idx = static_cast<const ui32>(InverseCtrIndex.size());
                    InverseCtrIndex[ctr] = idx;
                    Ctrs.push_back(ctr);
                    CB_ENSURE(FeaturesManager.GetCtrBinarization(ctr).Discretization <= 15,
                              "Error: maximum tree-ctrs border count is compile-time constant and currently set to 15 for optimal performance");
                    const ui32 bordersSize = 1 + FeaturesManager.GetCtrBinarization(ctr).Discretization;
                    const ui32 offset = static_cast<const ui32>(CtrBorderSlices.size() ? CtrBorderSlices.back().Right
                                                                                       : 0);
                    const TSlice bordersSlice = TSlice(offset, offset + bordersSize);
                    CtrBorderSlices.push_back(bordersSlice);
                }
            }

            TFeaturesMapping featuresMapping = CreateFeaturesMapping();

            auto bordersMapping = featuresMapping.Transform([&](TSlice deviceSlice)
                                                            {
                                                                ui32 size = 0;
                                                                for (ui32 feature = static_cast<ui32>(deviceSlice.Left);
                                                                     feature < deviceSlice.Right; ++feature)
                                                                {
                                                                    size += CtrBorderSlices[feature].Size();
                                                                }
                                                                return size;
                                                            });
            CtrBorders.Reset(bordersMapping);

            if (CtrBorderSlices.size())
            {
                //borders are so small, that it should be almost always faster to write all border vec then by parts
                yvector<float> borders(CtrBorderSlices.back().Right);
                bool needWrite = false;

                for (ui32 i = 0; i < Ctrs.size(); ++i)
                {
                    const auto& ctr = Ctrs[i];
                    AreCtrBordersComputed.push_back(false);
                    if (FeaturesManager.IsKnown(ctr))
                    {
                        const auto& ctrBorders = FeaturesManager.GetBorders(FeaturesManager.GetId(ctr));
                        const ui64 offset = CtrBorderSlices[i].Left;
                        borders[offset] = ctrBorders.size();
                        std::copy(ctrBorders.begin(), ctrBorders.end(), borders.begin() + offset + 1);
                        CB_ENSURE(ctrBorders.size() < CtrBorderSlices[i].Size());
                        AreCtrBordersComputed.back() = true;
                        needWrite = true;
                    }
                }
                if (needWrite)
                {
                    CtrBorders.Write(borders);
                }
            }
        }

        TFeaturesMapping CreateFeaturesMapping()
        {
            return NCudaLib::TSingleMapping(BaseTensorIndices.GetMapping().GetDeviceId(),
                                            static_cast<ui32>(Ctrs.size()));
        }

        ui32 GetDevice(const TFeaturesMapping& featuresMapping, ui32 featureId)
        {
            for (auto& dev : featuresMapping.NonEmptyDevices())
            {
                if (featuresMapping.DeviceSlice(dev).Contains(TSlice(featureId)))
                {
                    return dev;
                }
            }
            CB_ENSURE(false, "Error: featuresId is out of range");
            return 0;
        }

    private:
        const TBinarizedFeaturesManager& FeaturesManager;

        TFeatureTensor BaseFeatureTensor;
        TCudaBuffer<const ui32, TSampleMapping> BaseTensorIndices;
        yset<ui32> CatFeatures;

        yhash<TCtr, ui32> InverseCtrIndex;
        yvector<TCtr> Ctrs;
        yvector<TSlice> CtrBorderSlices;
        TCudaBuffer<float, TFeaturesMapping> CtrBorders;
        yvector<bool> AreCtrBordersComputed;
        yhash<TFeatureTensor, yvector<TCtrConfig>> CtrConfigs; //ctr configs for baseTensor + catFeature

        THolder<TGpuDataSet> BinarizedDataSet;
        THolder<TScopedCacheHolder> CacheHolder;
        TCompressedIndexMapping CompresssedIndexMapping;

        ui32 PermutationKey = 0;

        friend class TTreeCtrDataSetBuilder;

        template<NCudaLib::EPtrType>
        friend
        class TTreeCtrDataSetsHelper;
    };
}

