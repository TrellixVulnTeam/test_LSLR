# coding: utf-8
# cython: wraparound=False

from six import iteritems, string_types, PY3
from json import dumps, loads
from copy import deepcopy
from collections import Sequence, defaultdict

from cython.operator cimport dereference

from libcpp cimport bool as bool_t
from libcpp.map cimport map as cmap
from libcpp.vector cimport vector
from libcpp.pair cimport pair

from util.generic.string cimport TString
from util.generic.string cimport TStringBuf
from util.generic.vector cimport yvector
from util.generic.maybe cimport TMaybe
from util.generic.hash cimport yhash


class CatboostError(Exception):
    pass

cdef public object PyCatboostExceptionType = <object>CatboostError

cdef extern from "catboost/libs/logging/logging.h":
    cdef void SetCustomLoggingFunction(void(*func)(const char*, size_t len) except * with gil)
    cdef void RestoreOriginalLogger()

cdef extern from "catboost/python-package/catboost/helpers.h":
    cdef void ProcessException()
    cdef void SetPythonInterruptHandler() nogil
    cdef void ResetPythonInterruptHandler() nogil

cdef extern from "catboost/libs/data/pair.h":
    cdef cppclass TPair:
        int WinnerId
        int LoserId

cdef extern from "catboost/libs/data/pool.h":
    cdef cppclass TDocInfo:
        float Target
        float Weight
        yvector[float] Factors
        yvector[double] Baseline
        void Swap(TDocInfo& other) except +ProcessException

    cdef cppclass TDocumentStorage:
        yvector[yvector[float]] Factors
        yvector[yvector[double]] Baseline
        yvector[float] Target
        yvector[float] Weight
        yvector[TString] Id
        void Swap(TDocumentStorage& other) except +ProcessException
        int GetBaselineDimension() except +ProcessException const
        int GetFactorsCount() except +ProcessException const
        size_t GetDocCount() except +ProcessException const
        float GetFeatureValue(int docIdx, int featureIdx) except +ProcessException const
        void Swap(TDocumentStorage& other) except +ProcessException
        void SwapDoc(size_t doc1Idx, size_t doc2Idx) except +ProcessException
        void AssignDoc(int destinationIdx, const TDocumentStorage& sourceDocs, int sourceIdx) except +ProcessException
        void Resize(int docCount, int featureCount, int approxDim) except +ProcessException
        void Clear() except +ProcessException
        void Append(const TDocumentStorage& documents) except +ProcessException

    cdef cppclass TPool:
        TDocumentStorage Docs
        yvector[int] CatFeatures
        yvector[TString] FeatureId
        yhash[int, TString] CatFeaturesHashToString
        yvector[TPair] Pairs

cdef extern from "catboost/libs/data/load_data.h":
    cdef void ReadPool(const TString& cdFile,
                       const TString& poolFile,
                       const TString& pairsFile,
                       int threadCount,
                       bool_t verbose,
                       const char fieldDelimiter,
                       bool_t has_header,
                       const yvector[TString]& classNames,
                       TPool* pool) nogil except +ProcessException
    cdef int CalcCatFeatureHash(const TStringBuf& feature) except +ProcessException
    cdef float ConvertCatFeatureHashToFloat(int hashVal) except +ProcessException

cdef extern from "catboost/libs/model/tensor_struct.h":
    cdef cppclass TTensorStructure3:
        pass

cdef extern from "catboost/libs/model/model.h":

    cdef cppclass TFullModel:
        yvector[int] CatFeatures
        yvector[TTensorStructure3] TreeStruct
        yhash[TString, TString] ModelInfo
        void Swap(TFullModel& other) except +ProcessException

    cdef cppclass EModelExportType:
        pass

    cdef EModelExportType EModelExportType_Catboost "EModelExportType::CatboostBinary"
    cdef EModelExportType EModelExportType_CoreML "EModelExportType::AppleCoreML"

    cdef void ExportModel(const TFullModel& model, const TString& modelFile, const EModelExportType format, const TString& userParametersJSON ) except +ProcessException

    cdef void OutputModel(const TFullModel& model, const TString& modelFile) except +ProcessException
    cdef TFullModel ReadModel(const TString& modelFile) nogil except +ProcessException
    cdef TString SerializeModel(const TFullModel& model) except +ProcessException
    cdef TFullModel DeserializeModel(const TString& serializeModelString) nogil except +ProcessException

