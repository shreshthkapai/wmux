#include "wmux/session_manager.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace wmux {
namespace {

bool contains_window_name(const SessionSummary& session, std::string_view name) {
  return std::any_of(session.windows.begin(), session.windows.end(), [&](const auto& window) {
    return window.name == name;
  });
}

WindowOperationResult missing_session(SessionId session_id) {
  return {false, WindowError::SessionNotFound, session_id};
}

PaneOperationResult missing_pane_session(SessionId session_id) {
  return {false, PaneError::SessionNotFound, session_id};
}

std::unique_ptr<PaneNode> clone_node(const PaneNode* node) {
  if (node == nullptr) {
    return nullptr;
  }

  return std::make_unique<PaneNode>(*node);
}

PaneNode make_split_node(PaneId existing_pane_id, PaneId new_pane_id, SplitDirection direction) {
  PaneNode split;
  split.kind = PaneNode::Kind::Split;
  split.direction = direction;
  split.ratio = 0.5;
  split.first = std::make_unique<PaneNode>(existing_pane_id);
  split.second = std::make_unique<PaneNode>(new_pane_id);
  return split;
}

bool replace_leaf_with_split(
    PaneNode& node,
    PaneId target,
    PaneId created,
    SplitDirection direction) {
  if (node.kind == PaneNode::Kind::Leaf) {
    if (node.pane_id != target) {
      return false;
    }

    node = make_split_node(target, created, direction);
    return true;
  }

  if (node.first && replace_leaf_with_split(*node.first, target, created, direction)) {
    return true;
  }

  return node.second && replace_leaf_with_split(*node.second, target, created, direction);
}

std::optional<std::reference_wrapper<WindowSummary>> active_window(SessionSummary& session) {
  auto found = std::find_if(
      session.windows.begin(),
      session.windows.end(),
      [&](const auto& window) { return window.id == session.active_window_id; });
  if (found == session.windows.end()) {
    return std::nullopt;
  }

  return *found;
}

struct NormalizedPaneRect {
  PaneId pane_id{0};
  double left{0.0};
  double top{0.0};
  double right{0.0};
  double bottom{0.0};
};

void collect_pane_rects(
    const PaneNode& node,
    double left,
    double top,
    double right,
    double bottom,
    std::vector<NormalizedPaneRect>& rects) {
  if (node.kind == PaneNode::Kind::Leaf) {
    rects.push_back({node.pane_id, left, top, right, bottom});
    return;
  }

  if (!node.first || !node.second) {
    return;
  }

  const double ratio = std::clamp(node.ratio, 0.05, 0.95);
  if (node.direction == SplitDirection::Horizontal) {
    const double split = left + ((right - left) * ratio);
    collect_pane_rects(*node.first, left, top, split, bottom, rects);
    collect_pane_rects(*node.second, split, top, right, bottom, rects);
    return;
  }

  const double split = top + ((bottom - top) * ratio);
  collect_pane_rects(*node.first, left, top, right, split, rects);
  collect_pane_rects(*node.second, left, split, right, bottom, rects);
}

double vertical_overlap(const NormalizedPaneRect& a, const NormalizedPaneRect& b) {
  return std::max(0.0, std::min(a.bottom, b.bottom) - std::max(a.top, b.top));
}

double horizontal_overlap(const NormalizedPaneRect& a, const NormalizedPaneRect& b) {
  return std::max(0.0, std::min(a.right, b.right) - std::max(a.left, b.left));
}

std::optional<PaneId> pane_neighbor(
    const std::vector<NormalizedPaneRect>& rects,
    PaneId active_pane_id,
    PaneDirection direction) {
  const auto active = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return rect.pane_id == active_pane_id;
  });
  if (active == rects.end()) {
    return std::nullopt;
  }

  PaneId best_pane_id = 0;
  double best_distance = std::numeric_limits<double>::max();
  double best_overlap = -1.0;

  for (const auto& candidate : rects) {
    if (candidate.pane_id == active_pane_id) {
      continue;
    }

    double distance = 0.0;
    double overlap = 0.0;
    switch (direction) {
      case PaneDirection::Left:
        if (candidate.right > active->left) {
          continue;
        }
        distance = active->left - candidate.right;
        overlap = vertical_overlap(*active, candidate);
        break;
      case PaneDirection::Right:
        if (candidate.left < active->right) {
          continue;
        }
        distance = candidate.left - active->right;
        overlap = vertical_overlap(*active, candidate);
        break;
      case PaneDirection::Up:
        if (candidate.bottom > active->top) {
          continue;
        }
        distance = active->top - candidate.bottom;
        overlap = horizontal_overlap(*active, candidate);
        break;
      case PaneDirection::Down:
        if (candidate.top < active->bottom) {
          continue;
        }
        distance = candidate.top - active->bottom;
        overlap = horizontal_overlap(*active, candidate);
        break;
    }

    if (overlap <= 0.0) {
      continue;
    }

    if (distance < best_distance ||
        (std::abs(distance - best_distance) < 0.000001 && overlap > best_overlap)) {
      best_distance = distance;
      best_overlap = overlap;
      best_pane_id = candidate.pane_id;
    }
  }

  if (best_pane_id == 0) {
    return std::nullopt;
  }

  return best_pane_id;
}

