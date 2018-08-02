/*
 * Copyright (C) 2018 Swift Navigation Inc.
 * Contact: Swift Navigation <dev@swiftnav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef ALBATROSS_CORE_MODEL_H
#define ALBATROSS_CORE_MODEL_H

#include "distribution.h"
#include "map_utils.h"
#include "parameter_handling_mixin.h"
#include "traits.h"
#include <Eigen/Core>
#include <cereal/archives/json.hpp>
#include <map>
#include <vector>

namespace albatross {

namespace detail {
// This is effectively just a container that allows us to develop methods
// which behave different conditional on the type of predictions desired.
template <typename T> struct PredictTypeIdentity { typedef T type; };
}

// This can be used to make intentions more obvious when calling
// predict variants for which you only want the mean.
using PredictMeanOnly = Eigen::VectorXd;

/*
 * A RegressionDataset holds two vectors of data, the features
 * where a single feature can be any class that contains the information used
 * to make predictions of the target.  This is called a RegressionDataset since
 * it is assumed that each feature is regressed to a single double typed
 * target.
 */
template <typename FeatureType> struct RegressionDataset {
  std::vector<FeatureType> features;
  MarginalDistribution targets;
  std::map<std::string, std::string> metadata;

  RegressionDataset(){};

  RegressionDataset(const std::vector<FeatureType> &features_,
                    const MarginalDistribution &targets_)
      : features(features_), targets(targets_) {
    // If the two inputs aren't the same size they clearly aren't
    // consistent.
    assert(static_cast<int>(features.size()) ==
           static_cast<int>(targets.size()));
  }

  RegressionDataset(const std::vector<FeatureType> &features_,
                    const Eigen::VectorXd &targets_)
      : RegressionDataset(features_, MarginalDistribution(targets_)) {}

  bool operator==(const RegressionDataset &other) const {
    return (features == other.features && targets == other.targets &&
            metadata == other.metadata);
  }

  template <class Archive>
  typename std::enable_if<valid_in_out_serializer<FeatureType, Archive>::value,
                          void>::type
  serialize(Archive &archive) {
    archive(cereal::make_nvp("features", features));
    archive(cereal::make_nvp("targets", targets));
    archive(cereal::make_nvp("metadata", metadata));
  }

  template <class Archive>
  typename std::enable_if<!valid_in_out_serializer<FeatureType, Archive>::value,
                          void>::type
  serialize(Archive &archive) {
    static_assert(delay_static_assert<Archive>::value,
                  "In order to serialize a RegressionDataset the corresponding "
                  "FeatureType must be serializable.");
  }
};

typedef int32_t s32;
using FoldIndices = std::vector<s32>;
using FoldName = std::string;
using FoldIndexer = std::map<FoldName, FoldIndices>;

/*
 * A combination of training and testing datasets, typically used in cross
 * validation.
 */
template <typename FeatureType> struct RegressionFold {
  RegressionDataset<FeatureType> train_dataset;
  RegressionDataset<FeatureType> test_dataset;
  FoldName name;
  FoldIndices test_indices;

  RegressionFold(const RegressionDataset<FeatureType> &train_dataset_,
                 const RegressionDataset<FeatureType> &test_dataset_,
                 const FoldName &name_, const FoldIndices &test_indices_)
      : train_dataset(train_dataset_), test_dataset(test_dataset_), name(name_),
        test_indices(test_indices_){};
};

/*
 * A model that uses a single Feature to estimate the value of a double typed
 * target.
 */
template <typename FeatureType>
class RegressionModel : public ParameterHandlingMixin {
public:
  using Feature = FeatureType;
  RegressionModel() : ParameterHandlingMixin(), has_been_fit_(){};
  virtual ~RegressionModel(){};

  virtual bool operator==(const RegressionModel<FeatureType> &other) const {
    // If the fit method has been called it's possible that some unknown
    // class members may have been modified.  As such, if a model has been
    // fit we fail hard to avoid possibly unexpected behavior.  Any
    // implementation that wants a functional equality operator after
    // having been fit will need to override this one.
    assert(!has_been_fit());
    return (get_name() == other.get_name() &&
            get_params() == other.get_params() &&
            has_been_fit() == other.has_been_fit());
  }

  /*
   * Provides a wrapper around the implementation `fit_` which performs
   * simple size checks and makes sure the fit method is called before
   * predict.
   */
  void fit(const std::vector<FeatureType> &features,
           const MarginalDistribution &targets) {
    assert(features.size() > 0);
    assert(static_cast<s32>(features.size()) ==
           static_cast<s32>(targets.size()));
    fit_(features, targets);
    has_been_fit_ = true;
  }

  /*
   * Convenience function which assumes zero target covariance.
   */
  void fit(const std::vector<FeatureType> &features,
           const Eigen::VectorXd &targets) {
    fit(features, MarginalDistribution(targets));
  }