cdef extern from "catboost/libs/model/formula_evaluator.h":
    cdef cppclass TFormulaEvaluator "NCatBoost::TFormulaEvaluator":
        TFormulaEvaluator(const TFullModel& model) nogil except +ProcessException

cdef extern from "library/json/writer/json_value.h" namespace "NJson":
    cdef cppclass TJsonValue:
        pass

cdef extern from "library/containers/2d_array/2d_array.h":
    cdef cppclass TArray2D[T]:
        T* operator[] (size_t index) const

cdef extern from "catboost/libs/algo/error_holder.h":
    cdef cppclass TErrorHolder:
        double Error
        double Weight

cdef extern from "catboost/libs/algo/ders_holder.h":
    cdef cppclass TDer1Der2:
        double Der1
        double Der2

cdef extern from "catboost/libs/algo/params.h":
    cdef TJsonValue ReadTJsonValue(const TString& paramsJson) nogil except +ProcessException
    cdef bool_t IsClassificationLoss(const TString& lossFunction) nogil except +ProcessException

    cdef cppclass EPredictionType:
        pass

    cdef EPredictionType EPredictionType_Class "EPredictionType::Class"
    cdef EPredictionType EPredictionType_Probability "EPredictionType::Probability"
    cdef EPredictionType EPredictionType_RawFormulaVal "EPredictionType::RawFormulaVal"

    cdef cppclass TCustomMetricDescriptor:
        void* CustomData
        TErrorHolder (*EvalFunc)(const yvector[yvector[double]]& approx,
                                 const yvector[float]& target,
                                 const yvector[float]& weight,
                                 int begin, int end, void* customData) except * with gil
        TString (*GetDescriptionFunc)(void *customData) except * with gil
        bool_t (*IsMaxOptimalFunc)(void *customData) except * with gil
        double (*GetFinalErrorFunc)(const TErrorHolder& error, void *customData) except * with gil

    cdef cppclass TCustomObjectiveDescriptor:
        void* CustomData
        void (*CalcDersRange)(int count, const double* approxes, const float* targets,
                              const float* weights, TDer1Der2* ders, void* customData) except * with gil
        void (*CalcDersMulti)(const yvector[double]& approx, float target, float weight,
                              yvector[double]* ders, TArray2D[double]* der2, void* customData) except * with gil

    cdef cppclass TCrossValidationParams:
        size_t FoldCount
        bool_t Inverted
        int PartitionRandSeed
        bool_t Shuffle
        int EvalPeriod

    cdef void CheckFitParams(const TJsonValue& tree,
                     const TMaybe[TCustomObjectiveDescriptor]& objectiveDescriptor,
                     const TMaybe[TCustomMetricDescriptor]& evalMetricDescriptor) nogil except +ProcessException

cdef extern from "catboost/libs/algo/train_model.h":
    cdef void TrainModel(const TJsonValue& params,
                         const TMaybe[TCustomObjectiveDescriptor]& objectiveDescriptor,
                         const TMaybe[TCustomMetricDescriptor]& evalMetricDescriptor,
                         TPool& learnPool,
                         bool_t allowClearPool,
                         const TPool& testPool,
                         const TString& outputModelPath,
                         TFullModel* model,
                         yvector[yvector[double]]* testApprox) nogil except +ProcessException

cdef extern from "catboost/libs/algo/cross_validation.h":
    cdef cppclass TCVResult:
        TString Metric
        yvector[double] AverageTrain
        yvector[double] StdDevTrain
        yvector[double] AverageTest
        yvector[double] StdDevTest

    cdef void CrossValidate(const TJsonValue& jsonParams,
                            const TMaybe[TCustomObjectiveDescriptor]& objectiveDescriptor,
                            const TMaybe[TCustomMetricDescriptor]& evalMetricDescriptor,
                            TPool& pool,
                            const TCrossValidationParams& cvParams,
                            yvector[TCVResult]* results) nogil except +ProcessException

cdef extern from "catboost/libs/algo/apply.h":
    cdef yvector[double] ApplyModel(const TFullModel& model,
                                    const TPool& pool,
                                    bool_t verbose,
                                    const EPredictionType predictionType,
                                    int begin,
                                    int end,
                                    int threadCount) nogil except +ProcessException

    cdef yvector[yvector[double]] ApplyModelMulti(const TFormulaEvaluator& calcer,
                                                      const TPool& pool,
                                                      bool_t verbose,
                                                      const EPredictionType predictionType,
                                                      int begin,
                                                      int end,
                                                      int threadCount) nogil except +ProcessException


