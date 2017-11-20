#include "protobuf_data_provider_reader.h"

using namespace NCatboostCuda;


void TCatBoostProtoPoolReader::AddFeatureColumn(TIFStream& input,
                                                TVector<TFeatureColumnPtr>& features,
                                                ui32 docCount) {
    ReadMessage(input, FeatureColumn);
    const auto& featureDescription = FeatureColumn.GetFeatureDescription();
    auto description = featureDescription;
    const ui32 featureId = featureDescription.GetFeatureId();
    const TString featureName = featureDescription.HasFeatureName()
                                    ? featureDescription.GetFeatureName()
                                    : ToString(featureId);
    switch (description.GetFeatureType()) {
        case ::NCompressedPool::TFeatureType::Float: {
            auto values = MakeHolder<TFloatValuesHolder>(featureId, FromProtoToVector(FeatureColumn.GetFloatColumn().GetValues()));
            FeaturesManager.RegisterDataProviderFloatFeature(featureId);
            auto borders = FeaturesManager.GetOrCreateFloatFeatureBorders(*values, TBordersBuilder(*GridBuilderFactory, values->GetValues()));
            features.push_back(FloatToBinarizedColumn(*values, borders));
            break;
        }
        case ::NCompressedPool::TFeatureType::Binarized: {
            TVector<float> borders = FromProtoToVector(FeatureColumn.GetBinarization().GetBorders());
            const auto& binarizedData = FeatureColumn.GetBinarizedColumn().GetData();
            auto values = FromProtoToVector(binarizedData);
            CB_ENSURE(borders.size(), "Error: binarization should be positive");
            auto feature = MakeHolder<TBinarizedFloatValuesHolder>(featureId, docCount, borders, std::move(values), featureName);
            FeaturesManager.RegisterDataProviderFloatFeature(feature->GetId());
            if (!FeaturesManager.HasFloatFeatureBordersForDataProviderFeature(feature->GetId()))
            {
                FeaturesManager.SetFloatFeatureBordersForDataProviderId(feature->GetId(), std::move(borders));
            }
            features.push_back(feature.Release());

            break;
        }
        case ::NCompressedPool::TFeatureType::Categorical: {
            const auto& binarizedData = FeatureColumn.GetBinarizedColumn().GetData();
            auto values = MakeHolder<TCatFeatureValuesHolder>(featureId, docCount, FromProtoToVector(binarizedData), FeatureColumn.GetUniqueValues(), featureName);
            FeaturesManager.RegisterDataProviderFloatFeature(featureId);
            features.push_back(values.Release());
            break;
        }
        default: {
            ythrow yexception() << "Error: unknown column type";
        }
    }
}