  /*
   * Convenience function which unpacks a dataset into features and targets.
   */
  void fit(const RegressionDataset<FeatureType> &dataset) {
    fit(dataset.features, dataset.targets);
  }

  /*
   * Similar to fit, this predict methods wrap the implementation `predict_*_`
   * and makes simple checks to confirm the implementation is returning
   * properly sized Distribution.
   */
  template <typename PredictType = JointDistribution>
  PredictType predict(const std::vector<FeatureType> &features) const {
    return predict(features, detail::PredictTypeIdentity<PredictType>());
  }

  /*
   * Predict specializations
   */

  JointDistribution
  predict(const std::vector<FeatureType> &features,
          detail::PredictTypeIdentity<JointDistribution> &&identity) const {
    assert(has_been_fit());
    JointDistribution preds = predict_(features);
    assert(static_cast<s32>(preds.mean.size()) ==
           static_cast<s32>(features.size()));
    return preds;
  }

  MarginalDistribution
  predict(const std::vector<FeatureType> &features,
          detail::PredictTypeIdentity<MarginalDistribution> &&identity) const {
    assert(has_been_fit());
    MarginalDistribution preds = predict_marginal_(features);
    assert(static_cast<s32>(preds.mean.size()) ==
           static_cast<s32>(features.size()));
    return preds;
  }

  Eigen::VectorXd
  predict(const std::vector<FeatureType> &features,
          detail::PredictTypeIdentity<Eigen::VectorXd> &&identity) const {
    assert(has_been_fit());
    Eigen::VectorXd preds = predict_mean_(features);
    assert(static_cast<s32>(preds.size()) == static_cast<s32>(features.size()));
    return preds;
  }

  template <typename PredictType>
  PredictType predict(const FeatureType &feature) const {
    std::vector<FeatureType> features = {feature};
    return predict<PredictType>(features);
  }

  /*
   * Computes predictions for the test features given set of training
   * features and targets. In the general case this is simply a call to fit,
   * follwed by predict but overriding this method may speed up computation for
   * some models.
   */
  template <typename PredictType = JointDistribution>
  PredictType fit_and_predict(const std::vector<FeatureType> &train_features,
                              const MarginalDistribution &train_targets,
                              const std::vector<FeatureType> &test_features) {
    // Fit using the training data, then predict with the test.
    fit(train_features, train_targets);
    return predict<PredictType>(test_features);
  }

  /*
   * A convenience wrapper around fit_and_predict which uses the entries
   * in a RegressionFold struct
   */
  template <typename PredictType = JointDistribution>
  PredictType fit_and_predict(const RegressionFold<FeatureType> &fold) {
    return fit_and_predict<PredictType>(fold.train.features, fold.train.targets,
                                        fold.test.features);
  }

  std::string pretty_string() const {
    std::ostringstream ss;
    ss << get_name() << std::endl;
    ss << ParameterHandlingMixin::pretty_string();
    return ss.str();
  }

  virtual bool has_been_fit() const { return has_been_fit_; }

  virtual std::string get_name() const = 0;

  /*
   * Here we define the serialization routines.  Note that while in most
   * cases we could use the cereal method `serialize`, in this case we don't
   * know for sure where the parameters are stored.  The
   * GaussianProcessRegression
   * model, for example, derives its parameters from its covariance function,
   * so it's `params_` are actually empty.  As a result we need to use the
   * save/load cereal variant and deal with parameters through the get/set
   * interface.
   */
  template <class Archive> void save(Archive &archive) const {
    auto params = get_params();
    archive(cereal::make_nvp("parameters", params));
    archive(cereal::make_nvp("has_been_fit", has_been_fit_));
  }

  template <class Archive> void load(Archive &archive) {
    auto params = get_params();
    archive(cereal::make_nvp("parameters", params));
    archive(cereal::make_nvp("has_been_fit", has_been_fit_));
    set_params(params);
  }

protected:
  virtual void fit_(const std::vector<FeatureType> &features,
                    const MarginalDistribution &targets) = 0;

  virtual JointDistribution
  predict_(const std::vector<FeatureType> &features) const = 0;

  virtual MarginalDistribution
  predict_marginal_(const std::vector<FeatureType> &features) const {
    std::cout << "WARNING: A marginal prediction is being made, but in a "
                 "horribly inefficient way.";
    const auto full_distribution = predict_(features);
    return MarginalDistribution(
        full_distribution.mean,
        full_distribution.covariance.diagonal().asDiagonal());
  }

  virtual Eigen::VectorXd
  predict_mean_(const std::vector<FeatureType> &features) const {
    std::cout << "WARNING: A mean prediction is being made, but in a horribly "
                 "inefficient way.";
    const auto full_distribution = predict_(features);
    return full_distribution.mean;
  }

  bool has_been_fit_;
};

template <typename FeatureType>
using RegressionModelCreator =
    std::function<std::unique_ptr<RegressionModel<FeatureType>>()>;
} // namespace albatross

#endif