cdef extern from "catboost/libs/algo/eval_helpers.h":
    cdef yvector[yvector[double]] PrepareEval(const EPredictionType predictionType,
                                              const yvector[yvector[double]]& approx,
                                              int threadCount) nogil except +ProcessException

cdef extern from "catboost/libs/algo/calc_fstr.h":
    cdef yvector[yvector[double]] GetFeatureImportances(const TFullModel& model,
                                                        const TPool& pool,
                                                        const TString& type,
                                                        int threadCount) nogil except +ProcessException

cdef TString _MetricGetDescription(void* customData) except * with gil:
    cdef metricObject = <object>customData
    name = metricObject.__class__.__name__
    if PY3:
        name = name.encode()
    return TString(<const char*>name)

cdef bool_t _MetricIsMaxOptimal(void* customData) except * with gil:
    cdef metricObject = <object>customData
    return metricObject.is_max_optimal()

cdef double _MetricGetFinalError(const TErrorHolder& error, void *customData) except * with gil:
    cdef metricObject = <object>customData
    return metricObject.get_final_error(error.Error, error.Weight)

cdef class _FloatArrayWrapper:
    cdef const float* _arr
    cdef int _count

    @staticmethod
    cdef create(const float* arr, int count):
        wrapper = _FloatArrayWrapper()
        wrapper._arr = arr
        wrapper._count = count
        return wrapper

    def __getitem__(self, key):
        if key >= self._count:
            raise IndexError()

        return self._arr[key]

    def __len__(self):
        return self._count

# Cython does not have generics so using small copy-paste here and below
cdef class _DoubleArrayWrapper:
    cdef const double* _arr
    cdef int _count

    @staticmethod
    cdef create(const double* arr, int count):
        wrapper = _DoubleArrayWrapper()
        wrapper._arr = arr
        wrapper._count = count
        return wrapper

    def __getitem__(self, key):
        if key >= self._count:
            raise IndexError()

        return self._arr[key]

    def __len__(self):
        return self._count

cdef TErrorHolder _MetricEval(const yvector[yvector[double]]& approx,
                              const yvector[float]& target,
                              const yvector[float]& weight,
                              int begin,
                              int end,
                              void* customData) except * with gil:
    cdef metricObject = <object>customData
    cdef TErrorHolder holder

    approxes = [_DoubleArrayWrapper.create(approx[i].data() + begin, end - begin) for i in xrange(approx.size())]
    targets = _FloatArrayWrapper.create(target.data() + begin, end - begin)

    if weight.size() == 0:
        weights = None
    else:
        weights = _FloatArrayWrapper.create(weight.data() + begin, end - begin)

    error, weight_ = metricObject.evaluate(approxes, targets, weights)

    holder.Error = error
    holder.Weight = weight_
    return holder

cdef void _ObjectiveCalcDersRange(int count, const double* approxes, const float* targets,
                                  const float* weights, TDer1Der2* ders, void* customData) except * with gil:
    cdef objectiveObject = <object>(customData)

    approx = _DoubleArrayWrapper.create(approxes, count)
    target = _FloatArrayWrapper.create(targets, count)

    if weights:
        weight = _FloatArrayWrapper.create(weights, count)
    else:
        weight = None

    result = objectiveObject.calc_ders_range(approx, target, weight)
    index = 0
    for der1, der2 in result:
        ders[index].Der1 = der1
        ders[index].Der2 = der2
        index += 1

cdef void _ObjectiveCalcDersMulti(const yvector[double]& approx, float target, float weight,
                                  yvector[double]* ders, TArray2D[double]* der2, void* customData) except * with gil:
    cdef objectiveObject = <object>(customData)

    approxes = _DoubleArrayWrapper.create(approx.data(), approx.size())

    ders_vector, second_ders_matrix = objectiveObject.calc_ders_multi(approxes, target, weight)
    for index, der in enumerate(ders_vector):
        dereference(ders)[index] = der

    for ind1, line in enumerate(second_ders_matrix):
        for ind2, num in enumerate(line):
            dereference(der2)[ind1][ind2] = num

cdef TCustomMetricDescriptor _BuildCustomMetricDescriptor(object metricObject):
    cdef TCustomMetricDescriptor descriptor
    descriptor.CustomData = <void*>metricObject
    descriptor.EvalFunc = &_MetricEval
    descriptor.GetDescriptionFunc = &_MetricGetDescription
    descriptor.IsMaxOptimalFunc = &_MetricIsMaxOptimal
    descriptor.GetFinalErrorFunc = &_MetricGetFinalError
    return descriptor

