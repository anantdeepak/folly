/*
 * Copyright 2016 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <folly/experimental/observer/detail/ObserverManager.h>

#include <folly/MPMCQueue.h>
#include <folly/Singleton.h>

namespace folly {
namespace observer_detail {

FOLLY_TLS bool ObserverManager::inManagerThread_{false};
FOLLY_TLS ObserverManager::DependencyRecorder::Dependencies*
    ObserverManager::DependencyRecorder::currentDependencies_{nullptr};

namespace {
constexpr size_t kCurrentThreadPoolSize{4};
constexpr size_t kCurrentQueueSize{10 * 1024};
constexpr size_t kNextQueueSize{10 * 1024};
}

class ObserverManager::CurrentQueue {
 public:
  CurrentQueue() : queue_(kCurrentQueueSize) {
    for (size_t i = 0; i < kCurrentThreadPoolSize; ++i) {
      threads_.emplace_back([&]() {
        ObserverManager::inManagerThread_ = true;

        while (true) {
          Function<void()> task;
          queue_.blockingRead(task);

          if (!task) {
            return;
          }

          try {
            task();
          } catch (...) {
            LOG(ERROR) << "Exception while running CurrentQueue task: "
                       << exceptionStr(std::current_exception());
          }
        }
      });
    }
  }

  ~CurrentQueue() {
    for (size_t i = 0; i < threads_.size(); ++i) {
      queue_.blockingWrite(nullptr);
    }

    for (auto& thread : threads_) {
      thread.join();
    }

    CHECK(queue_.isEmpty());
  }

  void add(Function<void()> task) {
    if (ObserverManager::inManagerThread()) {
      if (!queue_.write(std::move(task))) {
        throw std::runtime_error("Too many Observers scheduled for update.");
      }
    } else {
      queue_.blockingWrite(std::move(task));
    }
  }

 private:
  MPMCQueue<Function<void()>> queue_;
  std::vector<std::thread> threads_;
};

class ObserverManager::NextQueue {
 public:
  explicit NextQueue(ObserverManager& manager)
      : manager_(manager), queue_(kNextQueueSize) {
    thread_ = std::thread([&]() {
      Core::Ptr queueCore;

      while (true) {
        queue_.blockingRead(queueCore);

        if (!queueCore) {
          return;
        }

        std::vector<Core::Ptr> cores;
        cores.emplace_back(std::move(queueCore));

        {
          SharedMutexReadPriority::WriteHolder wh(manager_.versionMutex_);

          // We can't pick more tasks from the queue after we bumped the
          // version, so we have to do this while holding the lock.
          while (cores.size() < kNextQueueSize && queue_.read(queueCore)) {
            if (!queueCore) {
              return;
            }
            cores.emplace_back(std::move(queueCore));
          }

          ++manager_.version_;
        }

        for (auto& core : cores) {
          manager_.scheduleRefresh(std::move(core), manager_.version_, true);
        }
      }
    });
  }

  void add(Core::Ptr core) {
    queue_.blockingWrite(std::move(core));
  }

  ~NextQueue() {
    // Emtpy element signals thread to terminate
    queue_.blockingWrite(nullptr);
    thread_.join();
  }

 private:
  ObserverManager& manager_;
  MPMCQueue<Core::Ptr> queue_;
  std::thread thread_;
};

ObserverManager::ObserverManager() {
  currentQueue_ = make_unique<CurrentQueue>();
  nextQueue_ = make_unique<NextQueue>(*this);
}

ObserverManager::~ObserverManager() {
  // Destroy NextQueue, before the rest of this object, since it expects
  // ObserverManager to be alive.
  nextQueue_.release();
  currentQueue_.release();
}

void ObserverManager::scheduleCurrent(Function<void()> task) {
  currentQueue_->add(std::move(task));
}

void ObserverManager::scheduleNext(Core::Ptr core) {
  nextQueue_->add(std::move(core));
}

struct ObserverManager::Singleton {
  static folly::Singleton<ObserverManager> instance;
};

folly::Singleton<ObserverManager> ObserverManager::Singleton::instance([] {
  return new ObserverManager();
});

std::shared_ptr<ObserverManager> ObserverManager::getInstance() {
  return Singleton::instance.try_get();
}
}
}
