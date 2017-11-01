#pragma once

#include <util/generic/vector.h>
#include <util/generic/set.h>
#include <util/stream/file.h>

struct TUnitTestPool {
    yvector<float> Features;
    yvector<float> Targets;
    yvector<ui32> Qids;
    yvector<ui32> Gids;
    yvector<yvector<ui32>> Queries;

    ui64 NumSamples;
    ui64 SamplesPerQuery;
    ui64 NumFeatures;

    yvector<float> GetFeature(ui32 f) {
        yvector<float> feature;
        for (size_t doc = 0; doc < NumSamples; ++doc) {
            feature.push_back(Features[f * NumSamples + doc]);
        }
        return feature;
    }
};
////

struct TBinarizedPool {
    yvector<yvector<ui8>> Features;
    yvector<float> Targets;
    yvector<ui32> Qids;
    yvector<yvector<ui32>> CatFeatures;
    yvector<yvector<int>> Queries;
    ui32 NumSamples;
    ui32 SamplesPerQuery;
    ui32 NumFeatures;
    ui32 NumCatFeatures = 1;
    yvector<ui32> CompressedIndex;
    //
    //

    void CreateCompressedIndex(yset<int> skipFeatures) {
        CompressedIndex.clear();
        ui32 i = 0;
        for (ui32 f = 0; f < NumFeatures; ++f) {
            if (skipFeatures.count(f))
                continue;
            if (CompressedIndex.size() < (size_t)NumSamples * (i / 4 + 1)) {
                size_t start = CompressedIndex.size();
                CompressedIndex.resize(NumSamples * (i / 4 + 1));
                for (size_t j = start; j < CompressedIndex.size(); ++j) {
                    CompressedIndex[j] = 0;
                }
            }
            Add(Features[f], i);
            ++i;
        }
    }

    void Add(yvector<ui8> bins, ui32 i) {
        ui32 mask = GetMask(i);
        ui32 offset = NumSamples * (i / 4);
        for (ui32 doc = 0; doc < NumSamples; ++doc) {
            ui32& x = CompressedIndex[offset + doc];
            Binarize(mask, bins[doc], x);
        }
    }

    inline ui32 GetMask(int i) {
        ui32 shift = (ui32)(24 - ((8 * i) % 32));
        ui32 mask = (ui32)(0xFF << shift);
        return mask;
    }

    inline void Binarize(const ui32 mask, const ui8 bin, ui32& x) {
        x &= ~mask;
        ui32 shift = CountTrailingZeroBits(mask);
        x |= bin << shift;
    }
};

void GenerateTestPool(TBinarizedPool& pool, const ui32 binarization, ui32 catFeatures = 1, ui32 seed = 0);
void SavePoolToFile(TBinarizedPool& pool, const char* filename);
void SavePoolCDToFile(const char* filename, ui32 catFeatures = 1);

void GenerateTestPool(TUnitTestPool& pool, ui32 numFeatures = 319);
void SavePoolToFile(TUnitTestPool& pool, const char* filename);