cdef TCustomObjectiveDescriptor _BuildCustomObjectiveDescriptor(object objectiveObject):
    cdef TCustomObjectiveDescriptor descriptor
    descriptor.CustomData = <void*>objectiveObject
    descriptor.CalcDersRange = &_ObjectiveCalcDersRange
    descriptor.CalcDersMulti = &_ObjectiveCalcDersMulti
    return descriptor

cdef class PyPredictionType:
    cdef EPredictionType predictionType
    def __init__(self, prediction_type):
        if prediction_type == 'Class':
            self.predictionType = EPredictionType_Class
        elif prediction_type == 'Probability':
            self.predictionType = EPredictionType_Probability
        else:
            self.predictionType = EPredictionType_RawFormulaVal

cdef class PyExportType:
    cdef EModelExportType exportType
    def __init__(self, prediction_type):
        if prediction_type == 'coreml':
            self.exportType = EModelExportType_CoreML
        else:
            self.exportType = EModelExportType_Catboost

cdef class _PreprocessParams:
    cdef TJsonValue tree
    cdef TMaybe[TCustomObjectiveDescriptor] customObjectiveDescriptor
    cdef TMaybe[TCustomMetricDescriptor] customMetricDescriptor
    def __init__(self, dict params):
        eval_metric = params.get("eval_metric")
        objective = params.get("loss_function")

        is_custom_eval_metric = eval_metric is not None and not isinstance(eval_metric, string_types)
        is_custom_objective = objective is not None and not isinstance(objective, string_types)

        params_to_json = params

        if is_custom_objective or is_custom_eval_metric:
            keys_to_replace = set()
            if is_custom_objective:
                keys_to_replace.add("loss_function")
            if is_custom_eval_metric:
                keys_to_replace.add("eval_metric")

            params_to_json = {}

            for k, v in params.iteritems():
                if k in keys_to_replace:
                    continue
                params_to_json[k] = deepcopy(v)

            for k in keys_to_replace:
                params_to_json[k] = "Custom"

        dumps_params = dumps(params_to_json)

        if params_to_json.get("loss_function") == "Custom":
            self.customObjectiveDescriptor = _BuildCustomObjectiveDescriptor(params["loss_function"])
        if params_to_json.get("eval_metric") == "Custom":
            self.customMetricDescriptor = _BuildCustomMetricDescriptor(params["eval_metric"])

        dumps_params = to_binary_str(dumps_params)
        self.tree = ReadTJsonValue(TString(<const char*>dumps_params))

cdef to_binary_str(string):
    if PY3:
        return string.encode()
    return string

cdef to_native_str(binary):
    if PY3:
        return binary.decode()
    return binary