int split_extent(int extent, double ratio) {
  if (extent <= 1) {
    return 1;
  }

  const auto clamped_ratio = std::clamp(ratio, 0.05, 0.95);
  return std::clamp(
      static_cast<int>(std::lround(static_cast<double>(extent) * clamped_ratio)),
      1,
      extent - 1);
}

void collect_integer_layout_rects(
    const PaneNode& node,
    int left,
    int top,
    int width,
    int height,
    std::vector<PaneLayoutRect>& rects) {
  if (width <= 0 || height <= 0) {
    return;
  }

  if (node.kind == PaneNode::Kind::Leaf) {
    rects.push_back(PaneLayoutRect{node.pane_id, left, top, width, height});
    return;
  }

  if (!node.first || !node.second) {
    return;
  }

  if (node.direction == SplitDirection::Horizontal) {
    const int first_width = split_extent(width, node.ratio);
    collect_integer_layout_rects(*node.first, left, top, first_width, height, rects);
    collect_integer_layout_rects(
        *node.second, left + first_width, top, width - first_width, height, rects);
    return;
  }

  const int first_height = split_extent(height, node.ratio);
  collect_integer_layout_rects(*node.first, left, top, width, first_height, rects);
  collect_integer_layout_rects(
      *node.second, left, top + first_height, width, height - first_height, rects);
}

}  // namespace

PaneNode::PaneNode(PaneId leaf_pane_id) : kind{Kind::Leaf}, pane_id{leaf_pane_id} {}

PaneNode::PaneNode(const PaneNode& other)
    : kind{other.kind},
      pane_id{other.pane_id},
      direction{other.direction},
      ratio{other.ratio},
      first{clone_node(other.first.get())},
      second{clone_node(other.second.get())} {}

PaneNode& PaneNode::operator=(const PaneNode& other) {
  if (this == &other) {
    return *this;
  }

  kind = other.kind;
  pane_id = other.pane_id;
  direction = other.direction;
  ratio = other.ratio;
  first = clone_node(other.first.get());
  second = clone_node(other.second.get());
  return *this;
}

SessionOperationResult SessionManager::create_session(std::string name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  if (name_index_.contains(name)) {
    return {false, SessionError::DuplicateName};
  }

  SessionSummary session;
  session.id = next_id_++;
  session.name = std::move(name);
  session.created_at = std::chrono::system_clock::now();

  WindowSummary initial_window;
  initial_window.id = next_window_id_++;
  initial_window.name = "0";
  initial_window.created_at = session.created_at;
  PaneSummary initial_pane;
  initial_pane.id = next_pane_id_++;
  initial_pane.created_at = session.created_at;
  initial_window.active_pane_id = initial_pane.id;
  initial_window.pane_tree = PaneNode{initial_pane.id};
  initial_window.panes.push_back(std::move(initial_pane));
  session.active_window_id = initial_window.id;
  session.windows.push_back(std::move(initial_window));

  const auto id = session.id;
  const auto window_id = session.active_window_id;
  const auto pane_id = session.windows.front().active_pane_id;
  name_index_.emplace(session.name, id);
  sessions_.emplace(id, std::move(session));
  order_.push_back(id);

  return {true, SessionError::None, id, window_id, pane_id};
}

SessionOperationResult SessionManager::rename_session(
    std::string_view current_name,
    std::string new_name) {
  if (current_name.empty() || new_name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto id = session_id_for_name(current_name);
  if (!id) {
    return {false, SessionError::NotFound};
  }

  const auto session = sessions_.find(*id);
  if (session == sessions_.end()) {
    return {false, SessionError::NotFound};
  }

  if (session->second.name != new_name && name_index_.contains(new_name)) {
    return {false, SessionError::DuplicateName};
  }

  if (session->second.name != new_name) {
    name_index_.erase(session->second.name);
    session->second.name = std::move(new_name);
    name_index_.emplace(session->second.name, *id);
  }

  return {true, SessionError::None, *id};
}

SessionOperationResult SessionManager::kill_session(std::string_view name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  const auto id = session_id_for_name(name);
  if (!id) {
    return {false, SessionError::NotFound};
  }

  const auto session = sessions_.find(*id);
  if (session != sessions_.end()) {
    name_index_.erase(session->second.name);
    sessions_.erase(session);
  }

  order_.erase(std::remove(order_.begin(), order_.end(), *id), order_.end());
  return {true, SessionError::None, *id};
}

WindowOperationResult SessionManager::create_window(SessionId session_id, std::string name) {
  if (name.empty()) {
    return {false, WindowError::EmptyName, session_id};
  }

  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (contains_window_name(session->second, name)) {
    return {false, WindowError::DuplicateName, session_id};
  }

  WindowSummary window;
  window.id = next_window_id_++;
  window.name = std::move(name);
  window.created_at = std::chrono::system_clock::now();
  PaneSummary pane;
  pane.id = next_pane_id_++;
  pane.created_at = window.created_at;
  window.active_pane_id = pane.id;
  window.pane_tree = PaneNode{pane.id};
  window.panes.push_back(std::move(pane));

  const auto window_id = window.id;
  const auto pane_id = window.active_pane_id;
  session->second.windows.push_back(std::move(window));
  session->second.active_window_id = window_id;
  return {true, WindowError::None, session_id, window_id, pane_id};
}

WindowOperationResult SessionManager::rename_active_window(
    SessionId session_id,
    std::string name) {
  if (name.empty()) {
    return {false, WindowError::EmptyName, session_id};
  }

  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  const auto active_id = session->second.active_window_id;
  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == active_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active->name != name && contains_window_name(session->second, name)) {
    return {false, WindowError::DuplicateName, session_id};
  }

  active->name = std::move(name);
  return {true, WindowError::None, session_id, active_id, active->active_pane_id};
}

