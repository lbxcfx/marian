#pragma once

#include <future>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "3rd_party/threadpool.h"
#include "training/dropper.h"
#include "training/graph_group.h"
#include "training/sparse_tensor.h"
#include "training/validator.h"

namespace marian {

class SyncGraphGroup : public GraphGroup {
public:
  virtual void setScheduler(Ptr<Scheduler> scheduler) {
    scheduler_ = scheduler;
    // optimizer has to be registered last to see changes of learning rate
    scheduler_->registerTrainingObserver(scheduler_);

    for(auto opt : shardOpt_)
      scheduler_->registerTrainingObserver(opt);
  }

private:
  std::vector<Ptr<models::ModelBase>> builders_;
  std::vector<Ptr<ExpressionGraph>> graphs_;
  std::vector<size_t> devices_;

  std::vector<Tensor> params_;
  std::vector<Tensor> grads_;
  std::vector<Tensor> tmpTensors_;
  std::vector<Ptr<TensorAllocator>> paramsAllocs_;

  std::vector<Ptr<OptimizerBase>> shardOpt_;

  int shardSize_;
  bool first_{true};

  std::vector<Tensor> paramsAvg_;
  std::vector<Ptr<TensorAllocator>> paramsAllocAvg_;
  bool movingAvg_{false};
  float mvDecay_{0.9999};

  void updateMovingAverage(Tensor paramsAvg, Tensor params, size_t batches) {
    float decay = min(mvDecay_, (float)(batches + 1) / (float)(batches + 10));
    Element(_1 = (decay * _1) + ((1.f - decay) * _2), paramsAvg, params);
  }

  void fetchParams(Tensor oldParams, const std::vector<Tensor>& params) {
    // @TODO read guard on parameters
    int pos = 0;
    std::vector<std::thread> threads;
    for(int idx = 0; idx < devices_.size(); idx++) {
      threads.emplace_back(std::thread(
        [=](int idx, int pos) {
          oldParams->subtensor(pos, params[idx]->size())->copyFrom(params[idx]);
        },
        idx,
        pos));
      pos += shardSize_;
    }
    for(auto&& t : threads) {
      t.join();
    }
  }