cdef class _PoolBase:
    cdef TPool* __pool
    cdef bool_t has_label_

    def __cinit__(self):
        self.__pool = new TPool()
        self.has_label_ = False

    def __dealloc__(self):
        del self.__pool

    def __deepcopy__(self, _):
        raise CatboostError('Can\'t deepcopy _PoolBase object')

    cpdef _init_cat_features(self, cat_features):
        self.__pool.CatFeatures.clear()
        for feature in cat_features:
            self.__pool.CatFeatures.push_back(int(feature))

    cpdef _read_pool(self, pool_file, cd_file, delimiter, bool_t has_header, int thread_count):
        pool_file = to_binary_str(pool_file)
        cd_file = to_binary_str(cd_file)
        cdef yvector[TString] emptyVec
        ReadPool(TString(<char*>cd_file),
                 TString(<char*>pool_file),
                 "",
                 thread_count,
                 False,
                 ord(delimiter),
                 has_header,
                 emptyVec,
                 self.__pool)
        if len(set([target for target in self.__pool.Docs.Target])) > 1:
            self.has_label_ = True

    cpdef _init_pool(self, data, label, weight, baseline, feature_names):
        self._set_data(data)
        num_class = 2
        if label is not None:
            self._set_label(label)
            num_class = len(set(label))
            self.has_label_ = True
        if baseline is not None:
            self._set_baseline(baseline)
        if weight is not None:
            self._set_weight(weight)
        if feature_names is not None:
            self._set_feature_names(feature_names)

    cpdef _set_data(self, data):
        self.__pool.Docs.Clear()
        if len(data) == 0:
            return
        self.__pool.Docs.Resize(len(data), len(data[0]), 0)
        cdef TString factor_str
        cat_features = set(self.get_cat_feature_indices())
        for i in range(len(data)):
            for j, factor in enumerate(data[i]):
                if j in cat_features:
                    if not isinstance(factor, string_types):
                        if int(factor) != factor:
                            raise CatboostError('Invalid type for cat_feature[{},{}]={} : cat_features must be integer or string, real number values should be converted to string.'.format(i, j, factor))
                        factor = str(int(factor))
                    factor = to_binary_str(factor)
                    factor_str = TString(<char*>factor)
                    factor = CalcCatFeatureHash(factor_str)
                    self.__pool.CatFeaturesHashToString[factor] = factor_str
                    factor = ConvertCatFeatureHashToFloat(factor)
                self.__pool.Docs.Factors[j][i] = float(factor)

    cpdef _set_label(self, label):
        rows = self.num_row()
        for i in range(rows):
            self.__pool.Docs.Target[i] = float(label[i])

    cpdef _set_weight(self, weight):
        rows = self.num_row()
        for i in range(rows):
            self.__pool.Docs.Weight[i] = float(weight[i])

    cpdef _set_baseline(self, baseline):
        rows = self.num_row()
        if rows == 0:
            return
        self.__pool.Docs.Resize(rows, self.__pool.Docs.GetFactorsCount(), len(baseline[0]))
        for i in range(rows):
            for j, value in enumerate(baseline[i]):
                self.__pool.Docs.Baseline[j][i] = float(value)

    cpdef _set_feature_names(self, feature_names):
        self.__pool.FeatureId.clear()
        for value in feature_names:
            value = to_binary_str(str(value))
            self.__pool.FeatureId.push_back(value)

    cpdef get_feature_names(self):
        if self.is_empty_:
            return None
        feature_names = []
        cdef bytes pystr
        for value in self.__pool.FeatureId:
            pystr = value.c_str()
            feature_names.append(to_native_str(pystr))
        return feature_names

    cpdef num_row(self):
        """
        Get the number of rows in the Pool.

        Returns
        -------
        number of rows : int
        """
        if not self.is_empty_:
            return self.__pool.Docs.GetDocCount()
        return None

    cpdef num_col(self):
        """
        Get the number of columns in the Pool.

        Returns
        -------
        number of cols : int
        """
        if not self.is_empty_:
            return self.__pool.Docs.GetFactorsCount()
        return None

    @property
    def shape(self):
        """
        Get the shape of the Pool.

        Returns
        -------
        shape : (int, int)
            (rows, cols)
        """
        if not self.is_empty_:
            return tuple([self.num_row(), self.num_col()])
        return None

    cpdef get_features(self):
        """
        Get feature matrix from Pool.

        Returns
        -------
        feature matrix : list(list)
        """
        if not self.is_empty_:
            data = []
            for doc in range(self.__pool.Docs.GetDocCount()):
                factors = []
                for factor in self.__pool.Docs.Factors:
                    factors.append(factor[doc])
                data.append(factors)
            return data
        return None

    cpdef get_label(self):
        """
        Get labels from Pool.

        Returns
        -------
        labels : list
        """
        if self.has_label_:
            return [target for target in self.__pool.Docs.Target]
        return None

    cpdef get_cat_feature_indices(self):
        """
        Get cat_feature indices from Pool.

        Returns
        -------
        cat_feature_indices : list
        """
        return [self.__pool.CatFeatures.at(i) for i in range(self.__pool.CatFeatures.size())]

    cpdef get_weight(self):
        """
        Get weight for each instance.

        Returns
        -------
        weight : list
        """
        if not self.is_empty_:
            return [weight for weight in self.__pool.Docs.Weight]
        return None

    cpdef get_baseline(self):
        """
        Get baseline from Pool.

        Returns
        -------
        baseline : list(list)
        """
        if not self.is_empty_:
            baseline = []
            for doc in range(self.__pool.Docs.GetDocCount()):
                doc_approxes = []
                for approx in self.__pool.Docs.Baseline:
                    doc_approxes.append(approx[doc])
                baseline.append(doc_approxes)
            return baseline
        return None

    @property
    def is_empty_(self):
        """
        Check if Pool is empty.

        Returns
        -------
        is_empty_ : bool
        """
        return self.__pool.Docs.GetDocCount() == 0


