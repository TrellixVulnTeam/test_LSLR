#pragma once

#include "pool.h"
#include "pair.h"

#include <catboost/libs/cat_feature/cat_feature.h>
#include <catboost/libs/column_description/column.h>

#include <library/threading/local_executor/local_executor.h>

#include <util/system/event.h>
#include <util/stream/file.h>
#include <util/string/vector.h>

#include <string>

struct TPoolColumnsMetaInfo {
    ui32 FactorCount;
    TVector<int> CatFeatureIds;

    ui32 BaselineCount = 0;

    bool HasQueryIds = false;
    bool HasDocIds = false;
    bool HasWeights = false;
};

class IPoolBuilder {
public:
    virtual void Start(const TPoolColumnsMetaInfo& poolMetaInfo, int docCount) = 0;
    virtual void StartNextBlock(ui32 blockSize) = 0;

    virtual float GetCatFeatureValue(const TStringBuf& feature) = 0;
    virtual void AddCatFeature(ui32 localIdx, ui32 featureId, const TStringBuf& feature) = 0;
    virtual void AddFloatFeature(ui32 localIdx, ui32 featureId, float feature) = 0;
    virtual void AddAllFloatFeatures(ui32 localIdx, const TVector<float>& features) = 0;
    virtual void AddTarget(ui32 localIdx, float value) = 0;
    virtual void AddWeight(ui32 localIdx, float value) = 0;
    virtual void AddQueryId(ui32 localIdx, ui32 value) = 0;
    virtual void AddBaseline(ui32 localIdx, ui32 offset, double value) = 0;
    virtual void AddDocId(ui32 localIdx, const TStringBuf& value) = 0;
    virtual void SetFeatureIds(const TVector<TString>& featureIds) = 0;
    virtual void SetPairs(const TVector<TPair>& pairs) = 0;
    virtual int GetDocCount() const = 0;
    virtual void GenerateDocIds(int offset) = 0;
    virtual void Finish() = 0;
    virtual ~IPoolBuilder() = default;
};

class TTargetConverter {
    public:
    static constexpr float UNDEFINED_CLASS = -1;

    explicit TTargetConverter(const TVector<TString>& classNames);

    float operator()(const TString& word) const;

    private:
    TVector<TString> ClassNames;
};

void StartBuilder(const TVector<TString>& FeatureIds, const TPoolColumnsMetaInfo& PoolMetaInfo, int docCount,
                        bool hasHeader, int offset, IPoolBuilder* poolBuilder);
void FinalizeBuilder(const TVector<TColumn>& ColumnsDescription, const TString& pairsFile, IPoolBuilder* poolBuilder);

class TPoolReader {
public:
    TPoolReader(const TString& cdFile, const TString& poolFile, const TString& pairsFile, char fieldDelimiter,
                bool hasHeader, const TVector<TString>& classNames, int blockSize, IPoolBuilder* poolBuilder, NPar::TLocalExecutor* localExecutor);

    bool ReadBlock();
    int GetBlockSize() const {
        return ParseBuffer.ysize();
    }
    void ProcessBlock();
    TString PairsFile;
    TVector<TString> FeatureIds;
    TPoolColumnsMetaInfo PoolMetaInfo;
    TVector<TColumn> ColumnsDescription;

private:
    size_t LinesRead;
    char FieldDelimiter;
    bool HasHeader;
    TTargetConverter ConvertTarget;
    const size_t BlockSize;
    TIFStream Reader;
    TVector<TString> ParseBuffer;
    TVector<TString> ReadBuffer;
    IPoolBuilder& PoolBuilder;
    NPar::TLocalExecutor* LocalExecutor;
    TAutoEvent BlockReadCompletedEvent;

    void ReadBlockAsync();
    void StartBuild();
    void FinishBuild();
};

THolder<IPoolBuilder> InitBuilder(TPool* pool, NPar::TLocalExecutor* localExecutor);

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              int threadCount,
              bool verbose,
              char fieldDelimiter,
              bool hasHeader,
              const TVector<TString>& classNames,
              TPool* pool);

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              bool verbose,
              char fieldDelimiter,
              bool hasHeader,
              const TVector<TString>& classNames,
              NPar::TLocalExecutor* localExecutor,
              IPoolBuilder* poolBuilder);

void ReadPool(const TString& cdFile,
              const TString& poolFile,
              const TString& pairsFile,
              int threadCount,
              bool verbose,
              IPoolBuilder& poolBuilder);
