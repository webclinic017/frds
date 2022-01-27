#include "IsolationForest.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>

#ifndef EULER_GAMMA
#define EULER_GAMMA 0.57721566490153286
#endif

void IsolationForest::growTree(std::vector<size_t> &sample,
                               std::unique_ptr<IsolationTree::Node> &node,
                               int const height) {
  auto nObs = sample.size();
  if ((nObs <= 1) || (height >= this->maxTreeHeight)) {
    node = std::make_unique<IsolationTree::Node>(-1, NAN, nullptr, true, nObs);
    return;
  }
  // Here, randomly pick an attribute
  auto attr = this->uniformDist(this->randomGen);

  std::vector<size_t> lobs, robs;
  lobs.reserve(nObs);
  robs.reserve(nObs);
  std::uniform_int_distribution<size_t> dist(0, nObs - 1);

  if (attr < this->n_num_attrs) {
    // When the datatype of this attribute is numeric, pick a split value
    // directly from numpy array.
    DataType val = *(DataType *)PyArray_GETPTR2(this->num_data, attr,
                                                sample[dist(this->randomGen)]);
    for (auto &i : sample) {
      auto obsVal = *(DataType *)PyArray_GETPTR2(this->num_data, attr, i);
      // NAN is set to be "smaller" than any value
      // If the split value is NAN, then obs with NAN are pushed left
      if (isnan(val)) {
        if (isnan(obsVal)) {
          lobs.push_back(i);
        } else {
          robs.push_back(i);
        }
        // Split value is not NAN, then obs with NAN or smaller are pushed left
      } else {
        if ((isnan(obsVal)) || (obsVal <= val)) {
          lobs.push_back(i);
        } else {
          robs.push_back(i);
        }
      }
    }

    node =
        std::make_unique<IsolationTree::Node>(attr, val, nullptr, false, nObs);

  } else {
    // The datatype of the attribute is string
    CharDataType val =
        (CharDataType)PyArray_GETPTR2(this->char_data, attr - this->n_num_attrs,
                                      sample[dist(this->randomGen)]);
    const auto val_len = strlen(val);
    for (auto &i : sample) {
      auto obsVal = (CharDataType)PyArray_GETPTR2(this->char_data,
                                                  attr - this->n_num_attrs, i);
      const auto obsVal_len = strlen(obsVal);
      if (obsVal_len < val_len) {
        lobs.push_back(i);
      } else if (obsVal_len > val_len) {
        robs.push_back(i);
      } else if (strcmp(obsVal, val) <= 0) {
        lobs.push_back(i);
      } else {
        robs.push_back(i);
      }
    }

    node = std::make_unique<IsolationTree::Node>(attr, NAN, val, false, nObs);
  }

  growTree(lobs, node->lnode, height + 1);
  growTree(robs, node->rnode, height + 1);
}

IsolationForest::IsolationForest(PyArrayObject *num_data,
                                 PyArrayObject *char_data,
                                 size_t const &treeSize,
                                 size_t const &forestSize,
                                 size_t const &randomSeed)
    : treeSize(treeSize),
      forestSize(forestSize),
      randomSeed(randomSeed),
      maxTreeHeight(ceil(log2((double)treeSize))),
      n_num_attrs(PyArray_DIM(num_data, 0)),
      n_char_attrs(PyArray_DIM(char_data, 0)),
      nObs(PyArray_DIM(num_data, 1)) {
  this->num_data = num_data;
  this->char_data = char_data;
  this->trees.reserve(forestSize);
  this->randomGen = std::mt19937_64(randomSeed);
  this->uniformDist =
      std::uniform_int_distribution<size_t>(0, n_num_attrs + n_char_attrs - 1);
}

inline double IsolationForest::averagePathLength(size_t const &nObs) {
  auto n = (double)nObs;
  return 2 * (log(n - 1) + EULER_GAMMA) - (2 * (n - 1) / n);
}

void IsolationForest::growForest() {
  // Make a vector from 0 to (nObs-1) representing the indices of observations
  std::vector<size_t> obs(this->nObs);
  std::iota(obs.begin(), obs.end(), 0);

  for (size_t i = 0; i < this->forestSize; i++) {
    // Sample `treeSize` observations without replacement
    std::vector<size_t> sample;
    std::sample(obs.begin(), obs.end(), std::back_inserter(sample),
                this->treeSize, this->randomGen);

    auto tree = std::make_unique<IsolationTree>();
    this->growTree(sample, tree->root);
    this->trees.push_back(std::move(tree));
  }
}

double IsolationForest::anomalyScore(size_t const &ob) {
  double avg = 0;
  for (auto &tree : this->trees) avg += this->pathLength(ob, tree->root);
  avg /= this->forestSize;
  return pow(2, -avg / this->averagePathLength(this->treeSize));
}

double IsolationForest::pathLength(size_t const &ob,
                                   std::unique_ptr<IsolationTree::Node> &node,
                                   int length) {
  if (node->isExNode) {
    if (node->nObs <= 1) return length;
    return length + this->averagePathLength(node->nObs);
  }
  if (node->splitAttribute < this->n_num_attrs) {
    DataType val =
        *(DataType *)PyArray_GETPTR2(this->num_data, node->splitAttribute, ob);
    if (val <= node->splitValue) {
      return this->pathLength(ob, node->lnode, length + 1);
    } else {
      return this->pathLength(ob, node->rnode, length + 1);
    }
  } else {
    CharDataType val = (CharDataType)PyArray_GETPTR2(
        this->char_data, node->splitAttribute - this->n_num_attrs, ob);
    const auto val_len = strlen(val);
    const auto splitVal_len = strlen(node->splitChar);
    if (val_len < splitVal_len) {
      return this->pathLength(ob, node->lnode, length + 1);
    } else if (val_len > splitVal_len) {
      return this->pathLength(ob, node->rnode, length + 1);
    } else if (strcmp(val, node->splitChar) <= 0) {
      return this->pathLength(ob, node->lnode, length + 1);
    } else {
      return this->pathLength(ob, node->rnode, length + 1);
    }
  }
}

std::thread IsolationForest::grow(const unsigned int jobs) {
  return std::thread([this, jobs] {
    std::vector<size_t> obs(this->nObs);
    std::iota(obs.begin(), obs.end(), 0);
    std::vector<size_t> sample;
    for (size_t i = 0; i < jobs; i++) {
      std::sample(obs.begin(), obs.end(), std::back_inserter(sample),
                  this->treeSize, this->randomGen);
      auto tree = std::make_unique<IsolationTree>();
      growTree(sample, tree->root);
      this->mylock.lock();
      this->trees.push_back(std::move(tree));
      this->mylock.unlock();
    }
  });
};