cdef class _CatBoost:
    cdef TFullModel* __model
    cdef yvector[yvector[double]]* __test_eval

    def __cinit__(self):
        self.__model = new TFullModel()
        self.__test_eval = new yvector[yvector[double]]()

    def __dealloc__(self):
        del self.__model
        del self.__test_eval

    cpdef _train(self, _PoolBase train_pool, _PoolBase test_pool, dict params):
        prep_params = _PreprocessParams(params)
        with nogil:
            SetPythonInterruptHandler()
            try:
                TrainModel(prep_params.tree,
                       prep_params.customObjectiveDescriptor,
                       prep_params.customMetricDescriptor,
                       dereference(train_pool.__pool),
                       False,
                       dereference(test_pool.__pool),
                       TString(<const char*>""),
                       self.__model,
                       self.__test_eval)
            finally:
                ResetPythonInterruptHandler()

    cpdef _set_test_eval(self, test_eval):
        cdef yvector[double] vector
        for row in test_eval:
            for value in row:
                vector.push_back(float(value))
            self.__test_eval.push_back(vector)
            vector.clear()

    cpdef _get_test_eval(self):
        test_eval = []
        for i in range(self.__test_eval.size()):
            test_eval.append([value for value in dereference(self.__test_eval)[i]])
        if (len(test_eval) == 1):
            return test_eval[0]
        return test_eval

    cpdef _get_cat_feature_indices(self):
        return [feature for feature in self.__model.CatFeatures]

    cpdef _base_predict(self, _PoolBase pool, str prediction_type, int ntree_start, int ntree_end, int thread_count, verbose):
        cdef yvector[double] pred
        cdef EPredictionType predictionType = PyPredictionType(prediction_type).predictionType
        pred = ApplyModel(dereference(self.__model),
                          dereference(pool.__pool),
                          verbose,
                          predictionType,
                          ntree_start,
                          ntree_end,
                          thread_count)
        return [value for value in pred]

    cpdef _base_predict_multi(self, _PoolBase pool, str prediction_type, int ntree_start, int ntree_end, int thread_count, verbose):
        cdef yvector[yvector[double]] pred
        cdef EPredictionType predictionType = PyPredictionType(prediction_type).predictionType
        cdef TFormulaEvaluator* calcer = new TFormulaEvaluator(dereference(self.__model))
        pred = ApplyModelMulti(dereference(calcer),
                               dereference(pool.__pool),
                               verbose,
                               predictionType,
                               ntree_start,
                               ntree_end,
                               thread_count)
        del calcer
        return [[value for value in vec] for vec in pred]

    cpdef _staged_predict_iterator(self, _PoolBase pool, str prediction_type, int ntree_start, int ntree_end, int eval_period, int thread_count, verbose):
        stagedPredictIterator = _StagedPredictIterator(pool, prediction_type, ntree_start, ntree_end, eval_period, thread_count, verbose)
        stagedPredictIterator.set_model(self.__model)
        return stagedPredictIterator

    cpdef _calc_fstr(self, _PoolBase pool, fstr_type, int thread_count):
        fstr_type = to_binary_str(fstr_type)
        cdef yvector[yvector[double]] fstr = GetFeatureImportances(dereference(self.__model),
                                                                    dereference(pool.__pool),
                                                                    TString(<const char*>fstr_type),
                                                                    thread_count)
        return [[value for value in fstr[i]] for i in range(fstr.size())]

    cpdef _load_model(self, model_file):
        cdef TFullModel tmp_model
        model_file = to_binary_str(model_file)
        tmp_model = ReadModel(TString(<const char*>model_file))
        self.__model.Swap(tmp_model)

    cpdef _save_model(self, output_file, format, export_parameters):
        cdef EModelExportType exportType = PyExportType(format).exportType
        export_parameters = to_binary_str(export_parameters)
        output_file = to_binary_str(output_file)
        ExportModel(dereference(self.__model), output_file, exportType, export_parameters)

    cpdef _serialize_model(self):
        cdef TString tstr = SerializeModel(dereference(self.__model))
        cdef const char* c_serialized_model_string = tstr.c_str()
        cpdef bytes py_serialized_model_str = c_serialized_model_string[:tstr.size()]
        return py_serialized_model_str

    cpdef _deserialize_model(self, TString serialized_model_str):
        cdef TFullModel tmp_model
        tmp_model = DeserializeModel(serialized_model_str);
        self.__model.Swap(tmp_model)

    cpdef _get_params(self):
        cdef const char* c_params_json = self.__model.ModelInfo["params"].c_str()
        cdef bytes py_params_json = c_params_json
        params_json = to_native_str(py_params_json)
        params = {}
        if params_json:
            for key, value in loads(params_json).iteritems():
                if key != 'random_seed':
                    params[str(key)] = value
        return params

    def _get_tree_count(self):
        return self.__model.TreeStruct.size()

    def _get_random_seed(self):
        cdef const char* c_params_json = self.__model.ModelInfo["params"].c_str()
        cdef bytes py_params_json = c_params_json
        params_json = to_native_str(py_params_json)
        if params_json:
            return loads(params_json).get('random_seed', 0)
        return 0

