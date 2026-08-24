#include "model_cache/events.hpp"
#include "model_cache/error.hpp"

namespace model_cache {

Subscription::~Subscription() { unsubscribe(); }

void Subscription::unsubscribe() { if (bus_) bus_->unsubscribe(id_); }

EventBus::EventBus(std::uint32_t max_subs, std::uint32_t max_backlog)
    : max_subs_(max_subs), max_backlog_(max_backlog), published_(0) {}

Subscription EventBus::subscribe(EventCallback cb) {
  std::lock_guard<std::mutex> g(mu_);
  if (subs_.size() >= max_subs_)
    fail(ErrorCode::capacity_exceeded, "event subscription bound reached");
  Entry e;
  e.id = next_id_++;
  e.cb = std::move(cb);
  subs_.push_back(std::move(e));
  return Subscription(subs_.back().id, this);
}

void EventBus::unsubscribe(std::uint32_t id) {
  std::lock_guard<std::mutex> g(mu_);
  for (auto& s : subs_) if (s.id == id) { s.active = false; break; }
}

void EventBus::emit(EventType type, std::map<std::string, std::string> fields) {
  Event ev;
  ev.type = type;
  ev.sequence = ++next_seq_;
  ev.timestamp = 0;  // filled from a cheap clock by caller if needed
  ev.fields = std::move(fields);

  std::vector<Entry> targets;
  {
    std::lock_guard<std::mutex> g(mu_);
    targets.reserve(subs_.size());
    for (const auto& s : subs_) if (s.active) targets.push_back(s);
    backlog_.push_back(ev);
    if (backlog_.size() > max_backlog_) backlog_.erase(backlog_.begin(), backlog_.begin() + 1);
  }
  published_.store(ev.sequence, std::memory_order_relaxed);
  // Invoke callbacks with NO lock held.
  for (auto& t : targets) {
    try { t.cb(ev); } catch (...) { /* subscriber errors never break the runtime */ }
  }
}

std::size_t EventBus::subscriber_count() const {
  std::lock_guard<std::mutex> g(mu_);
  std::size_t n = 0;
  for (const auto& s : subs_) if (s.active) ++n;
  return n;
}

std::vector<Event> EventBus::recent(std::size_t max) {
  std::lock_guard<std::mutex> g(mu_);
  auto start = backlog_.size() > max ? backlog_.size() - max : 0;
  return std::vector<Event>(backlog_.begin() + start, backlog_.end());
}

}  // namespace model_cache