WindowOperationResult SessionManager::select_next_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  ++active;
  if (active == session->second.windows.end()) {
    active = session->second.windows.begin();
  }
  session->second.active_window_id = active->id;
  return {true, WindowError::None, session_id, active->id, active->active_pane_id};
}

WindowOperationResult SessionManager::select_previous_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active == session->second.windows.begin()) {
    active = session->second.windows.end();
  }
  --active;
  session->second.active_window_id = active->id;
  return {true, WindowError::None, session_id, active->id, active->active_pane_id};
}

PaneOperationResult SessionManager::split_active_pane(
    SessionId session_id,
    SplitDirection direction) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto active = active_window(session->second);
  if (!active) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = active->get();
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  const PaneId new_pane_id = next_pane_id_++;
  if (!replace_leaf_with_split(window.pane_tree, window.active_pane_id, new_pane_id, direction)) {
    --next_pane_id_;
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  PaneSummary pane;
  pane.id = new_pane_id;
  pane.created_at = std::chrono::system_clock::now();
  window.panes.push_back(std::move(pane));
  window.active_pane_id = new_pane_id;

  return {true, PaneError::None, session_id, window.id, new_pane_id};
}

PaneOperationResult SessionManager::select_pane(SessionId session_id, PaneDirection direction) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto active = active_window(session->second);
  if (!active) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = active->get();
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  std::vector<NormalizedPaneRect> rects;
  rects.reserve(window.panes.size());
  collect_pane_rects(window.pane_tree, 0.0, 0.0, 1.0, 1.0, rects);

  const auto neighbor = pane_neighbor(rects, window.active_pane_id, direction);
  if (!neighbor) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  window.active_pane_id = *neighbor;
  return {true, PaneError::None, session_id, window.id, *neighbor};
}

bool SessionManager::has_session(std::string_view name) const {
  return session_id_for_name(name).has_value();
}

std::optional<SessionId> SessionManager::session_id_for_name(std::string_view name) const {
  const auto found = name_index_.find(std::string{name});
  if (found == name_index_.end()) {
    return std::nullopt;
  }

  return found->second;
}

std::optional<SessionId> SessionManager::only_session_id() const {
  if (order_.size() != 1) {
    return std::nullopt;
  }

  const auto session = sessions_.find(order_.front());
  if (session == sessions_.end()) {
    return std::nullopt;
  }

  return session->first;
}

std::optional<WindowId> SessionManager::active_window_id(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end() || session->second.active_window_id == 0) {
    return std::nullopt;
  }

  return session->second.active_window_id;
}

std::optional<PaneId> SessionManager::active_pane_id(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end() || session->second.active_window_id == 0) {
    return std::nullopt;
  }

  const auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end() || active->active_pane_id == 0) {
    return std::nullopt;
  }

  return active->active_pane_id;
}

std::optional<WindowSummary> SessionManager::active_window_summary(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end() || session->second.active_window_id == 0) {
    return std::nullopt;
  }

  const auto active = std::find_if(
      session->second.windows.begin(),
      session->second.windows.end(),
      [&](const auto& window) { return window.id == session->second.active_window_id; });
  if (active == session->second.windows.end()) {
    return std::nullopt;
  }

  return *active;
}

std::size_t SessionManager::session_count() const {
  return sessions_.size();
}

std::vector<SessionSummary> SessionManager::list_sessions() const {
  std::vector<SessionSummary> listed;
  listed.reserve(order_.size());
  for (const auto id : order_) {
    const auto session = sessions_.find(id);
    if (session != sessions_.end()) {
      listed.push_back(session->second);
    }
  }

  return listed;
}

std::vector<WindowSummary> SessionManager::list_windows(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return {};
  }

  return session->second.windows;
}

std::vector<PaneLayoutRect> compute_pane_layout_rects(
    const PaneNode& root,
    int columns,
    int rows) {
  std::vector<PaneLayoutRect> rects;
  collect_integer_layout_rects(root, 0, 0, columns, rows, rects);
  return rects;
}

}  // namespace wmux