class _CatBoostBase(object):
    def __init__(self, params):
        self._init_params = params
        self._object = _CatBoost()
        self._check_train_params(self._get_init_train_params())

    def __getstate__(self):
        params = self._get_init_params()
        test_evals = self.get_test_eval()
        if test_evals:
            if test_evals[0]:
                params['_test_eval'] = test_evals
        if self.is_fitted_:
            params['__model'] = self._serialize_model()
        for attr in ['_classes', '_feature_importance']:
            if getattr(self, attr, None) is not None:
                params[attr] = getattr(self, attr, None)
        return params

    def __setstate__(self, state):
        if '_object' not in dict(self.__dict__.items()):
            self._object = _CatBoost()
        if '_init_params' not in dict(self.__dict__.items()):
            self._init_params = {}
        if '__model' in state:
            self._deserialize_model(state['__model'])
            setattr(self, '_is_fitted', True)
            setattr(self, '_random_seed', self._object._get_random_seed())
            del state['__model']
        if '_test_eval' in state:
            self._set_test_eval(state['_test_eval'])
            del state['_test_eval']
        for attr in ['_classes', '_feature_importance']:
            if attr in state:
                setattr(self, attr, state[attr])
                del state[attr]
        self._init_params.update(state)

    def __copy__(self):
        return self.__deepcopy__(None)

    def __deepcopy__(self, _):
        state = self.__getstate__()
        model = self.__class__()
        model.__setstate__(state)
        return model

    def _check_train_params(self, dict params):
        prep_params = _PreprocessParams(params)
        CheckFitParams(prep_params.tree,
                        prep_params.customObjectiveDescriptor,
                        prep_params.customMetricDescriptor)

    def copy(self):
        return self.__copy__()

    def _train(self, train_pool, test_pool, params):
        self._object._train(train_pool, test_pool, params)
        setattr(self, '_is_fitted', True)
        setattr(self, '_random_seed', self._object._get_random_seed())

    def _set_test_eval(self, test_eval):
        self._object._set_test_eval(test_eval)

    def get_test_eval(self):
        return self._object._get_test_eval()

    def _get_cat_feature_indices(self):
        return self._object._get_cat_feature_indices()

    def _base_predict(self, pool, prediction_type, ntree_start, ntree_end, thread_count, verbose):
        return self._object._base_predict(pool, prediction_type, ntree_start, ntree_end, thread_count, verbose)

    def _base_predict_multi(self, pool, prediction_type, ntree_start, ntree_end, thread_count, verbose):
        return self._object._base_predict_multi(pool, prediction_type, ntree_start, ntree_end, thread_count, verbose)

    def _staged_predict_iterator(self, pool, prediction_type, ntree_start, ntree_end, eval_period, thread_count, verbose):
        return self._object._staged_predict_iterator(pool, prediction_type, ntree_start, ntree_end, eval_period, thread_count, verbose)

    def _calc_fstr(self, pool, fstr_type, thread_count):
        return self._object._calc_fstr(pool, fstr_type, thread_count)

    def _save_model(self, output_file, format, export_parameters):
        if self.is_fitted_:
            params_string = ""
            if export_parameters:
                params_string = dumps(export_parameters)

            self._object._save_model(output_file, format, params_string)

    def _load_model(self, model_file):
        self._object._load_model(model_file)
        setattr(self, '_is_fitted', True)
        setattr(self, '_random_seed', self._object._get_random_seed())
        for key, value in iteritems(self._get_params()):
            self._set_param(key, value)

    def _serialize_model(self):
        return self._object._serialize_model()

    def _deserialize_model(self, dump_model_str):
        self._object._deserialize_model(dump_model_str)

    def _get_init_params(self):
        init_params = self._init_params.copy()
        if "kwargs" in init_params:
            init_params.update(init_params["kwargs"])
            del init_params["kwargs"]
        return init_params

    def _get_init_train_params(self):
        params = self._init_params.copy()
        if "kwargs" in params:
            del params["kwargs"]
        return params

    def _get_params(self):
        params = self._object._get_params()
        init_params = self._get_init_params()
        for key, value in iteritems(init_params):
            if key not in params:
                params[key] = value
        return params

    def _set_param(self, key, value):
        self._init_params[key] = value

    def _is_classification_loss(self, loss_function):
        return IsClassificationLoss(to_binary_str(loss_function))

    @property
    def tree_count_(self):
        return self._object._get_tree_count()

    @property
    def is_fitted_(self):
        return getattr(self, '_is_fitted', False)

    @property
    def random_seed_(self):
        return getattr(self, '_random_seed', 0)