  void execute(Ptr<data::Batch> batch) {
    std::vector<Ptr<data::Batch>> batches = batch->split(devices_.size());

    if(first_) {

      for(size_t i = 0; i < graphs_.size(); ++i) {
        // takes care of thead_local stuff
        THREAD_GUARD(
          builders_[i]->build(graphs_[i], batches[0]);
          graphs_[i]->forward();
        );

        if(i > 0)
          graphs_[i]->params()->vals()->copyFrom(graphs_[0]->params()->vals());
      }

      if(params_.size() == 0) {
        int totalSize = graphs_[0]->params()->vals()->size();
        shardSize_ = ceil(totalSize / (float)devices_.size());

        int pos = 0;
        for(auto device : devices_) {
          int __size__ = min(shardSize_, totalSize);

          auto paramsAlloc = New<TensorAllocator>(device);
          paramsAllocs_.push_back(paramsAlloc);

          paramsAlloc->reserveExact(3 * __size__ * sizeof(float));

          Tensor param, grad, tmp;
          paramsAlloc->allocate(param, {1, __size__});
          paramsAlloc->allocate(grad, {1, __size__});
          paramsAlloc->allocate(tmp, {1, __size__});
          params_.push_back(param);
          grads_.push_back(grad);
          tmpTensors_.push_back(tmp);

          param->copyFrom(graphs_[0]->params()->vals()->subtensor(pos, __size__));
          pos += __size__;
          totalSize -= __size__;
        }
      }

      if(movingAvg_ && paramsAvg_.size() == 0) {
        int totalSize = graphs_[0]->params()->vals()->size();

        int i = 0;
        for(auto device : devices_) {
          int __size__ = min(shardSize_, totalSize);
          totalSize -= __size__;
          Tensor paramAvg;
          auto allocator = New<TensorAllocator>(device);

          allocator->reserveExact(__size__ * sizeof(float));
          allocator->allocate(paramAvg, {1, __size__});

          paramAvg->copyFrom(params_[i++]);

          paramsAllocAvg_.push_back(allocator);
          paramsAvg_.push_back(paramAvg);
        }
      }

      first_ = false;
    }

    std::vector<float> costs(devices_.size());

    {
      auto task = [this, &costs, batches](size_t idx) {
        auto graph = graphs_[idx];
        auto batch = batches[idx];

        if(batch->size() > 0) {
          auto costNode = builders_[idx]->build(graph, batch);
          graph->forward();
          costs[idx] = costNode->scalar();
          graph->backward();
        }
      };

      ThreadPool pool(devices_.size(), devices_.size());
      for(int idx = 0; idx < batches.size(); ++idx)
        pool.enqueue(task, idx);
    }

    {
      auto task = [this, batches](size_t idx, int pos) {
        grads_[idx]->set(0);
        int size = params_[idx]->size();
        int i = 0;
        for(auto graph : graphs_) {
          if(batches[i]->size() > 0) {
            auto subGrad = graph->params()->grads()->subtensor(pos, size);
            tmpTensors_[idx]->copyFrom(subGrad);
            Element(_1 += _2, grads_[idx], tmpTensors_[idx]);
          }
          i++;
        }

        shardOpt_[idx]->update(params_[idx], grads_[idx]);

        if(movingAvg_)
          updateMovingAverage(paramsAvg_[idx], params_[idx],
                              scheduler_->numberOfBatches());

        for(auto graph : graphs_) {
          auto subParam = graph->params()->vals()->subtensor(pos, size);
          subParam->copyFrom(params_[idx]);
        }
      };

      ThreadPool pool(devices_.size(), devices_.size());
      int pos = 0;
      for(int idx = 0; idx < devices_.size(); ++idx) {
        pool.enqueue(task, idx, pos);
        pos += params_[idx]->size();
      }
    }

    float cost = 0;
    for(auto c : costs)
      cost += c;
    cost = cost / costs.size();

    if(scheduler_) {
      scheduler_->update(cost, batch);

      if(scheduler_->saving()) {
        this->save();
      }

      if(scheduler_->validating()) {
        if(movingAvg_)
          fetchParams(graphs_[0]->params()->vals(), paramsAvg_);

        scheduler_->validate(graphs_[0]);

        if(movingAvg_)
          fetchParams(graphs_[0]->params()->vals(), params_);
      }
    }
  }

public:
  template <class... Args>
  SyncGraphGroup(Ptr<Config> options, Args... args)
      : GraphGroup(options),
        devices_{options_->get<std::vector<size_t>>("devices")},
        movingAvg_{options_->get<bool>("moving-average")},
        mvDecay_{(float)options_->get<double>("moving-decay")}
  {
    for(auto device : devices_) {
      auto graph = New<ExpressionGraph>();
      graph->setDevice(device);
      graph->reserveWorkspaceMB(options_->get<size_t>("workspace"));
      graphs_.push_back(graph);
      shardOpt_.push_back(Optimizer(options_));
      builders_.push_back(models::from_config(options_));
    }
  }

  void update(Ptr<data::Batch> batch) { execute(batch); }

  void load() {
    if(!options_->get<bool>("no-reload")) {
      std::string init = options_->get<std::string>("model");
      if(boost::filesystem::exists(init)) {
        size_t i = 0;
        if(scheduler_)
          scheduler_->load(init);
        for(auto graph : graphs_)
          builders_[i++]->load(graph, init);
      }
    }
  }

  void save(bool final = false) { save(graphs_[0], final); }

  void save(Ptr<ExpressionGraph> graph, bool final = false) {
    int idx = 0;
    for(int i = 0; i < graphs_.size(); ++i) {
      if(graph == graphs_[i]) {
        idx = i;
        break;
      }
    }

    if(movingAvg_)
      fetchParams(graphs_[idx]->params()->vals(), paramsAvg_);

    if(options_->get<bool>("overwrite")) {
      std::string name = options_->get<std::string>("model");

      builders_[idx]->save(graphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    } else {
      std::string name = options_->get<std::string>("model");

      if(!final) {
        std::string numberOfBatches
            = scheduler_ ? std::to_string(scheduler_->numberOfBatches()) :
                           "unknown";
        std::string nameOverwrite = name;
        nameOverwrite.replace(
            name.size() - 4, 4, ".iter" + numberOfBatches + ".npz");
        builders_[idx]->save(graphs_[idx], nameOverwrite);
      }

      builders_[idx]->save(graphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    }

    if(movingAvg_)
      fetchParams(graphs_[idx]->params()->vals(), params_);
  }

  Ptr<data::BatchStats> collectStats() {
    return builders_[0]->collectStats(graphs_[0], devices_.size());
  }
};

}