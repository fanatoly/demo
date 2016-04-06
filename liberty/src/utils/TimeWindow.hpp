#pragma once
#include <chrono>
#include <vector>
#include <deque>
#include <algorithm>
#include <concord/Computation.hpp>
#include <concord/time_utils.hpp>
#include <glog/logging.h>
#include "WindowOptions.hpp"
namespace concord {
template <class ReducerType> class TimeWindow : public bolt::Computation {
  public:
  using CtxPtr = bolt::Computation::CtxPtr;

  TimeWindow(const WindowOptions<ReducerType> &options) : opts_(options) {}
  virtual ~TimeWindow() {}

  void destroy() override {}
  void init(CtxPtr ctx) override {
    ctx->setTimer("interval_loop", bolt::timeNowMilli());
  }

  void processTimer(CtxPtr ctx, const std::string &key, int64_t time) override {
    // Every 'slideInterval_' period create a new window
    // Every 'windowLength_' period evaluate all windows
    if(key == "interval_loop") {
      LOG(INFO) << "Creating window";
      windows_.push_back(Window(opts_.windowLength));
      const auto windowClose = windows_.back().end_;
      const auto nextWindow =
        windows_.back().begin_ + opts_.slideInterval.count();
      ctx->setTimer("interval_loop", nextWindow);
      ctx->setTimer("window_loop", windowClose);
    } else if(key == "window_loop") {
      LOG(INFO) << "Processing window";
      processWindows(ctx);
    } else {
      throw std::logic_error("Unexpected type of timer was set");
    }
  }

  void processRecord(CtxPtr ctx, bolt::FrameworkRecord &&r) override {
    auto recordPtr = std::make_shared<bolt::FrameworkRecord>(r);
    recordPtr->key = std::move(r.key);
    recordPtr->value = std::move(r.value);
    recordPtr->time = bolt::timeNowMilli();
    for(auto &w : windows_) {
      if(w.isWithinWindow(recordPtr.get())) {
        w.records_.push_back(recordPtr);
      }
    }
  }

  bolt::Metadata metadata() override { return opts_.metadata; }

  private:
  void processWindows(CtxPtr ctx) {
    // Only perform processing on closed windows.. windows will be queued in
    // chronological order
    while(!windows_.empty() && windows_.front().isWindowClosed()) {
      LOG(INFO) << "Closing window!";
      const auto &w = windows_.front();
      // Reduce all values inside of window using user supplied reducer func
      ReducerType acc = std::accumulate(
        w.records_.begin(), w.records_.end(), ReducerType(),
        [this](ReducerType &a, std::shared_ptr<bolt::FrameworkRecord> rec) {
          return opts_.reducerFn(a, rec.get());
        });
      // When the result has finished calculating, call a user supplied
      // callback and produce the result onto any downstream subscribers. The
      // key being the windowID and the value being the calculated result
      opts_.resultFn(w.begin_, acc);
      for(const auto stream : opts_.metadata.ostreams) {
        ctx->produceRecord(stream, std::to_string(w.begin_),
                           opts_.serializerFn(acc));
      }
      windows_.pop_front();
    }
  }

  struct Window {
    Window(const std::chrono::milliseconds &windowLength)
      : begin_(bolt::timeNowMilli()), end_(begin_ + windowLength.count()) {}

    bool isWithinWindow(const bolt::FrameworkRecord *r) const {
      const auto utime = static_cast<uint64_t>(r->time);
      return utime > begin_ && utime < end_;
    }

    bool isWindowClosed() const { return bolt::timeNowMilli() >= end_; }

    // all time measured in milliseconds since epoch
    const uint64_t begin_;
    const uint64_t end_;
    std::vector<std::shared_ptr<bolt::FrameworkRecord>> records_;
  };

  private:
  const WindowOptions<ReducerType> opts_;
  std::deque<Window> windows_;
};
}