cpdef _cv(dict params, _PoolBase pool, int fold_count, bool_t inverted, int partition_random_seed,
          bool_t shuffle):
    prep_params = _PreprocessParams(params)
    cdef TCrossValidationParams cvParams
    cdef yvector[TCVResult] results

    cvParams.FoldCount = fold_count
    cvParams.PartitionRandSeed = partition_random_seed
    cvParams.Shuffle = shuffle
    cvParams.Inverted = inverted

    with nogil:
        SetPythonInterruptHandler()
        try:
            CrossValidate(
                prep_params.tree,
                prep_params.customObjectiveDescriptor,
                prep_params.customMetricDescriptor,
                dereference(pool.__pool),
                cvParams,
                &results)
        finally:
            ResetPythonInterruptHandler()

    result = defaultdict(list)
    for metric_idx in xrange(results.size()):
        metric_name = to_native_str(results[metric_idx].Metric.c_str())
        for it in xrange(results[metric_idx].AverageTrain.size()):
            result[metric_name + "_train_avg"].append(results[metric_idx].AverageTrain[it])
            result[metric_name + "_train_stddev"].append(results[metric_idx].StdDevTrain[it])
            result[metric_name + "_test_avg"].append(results[metric_idx].AverageTest[it])
            result[metric_name + "_test_stddev"].append(results[metric_idx].StdDevTest[it])

    return result


cdef class _StagedPredictIterator:
    cdef yvector[yvector[double]] __approx
    cdef TFullModel* __model
    cdef _PoolBase pool
    cdef TFormulaEvaluator* __calcer
    cdef str prediction_type
    cdef int ntree_start, ntree_end, eval_period, thread_count
    cdef bool_t verbose

    cdef set_model(self, TFullModel* model):
        self.__model = model
        self.__calcer = new TFormulaEvaluator(dereference(self.__model))

    def __cinit__(self, _PoolBase pool, str prediction_type, int ntree_start, int ntree_end, int eval_period, int thread_count, verbose):
        self.pool = pool
        self.prediction_type = prediction_type
        self.ntree_start = ntree_start
        self.ntree_end = ntree_end
        self.eval_period = eval_period
        self.thread_count = thread_count
        self.verbose = verbose

    def __dealloc__(self):
        del self.__calcer

    def __deepcopy__(self, _):
        raise CatboostError('Can\'t deepcopy _StagedPredictIterator object')

    def next(self):
        if self.ntree_start >= self.ntree_end:
            raise StopIteration

        cdef yvector[yvector[double]] pred
        cdef EPredictionType predictionType = PyPredictionType(self.prediction_type).predictionType
        pred = ApplyModelMulti(dereference(self.__calcer),
                               dereference(self.pool.__pool),
                               self.verbose,
                               PyPredictionType('RawFormulaVal').predictionType,
                               self.ntree_start,
                               min(self.ntree_start + self.eval_period, self.ntree_end),
                               self.thread_count)
        if self.__approx.empty():
            self.__approx = pred
        else:
            for i in range(self.__approx.size()):
                for j in range(self.__approx[0].size()):
                    self.__approx[i][j] += pred[i][j]
        pred = PrepareEval(predictionType, self.__approx, 1)
        self.ntree_start += self.eval_period
        return [[value for value in vec] for vec in pred]

log_out = None

cdef void _LogPrinter(const char* str, size_t len) except * with gil:
    cdef bytes bytes_str = str[:len]
    log_out.write(to_native_str(bytes_str))

cpdef _set_logger(out):
    global log_out
    log_out = out
    SetCustomLoggingFunction(&_LogPrinter)

cpdef _reset_logger():
    RestoreOriginalLogger()
