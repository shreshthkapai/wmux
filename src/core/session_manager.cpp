#include "wmux/session_manager.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace wmux {
namespace {

Window* window_by_id(std::unordered_map<WindowId, Window>& windows, WindowId id) {
  const auto found = windows.find(id);
  return found == windows.end() ? nullptr : &found->second;
}

const Window* window_by_id(const std::unordered_map<WindowId, Window>& windows, WindowId id) {
  const auto found = windows.find(id);
  return found == windows.end() ? nullptr : &found->second;
}

bool contains_window_name(
    const Session& session,
    const std::unordered_map<WindowId, Window>& windows,
    std::string_view name) {
  return std::any_of(session.windows.begin(), session.windows.end(), [&](const auto window_id) {
    const auto* window = window_by_id(windows, window_id);
    return window != nullptr && window->name == name;
  });
}

WindowOperationResult missing_session(SessionId session_id) {
  return {false, WindowError::SessionNotFound, session_id};
}

PaneOperationResult missing_pane_session(SessionId session_id) {
  return {false, PaneError::SessionNotFound, session_id};
}

double resized_boundary_ratio(const PaneSplitResizeTarget& target, int column, int row);
std::vector<int> largest_remainder_extents(int extent, const std::vector<double>& weights);

LayoutNode* layout_node(LayoutArena& arena, NodeId id) {
  const auto found = arena.nodes.find(id);
  return found == arena.nodes.end() ? nullptr : &found->second;
}

const LayoutNode* layout_node(const LayoutArena& arena, NodeId id) {
  const auto found = arena.nodes.find(id);
  return found == arena.nodes.end() ? nullptr : &found->second;
}

bool is_valid_layout_split(const LayoutNode& node) {
  return node.kind == LayoutNode::Kind::Split && node.children.size() >= 2;
}

std::vector<double> normalized_layout_child_weights(const LayoutNode& node) {
  std::vector<double> weights;
  weights.reserve(node.children.size());
  if (node.child_weights.size() == node.children.size()) {
    for (const auto weight : node.child_weights) {
      weights.push_back(std::isfinite(weight) ? std::max(0.0, weight) : 0.0);
    }
  } else {
    weights.assign(node.children.size(), 1.0);
  }

  const auto total = std::accumulate(weights.begin(), weights.end(), 0.0);
  if (total <= 0.000001) {
    weights.assign(node.children.size(), 1.0);
    return weights;
  }

  for (auto& weight : weights) {
    weight /= total;
  }
  return weights;
}

void normalize_layout_child_weights_in_place(LayoutNode& node) {
  node.child_weights = normalized_layout_child_weights(node);
}

NodeId allocate_layout_node(LayoutArena& arena, LayoutNode node) {
  node.id = arena.next_node_id++;
  const auto id = node.id;
  arena.nodes.emplace(id, std::move(node));
  return id;
}

NodeId create_layout_leaf(
    LayoutArena& arena,
    PaneId pane_id,
    std::optional<NodeId> parent_id = std::nullopt) {
  LayoutNode node;
  node.kind = LayoutNode::Kind::Leaf;
  node.parent_id = parent_id;
  node.pane_id = pane_id;
  const auto id = allocate_layout_node(arena, std::move(node));
  arena.pane_leaf_index[pane_id] = id;
  if (arena.root_id == 0) {
    arena.root_id = id;
  }
  return id;
}

NodeId create_layout_split(
    LayoutArena& arena,
    SplitDirection direction,
    std::vector<NodeId> children,
    std::vector<double> child_weights,
    std::optional<NodeId> parent_id = std::nullopt) {
  LayoutNode node;
  node.kind = LayoutNode::Kind::Split;
  node.parent_id = parent_id;
  node.direction = direction;
  node.children = std::move(children);
  node.child_weights = std::move(child_weights);
  const auto id = allocate_layout_node(arena, std::move(node));
  auto* split = layout_node(arena, id);
  if (split != nullptr) {
    normalize_layout_child_weights_in_place(*split);
    for (const auto child_id : split->children) {
      if (auto* child = layout_node(arena, child_id)) {
        child->parent_id = id;
      }
    }
  }
  if (arena.root_id == 0) {
    arena.root_id = id;
  }
  return id;
}

void initialize_window_layout(Window& window, PaneId pane_id) {
  window.layout = LayoutArena{};
  window.layout_root = create_layout_leaf(window.layout, pane_id);
}

PaneNode pane_tree_snapshot(const LayoutArena& arena, NodeId node_id) {
  PaneNode snapshot;
  const auto* node = layout_node(arena, node_id);
  if (node == nullptr) {
    return snapshot;
  }

  snapshot.node_id = node->id;
  if (node->kind == LayoutNode::Kind::Leaf) {
    snapshot.kind = PaneNode::Kind::Leaf;
    snapshot.pane_id = node->pane_id;
    return snapshot;
  }

  snapshot.kind = PaneNode::Kind::Split;
  snapshot.direction = node->direction;
  snapshot.child_weights = normalized_layout_child_weights(*node);
  snapshot.children.reserve(node->children.size());
  for (const auto child_id : node->children) {
    snapshot.children.push_back(pane_tree_snapshot(arena, child_id));
  }
  return snapshot;
}

LayoutNode* layout_node_at_path(
    LayoutArena& arena,
    NodeId root_id,
    const std::vector<PaneTreePathStep>& path) {
  auto* node = layout_node(arena, root_id);
  for (const auto step : path) {
    if (node == nullptr || node->kind != LayoutNode::Kind::Split ||
        step >= node->children.size()) {
      return nullptr;
    }
    node = layout_node(arena, node->children[step]);
  }
  return node;
}

std::optional<std::size_t> child_index_for_node(const LayoutNode& parent, NodeId child_id) {
  const auto found = std::find(parent.children.begin(), parent.children.end(), child_id);
  if (found == parent.children.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::distance(parent.children.begin(), found));
}

bool split_layout_leaf(
    LayoutArena& arena,
    PaneId target,
    PaneId created,
    SplitDirection direction) {
  const auto leaf_entry = arena.pane_leaf_index.find(target);
  if (leaf_entry == arena.pane_leaf_index.end()) {
    return false;
  }

  const auto leaf_id = leaf_entry->second;
  auto* leaf = layout_node(arena, leaf_id);
  if (leaf == nullptr || leaf->kind != LayoutNode::Kind::Leaf || leaf->pane_id != target) {
    return false;
  }

  const auto parent_id = leaf->parent_id;
  if (!parent_id) {
    const auto new_leaf_id = create_layout_leaf(arena, created);
    const auto split_id = create_layout_split(
        arena,
        direction,
        {leaf_id, new_leaf_id},
        {0.5, 0.5});
    if (auto* existing_leaf = layout_node(arena, leaf_id)) {
      existing_leaf->parent_id = split_id;
    }
    if (auto* new_leaf = layout_node(arena, new_leaf_id)) {
      new_leaf->parent_id = split_id;
    }
    arena.root_id = split_id;
    return true;
  }

  auto* parent = layout_node(arena, *parent_id);
  if (parent == nullptr || parent->kind != LayoutNode::Kind::Split) {
    return false;
  }

  const auto leaf_index = child_index_for_node(*parent, leaf_id);
  if (!leaf_index) {
    return false;
  }

  if (parent->direction != direction) {
    const auto new_leaf_id = create_layout_leaf(arena, created);
    const auto split_id = create_layout_split(
        arena,
        direction,
        {leaf_id, new_leaf_id},
        {0.5, 0.5},
        parent->id);
    parent = layout_node(arena, *parent_id);
    if (parent == nullptr || *leaf_index >= parent->children.size()) {
      return false;
    }
    parent->children[*leaf_index] = split_id;
    if (auto* existing_leaf = layout_node(arena, leaf_id)) {
      existing_leaf->parent_id = split_id;
    }
    if (auto* new_leaf = layout_node(arena, new_leaf_id)) {
      new_leaf->parent_id = split_id;
    }
    normalize_layout_child_weights_in_place(*parent);
    return true;
  }

  auto weights = normalized_layout_child_weights(*parent);
  if (weights.size() != parent->children.size()) {
    weights.assign(parent->children.size(), 1.0 / static_cast<double>(parent->children.size()));
  }

  const auto new_leaf_id = create_layout_leaf(arena, created, parent->id);
  parent = layout_node(arena, *parent_id);
  if (parent == nullptr || *leaf_index >= parent->children.size()) {
    return false;
  }

  const auto insert_child =
      parent->children.begin() + static_cast<std::ptrdiff_t>(*leaf_index + 1);
  parent->children.insert(insert_child, new_leaf_id);

  const double split_weight = weights[*leaf_index] / 2.0;
  weights[*leaf_index] = split_weight;
  const auto insert_weight = weights.begin() + static_cast<std::ptrdiff_t>(*leaf_index + 1);
  weights.insert(insert_weight, split_weight);
  parent->child_weights = std::move(weights);
  normalize_layout_child_weights_in_place(*parent);
  return true;
}

std::optional<PaneId> first_layout_leaf_pane_id(const LayoutArena& arena, NodeId node_id) {
  const auto* node = layout_node(arena, node_id);
  if (node == nullptr) {
    return std::nullopt;
  }

  if (node->kind == LayoutNode::Kind::Leaf) {
    return node->pane_id;
  }

  for (const auto child_id : node->children) {
    const auto leaf = first_layout_leaf_pane_id(arena, child_id);
    if (leaf) {
      return leaf;
    }
  }
  return std::nullopt;
}

std::optional<PaneId> replacement_layout_leaf_after_removal(
    const LayoutArena& arena,
    const LayoutNode& parent,
    std::size_t removed_index) {
  if (removed_index + 1 < parent.children.size()) {
    if (const auto leaf = first_layout_leaf_pane_id(arena, parent.children[removed_index + 1])) {
      return leaf;
    }
  }

  if (removed_index > 0) {
    return first_layout_leaf_pane_id(arena, parent.children[removed_index - 1]);
  }

  for (std::size_t index = 0; index < parent.children.size(); ++index) {
    if (index == removed_index) {
      continue;
    }
    if (const auto leaf = first_layout_leaf_pane_id(arena, parent.children[index])) {
      return leaf;
    }
  }
  return std::nullopt;
}

void collapse_one_child_split(LayoutArena& arena, NodeId split_id) {
  auto* split = layout_node(arena, split_id);
  if (split == nullptr || split->kind != LayoutNode::Kind::Split || split->children.size() != 1) {
    return;
  }

  const auto child_id = split->children.front();
  const auto parent_id = split->parent_id;
  if (!parent_id) {
    arena.root_id = child_id;
    if (auto* child = layout_node(arena, child_id)) {
      child->parent_id = std::nullopt;
    }
    arena.nodes.erase(split_id);
    return;
  }

  auto* parent = layout_node(arena, *parent_id);
  if (parent == nullptr) {
    return;
  }

  const auto index = child_index_for_node(*parent, split_id);
  if (!index) {
    return;
  }

  parent->children[*index] = child_id;
  if (auto* child = layout_node(arena, child_id)) {
    child->parent_id = parent->id;
  }
  arena.nodes.erase(split_id);
  normalize_layout_child_weights_in_place(*parent);
}

bool remove_layout_leaf_and_collapse(LayoutArena& arena, PaneId target, PaneId& replacement) {
  const auto leaf_entry = arena.pane_leaf_index.find(target);
  if (leaf_entry == arena.pane_leaf_index.end()) {
    return false;
  }

  const auto leaf_id = leaf_entry->second;
  const auto* leaf = layout_node(arena, leaf_id);
  if (leaf == nullptr || leaf->kind != LayoutNode::Kind::Leaf || !leaf->parent_id) {
    return false;
  }

  const auto parent_id = *leaf->parent_id;
  auto* parent = layout_node(arena, parent_id);
  if (parent == nullptr || parent->kind != LayoutNode::Kind::Split) {
    return false;
  }

  const auto removed_index = child_index_for_node(*parent, leaf_id);
  if (!removed_index) {
    return false;
  }

  const auto next_active = replacement_layout_leaf_after_removal(arena, *parent, *removed_index);
  if (!next_active) {
    return false;
  }
  replacement = *next_active;

  parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(*removed_index));
  if (*removed_index < parent->child_weights.size()) {
    parent->child_weights.erase(
        parent->child_weights.begin() + static_cast<std::ptrdiff_t>(*removed_index));
  }
  arena.pane_leaf_index.erase(target);
  arena.nodes.erase(leaf_id);

  parent = layout_node(arena, parent_id);
  if (parent == nullptr) {
    return false;
  }
  normalize_layout_child_weights_in_place(*parent);
  collapse_one_child_split(arena, parent_id);
  return true;
}

bool equalize_layout_immediate_children(LayoutArena& arena, NodeId node_id) {
  auto* node = layout_node(arena, node_id);
  if (node == nullptr || !is_valid_layout_split(*node)) {
    return false;
  }

  const auto child_count = node->children.size();
  const std::vector<double> target_weights(
      child_count,
      1.0 / static_cast<double>(child_count));

  const auto current_weights = normalized_layout_child_weights(*node);
  bool changed = node->child_weights.size() != target_weights.size() ||
                 current_weights.size() != target_weights.size();
  for (std::size_t index = 0; index < current_weights.size() && index < target_weights.size();
       ++index) {
    changed = changed || std::abs(current_weights[index] - target_weights[index]) > 0.000001;
  }

  if (changed) {
    node->child_weights = target_weights;
  }
  return changed;
}

bool resize_layout_split_boundary(
    LayoutNode& node,
    const PaneSplitResizeTarget& target,
    int column,
    int row) {
  if (!is_valid_layout_split(node) || target.child_index + 1 >= node.children.size()) {
    return false;
  }

  auto weights = normalized_layout_child_weights(node);
  const double desired_boundary = resized_boundary_ratio(target, column, row);
  double fixed_before = 0.0;
  for (std::size_t index = 0; index < target.child_index; ++index) {
    fixed_before += weights[index];
  }

  const double pair_total = weights[target.child_index] + weights[target.child_index + 1];
  if (pair_total <= 0.000001) {
    return false;
  }

  const double min_child = std::min(0.05, pair_total / 2.0);
  const double pair_left = std::clamp(
      desired_boundary - fixed_before,
      min_child,
      std::max(min_child, pair_total - min_child));
  weights[target.child_index] = pair_left;
  weights[target.child_index + 1] = std::max(0.0, pair_total - pair_left);

  const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
  if (total <= 0.000001) {
    return false;
  }

  node.child_weights = std::move(weights);
  normalize_layout_child_weights_in_place(node);
  return true;
}

struct NodeIntegerRect {
  NodeId node_id{0};
  int left{0};
  int top{0};
  int width{0};
  int height{0};
};

void collect_node_integer_rects(
    const LayoutArena& arena,
    NodeId node_id,
    int left,
    int top,
    int width,
    int height,
    std::vector<NodeIntegerRect>& rects) {
  if (width <= 0 || height <= 0) {
    return;
  }

  const auto* node = layout_node(arena, node_id);
  if (node == nullptr) {
    return;
  }

  rects.push_back(NodeIntegerRect{node_id, left, top, width, height});
  if (node->kind != LayoutNode::Kind::Split || node->children.empty()) {
    return;
  }

  const auto extents = node->direction == SplitDirection::Horizontal
                           ? largest_remainder_extents(width, normalized_layout_child_weights(*node))
                           : largest_remainder_extents(height, normalized_layout_child_weights(*node));
  if (node->direction == SplitDirection::Horizontal) {
    int child_left = left;
    for (std::size_t index = 0; index < node->children.size(); ++index) {
      const int child_width = extents[index];
      collect_node_integer_rects(
          arena,
          node->children[index],
          child_left,
          top,
          child_width,
          height,
          rects);
      child_left += child_width;
    }
    return;
  }

  int child_top = top;
  for (std::size_t index = 0; index < node->children.size(); ++index) {
    const int child_height = extents[index];
    collect_node_integer_rects(
        arena,
        node->children[index],
        left,
        child_top,
        width,
        child_height,
        rects);
    child_top += child_height;
  }
}

std::optional<NodeIntegerRect> node_integer_rect(
    const LayoutArena& arena,
    NodeId node_id,
    int columns,
    int rows) {
  std::vector<NodeIntegerRect> rects;
  collect_node_integer_rects(arena, arena.root_id, 0, 0, columns, rows, rects);
  const auto found = std::find_if(rects.begin(), rects.end(), [&](const auto& rect) {
    return rect.node_id == node_id;
  });
  if (found == rects.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<PaneSplitResizeTarget> keyboard_resize_target(
    const LayoutArena& arena,
    PaneId active_pane_id,
    PaneDirection direction,
    std::uint16_t amount,
    int columns,
    int rows,
    int& column,
    int& row) {
  const auto leaf = arena.pane_leaf_index.find(active_pane_id);
  if (leaf == arena.pane_leaf_index.end() || columns <= 0 || rows <= 0) {
    return std::nullopt;
  }

  const bool horizontal =
      direction == PaneDirection::Left || direction == PaneDirection::Right;
  const auto wanted_split =
      horizontal ? SplitDirection::Horizontal : SplitDirection::Vertical;
  const int signed_amount =
      direction == PaneDirection::Left || direction == PaneDirection::Up
          ? -static_cast<int>(std::max<std::uint16_t>(amount, 1))
          : static_cast<int>(std::max<std::uint16_t>(amount, 1));

  NodeId child_id = leaf->second;
  auto* child = layout_node(arena, child_id);
  auto parent_id = child == nullptr ? std::optional<NodeId>{} : child->parent_id;
  while (parent_id) {
    const auto* parent = layout_node(arena, *parent_id);
    if (parent == nullptr || parent->kind != LayoutNode::Kind::Split) {
      return std::nullopt;
    }

    const auto child_index = child_index_for_node(*parent, child_id);
    if (!child_index) {
      return std::nullopt;
    }

    if (parent->direction == wanted_split) {
      std::optional<std::size_t> boundary_index;
      if (direction == PaneDirection::Left || direction == PaneDirection::Up) {
        boundary_index = *child_index > 0 ? std::optional<std::size_t>{*child_index - 1}
                                          : std::optional<std::size_t>{*child_index};
      } else {
        boundary_index = *child_index + 1 < parent->children.size()
                             ? std::optional<std::size_t>{*child_index}
                             : std::optional<std::size_t>{*child_index - 1};
      }

      if (!boundary_index || *boundary_index + 1 >= parent->children.size()) {
        return std::nullopt;
      }

      const auto rect = node_integer_rect(arena, parent->id, columns, rows);
      if (!rect) {
        return std::nullopt;
      }

      const auto extents = parent->direction == SplitDirection::Horizontal
                               ? largest_remainder_extents(
                                     rect->width,
                                     normalized_layout_child_weights(*parent))
                               : largest_remainder_extents(
                                     rect->height,
                                     normalized_layout_child_weights(*parent));
      int boundary = parent->direction == SplitDirection::Horizontal ? rect->left : rect->top;
      for (std::size_t index = 0; index <= *boundary_index && index < extents.size(); ++index) {
        boundary += extents[index];
      }
      boundary += signed_amount;

      column = parent->direction == SplitDirection::Horizontal ? boundary : rect->left;
      row = parent->direction == SplitDirection::Vertical ? boundary : rect->top;
      return PaneSplitResizeTarget{
          parent->id,
          {},
          *boundary_index,
          parent->direction,
          rect->left,
          rect->top,
          rect->width,
          rect->height};
    }

    child_id = parent->id;
    parent_id = parent->parent_id;
  }

  return std::nullopt;
}

bool is_valid_split(const PaneNode& node) {
  return node.kind == PaneNode::Kind::Split && node.children.size() >= 2;
}

std::vector<double> normalized_child_weights(const PaneNode& node) {
  std::vector<double> weights;
  weights.reserve(node.children.size());
  if (node.child_weights.size() == node.children.size()) {
    for (const auto weight : node.child_weights) {
      weights.push_back(std::max(0.0, weight));
    }
  } else {
    weights.assign(node.children.size(), 1.0);
  }

  const auto total = std::accumulate(weights.begin(), weights.end(), 0.0);
  if (total <= 0.000001) {
    weights.assign(node.children.size(), 1.0);
    return weights;
  }

  for (auto& weight : weights) {
    weight /= total;
  }
  return weights;
}

Window* active_window(Session& session, std::unordered_map<WindowId, Window>& windows) {
  if (session.active_window_id == 0) {
    return nullptr;
  }
  return window_by_id(windows, session.active_window_id);
}

const Window* active_window(
    const Session& session,
    const std::unordered_map<WindowId, Window>& windows) {
  if (session.active_window_id == 0) {
    return nullptr;
  }
  return window_by_id(windows, session.active_window_id);
}

void refresh_window_indices(Session& session, std::unordered_map<WindowId, Window>& windows) {
  for (std::size_t index = 0; index < session.windows.size(); ++index) {
    if (auto* window = window_by_id(windows, session.windows[index])) {
      window->index = index;
    }
  }
}

PaneSummary make_pane_summary(const Pane& pane) {
  return PaneSummary{
      pane.id,
      pane.window_id,
      pane.created_at};
}

WindowSummary make_window_summary(const Window& window) {
  WindowSummary summary;
  summary.id = window.id;
  summary.session_id = window.session_id;
  summary.index = window.index;
  summary.name = window.name;
  summary.active_pane_id = window.active_pane_id;
  summary.pane_tree = pane_tree_snapshot(window.layout, window.layout_root);
  summary.created_at = window.created_at;
  summary.panes.reserve(window.pane_order.size());
  for (const auto pane_id : window.pane_order) {
    const auto pane = window.panes.find(pane_id);
    if (pane != window.panes.end()) {
      summary.panes.push_back(make_pane_summary(pane->second));
    }
  }
  return summary;
}

SessionSummary make_session_summary(
    const Session& session,
    const std::unordered_map<WindowId, Window>& windows) {
  SessionSummary summary;
  summary.id = session.id;
  summary.name = session.name;
  summary.active_window_id = session.active_window_id;
  summary.created_at = session.created_at;
  summary.windows.reserve(session.windows.size());
  for (const auto window_id : session.windows) {
    const auto* window = window_by_id(windows, window_id);
    if (window != nullptr) {
      summary.windows.push_back(make_window_summary(*window));
    }
  }
  return summary;
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

  if (!is_valid_split(node)) {
    return;
  }

  const auto weights = normalized_child_weights(node);
  if (node.direction == SplitDirection::Horizontal) {
    double child_left = left;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const double child_right =
          index + 1 == node.children.size()
              ? right
              : child_left + ((right - left) * weights[index]);
      collect_pane_rects(node.children[index], child_left, top, child_right, bottom, rects);
      child_left = child_right;
    }
    return;
  }

  double child_top = top;
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    const double child_bottom =
        index + 1 == node.children.size()
            ? bottom
            : child_top + ((bottom - top) * weights[index]);
    collect_pane_rects(node.children[index], left, child_top, right, child_bottom, rects);
    child_top = child_bottom;
  }
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

std::optional<PaneId> pane_neighbor(
    const std::vector<PaneLayoutRect>& rects,
    PaneId active_pane_id,
    PaneDirection direction) {
  std::vector<NormalizedPaneRect> neighbor_rects;
  neighbor_rects.reserve(rects.size());
  for (const auto& rect : rects) {
    if (rect.width <= 0 || rect.height <= 0) {
      continue;
    }

    neighbor_rects.push_back(NormalizedPaneRect{
        rect.pane_id,
        static_cast<double>(rect.left),
        static_cast<double>(rect.top),
        static_cast<double>(rect.left + rect.width),
        static_cast<double>(rect.top + rect.height)});
  }

  return pane_neighbor(neighbor_rects, active_pane_id, direction);
}

std::vector<int> largest_remainder_extents(int extent, const std::vector<double>& weights) {
  std::vector<int> extents(weights.size(), 0);
  if (extent <= 0 || weights.empty()) {
    return extents;
  }

  std::vector<double> remainders(weights.size(), 0.0);
  int used = 0;
  for (std::size_t index = 0; index < weights.size(); ++index) {
    const double raw = static_cast<double>(extent) * std::max(0.0, weights[index]);
    extents[index] = static_cast<int>(std::floor(raw));
    remainders[index] = raw - static_cast<double>(extents[index]);
    used += extents[index];
  }

  while (used < extent) {
    const auto next = static_cast<std::size_t>(std::distance(
        remainders.begin(),
        std::max_element(remainders.begin(), remainders.end())));
    ++extents[next];
    remainders[next] = -1.0;
    ++used;
  }

  while (used > extent) {
    const auto largest = static_cast<std::size_t>(std::distance(
        extents.begin(),
        std::max_element(extents.begin(), extents.end())));
    if (extents[largest] == 0) {
      break;
    }
    --extents[largest];
    --used;
  }

  if (extent >= static_cast<int>(extents.size())) {
    for (std::size_t index = 0; index < extents.size(); ++index) {
      if (extents[index] > 0) {
        continue;
      }

      auto donor = static_cast<std::size_t>(std::distance(
          extents.begin(),
          std::max_element(extents.begin(), extents.end())));
      if (extents[donor] <= 1) {
        break;
      }
      --extents[donor];
      ++extents[index];
    }
  }

  return extents;
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

  if (!is_valid_split(node)) {
    return;
  }

  const auto extents = node.direction == SplitDirection::Horizontal
                           ? largest_remainder_extents(width, normalized_child_weights(node))
                           : largest_remainder_extents(height, normalized_child_weights(node));
  if (node.direction == SplitDirection::Horizontal) {
    int child_left = left;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const int child_width = extents[index];
      collect_integer_layout_rects(
          node.children[index], child_left, top, child_width, height, rects);
      child_left += child_width;
    }
    return;
  }

  int child_top = top;
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    const int child_height = extents[index];
    collect_integer_layout_rects(
        node.children[index], left, child_top, width, child_height, rects);
    child_top += child_height;
  }
}

bool in_rect(int column, int row, int left, int top, int width, int height) {
  return column >= left &&
         column < left + width &&
         row >= top &&
         row < top + height;
}

std::optional<PaneSplitResizeTarget> find_split_resize_target(
    const PaneNode& node,
    int left,
    int top,
    int width,
    int height,
    int column,
    int row,
    std::vector<PaneTreePathStep>& path) {
  if (width <= 1 || height <= 1 || !is_valid_split(node)) {
    return std::nullopt;
  }

  const auto extents = node.direction == SplitDirection::Horizontal
                           ? largest_remainder_extents(width, normalized_child_weights(node))
                           : largest_remainder_extents(height, normalized_child_weights(node));
  if (node.direction == SplitDirection::Horizontal) {
    int child_left = left;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
      const int child_width = extents[index];
      path.push_back(index);
      if (const auto target = find_split_resize_target(
              node.children[index],
              child_left,
              top,
              child_width,
              height,
              column,
              row,
              path)) {
        return target;
      }
      path.pop_back();
      child_left += child_width;
    }

    int split_column = left;
    for (std::size_t index = 0; index + 1 < extents.size(); ++index) {
      split_column += extents[index];
      if ((column == split_column || column == split_column - 1) &&
          row >= top &&
          row < top + height) {
        return PaneSplitResizeTarget{
            node.node_id,
            path,
            index,
            SplitDirection::Horizontal,
            left,
            top,
            width,
            height};
      }
    }
    return std::nullopt;
  }

  int child_top = top;
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    const int child_height = extents[index];
    path.push_back(index);
    if (const auto target = find_split_resize_target(
            node.children[index],
            left,
            child_top,
            width,
            child_height,
            column,
            row,
            path)) {
      return target;
    }
    path.pop_back();
    child_top += child_height;
  }

  int split_row = top;
  for (std::size_t index = 0; index + 1 < extents.size(); ++index) {
    split_row += extents[index];
    if ((row == split_row || row == split_row - 1) &&
        column >= left &&
        column < left + width) {
      return PaneSplitResizeTarget{
          node.node_id,
          path,
          index,
          SplitDirection::Vertical,
          left,
          top,
          width,
          height};
    }
  }

  return std::nullopt;
}

double resized_boundary_ratio(const PaneSplitResizeTarget& target, int column, int row) {
  constexpr double kMinSplitRatio = 0.05;
  constexpr double kMaxSplitRatio = 0.95;

  if (target.direction == SplitDirection::Horizontal) {
    if (target.width <= 1) {
      return 0.5;
    }
    return std::clamp(
        static_cast<double>(column - target.left) / static_cast<double>(target.width),
        kMinSplitRatio,
        kMaxSplitRatio);
  }

  if (target.height <= 1) {
    return 0.5;
  }
  return std::clamp(
      static_cast<double>(row - target.top) / static_cast<double>(target.height),
      kMinSplitRatio,
      kMaxSplitRatio);
}

}  // namespace

PaneNode::PaneNode(PaneId leaf_pane_id) : kind{Kind::Leaf}, pane_id{leaf_pane_id} {}

SessionOperationResult SessionManager::create_session(std::string name) {
  if (name.empty()) {
    return {false, SessionError::EmptyName};
  }

  if (name_index_.contains(name)) {
    return {false, SessionError::DuplicateName};
  }

  Session session;
  session.id = next_id_++;
  session.name = std::move(name);
  session.created_at = std::chrono::system_clock::now();

  Window initial_window;
  initial_window.id = next_window_id_++;
  initial_window.session_id = session.id;
  initial_window.index = 0;
  initial_window.name = "0";
  initial_window.created_at = session.created_at;
  Pane initial_pane;
  initial_pane.id = next_pane_id_++;
  initial_pane.window_id = initial_window.id;
  initial_pane.created_at = session.created_at;
  initial_window.active_pane_id = initial_pane.id;
  initialize_window_layout(initial_window, initial_pane.id);
  initial_window.pane_order.push_back(initial_pane.id);
  initial_window.panes.emplace(initial_pane.id, std::move(initial_pane));
  session.active_window_id = initial_window.id;
  session.windows.push_back(initial_window.id);

  const auto id = session.id;
  const auto window_id = session.active_window_id;
  const auto pane_id = initial_window.active_pane_id;
  name_index_.emplace(session.name, id);
  windows_.emplace(initial_window.id, std::move(initial_window));
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
    for (const auto window_id : session->second.windows) {
      windows_.erase(window_id);
    }
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

  if (contains_window_name(session->second, windows_, name)) {
    return {false, WindowError::DuplicateName, session_id};
  }

  Window window;
  window.id = next_window_id_++;
  window.session_id = session_id;
  window.index = session->second.windows.size();
  window.name = std::move(name);
  window.created_at = std::chrono::system_clock::now();
  Pane pane;
  pane.id = next_pane_id_++;
  pane.window_id = window.id;
  pane.created_at = window.created_at;
  window.active_pane_id = pane.id;
  initialize_window_layout(window, pane.id);
  window.pane_order.push_back(pane.id);
  window.panes.emplace(pane.id, std::move(pane));

  const auto window_id = window.id;
  const auto pane_id = window.active_pane_id;
  windows_.emplace(window_id, std::move(window));
  session->second.windows.push_back(window_id);
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
  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active->name != name && contains_window_name(session->second, windows_, name)) {
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

  auto active = std::find(
      session->second.windows.begin(),
      session->second.windows.end(),
      session->second.active_window_id);
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  ++active;
  if (active == session->second.windows.end()) {
    active = session->second.windows.begin();
  }
  auto* next_active = window_by_id(windows_, *active);
  if (next_active == nullptr) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  session->second.active_window_id = next_active->id;
  return {true, WindowError::None, session_id, next_active->id, next_active->active_pane_id};
}

WindowOperationResult SessionManager::select_previous_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto active = std::find(
      session->second.windows.begin(),
      session->second.windows.end(),
      session->second.active_window_id);
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (active == session->second.windows.begin()) {
    active = session->second.windows.end();
  }
  --active;
  auto* previous_active = window_by_id(windows_, *active);
  if (previous_active == nullptr) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  session->second.active_window_id = previous_active->id;
  return {
      true,
      WindowError::None,
      session_id,
      previous_active->id,
      previous_active->active_pane_id};
}

WindowOperationResult SessionManager::select_window(SessionId session_id, WindowId window_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  const auto found = std::find(
      session->second.windows.begin(),
      session->second.windows.end(),
      window_id);
  if (found == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  auto* window = window_by_id(windows_, window_id);
  if (window == nullptr) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  session->second.active_window_id = window->id;
  return {true, WindowError::None, session_id, window->id, window->active_pane_id};
}

WindowOperationResult SessionManager::kill_active_window(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_session(session_id);
  }

  if (session->second.windows.empty()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  if (session->second.windows.size() == 1) {
    return {false, WindowError::LastWindow, session_id, session->second.active_window_id};
  }

  auto active = std::find(
      session->second.windows.begin(),
      session->second.windows.end(),
      session->second.active_window_id);
  if (active == session->second.windows.end()) {
    return {false, WindowError::WindowNotFound, session_id};
  }

  const auto removed_window_id = *active;
  const auto removed_index =
      static_cast<std::size_t>(std::distance(session->second.windows.begin(), active));
  session->second.windows.erase(active);
  windows_.erase(removed_window_id);
  refresh_window_indices(session->second, windows_);
  const auto next_index = std::min(removed_index, session->second.windows.size() - 1);
  const auto next_window_id = session->second.windows[next_index];
  const auto* next_active = window_by_id(windows_, next_window_id);
  if (next_active == nullptr) {
    session->second.active_window_id = 0;
    return {false, WindowError::WindowNotFound, session_id};
  }

  session->second.active_window_id = next_active->id;
  return {
      true,
      WindowError::None,
      session_id,
      next_active->id,
      next_active->active_pane_id,
      removed_window_id};
}

PaneOperationResult SessionManager::split_active_pane(
    SessionId session_id,
    SplitDirection direction) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }
  if (!window.panes.contains(window.active_pane_id)) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }

  const PaneId new_pane_id = next_pane_id_++;
  if (!split_layout_leaf(window.layout, window.active_pane_id, new_pane_id, direction)) {
    --next_pane_id_;
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }
  window.layout_root = window.layout.root_id;

  Pane pane;
  pane.id = new_pane_id;
  pane.window_id = window.id;
  pane.created_at = std::chrono::system_clock::now();
  window.pane_order.push_back(new_pane_id);
  window.panes.emplace(new_pane_id, std::move(pane));
  window.active_pane_id = new_pane_id;

  return {true, PaneError::None, session_id, window.id, new_pane_id};
}

PaneOperationResult SessionManager::select_pane(SessionId session_id, PaneDirection direction) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  std::vector<NormalizedPaneRect> rects;
  rects.reserve(window.panes.size());
  const auto layout_snapshot = pane_tree_snapshot(window.layout, window.layout_root);
  collect_pane_rects(layout_snapshot, 0.0, 0.0, 1.0, 1.0, rects);

  const auto neighbor = pane_neighbor(rects, window.active_pane_id, direction);
  if (!neighbor) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  window.active_pane_id = *neighbor;
  return {true, PaneError::None, session_id, window.id, *neighbor};
}

PaneOperationResult SessionManager::select_pane(
    SessionId session_id,
    PaneDirection direction,
    int columns,
    int rows) {
  if (columns <= 0 || rows <= 0) {
    return select_pane(session_id, direction);
  }

  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  const auto layout_snapshot = pane_tree_snapshot(window.layout, window.layout_root);
  const auto rects = compute_pane_layout_rects(layout_snapshot, columns, rows);
  const auto neighbor = pane_neighbor(rects, window.active_pane_id, direction);
  if (!neighbor) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  window.active_pane_id = *neighbor;
  return {true, PaneError::None, session_id, window.id, *neighbor};
}

PaneOperationResult SessionManager::select_pane(SessionId session_id, PaneId pane_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  const auto pane = window.panes.find(pane_id);
  if (pane == window.panes.end()) {
    return {false, PaneError::PaneNotFound, session_id, window.id, pane_id};
  }

  window.active_pane_id = pane_id;
  return {true, PaneError::None, session_id, window.id, pane_id};
}

PaneOperationResult SessionManager::resize_active_window_split(
    SessionId session_id,
    const PaneSplitResizeTarget& target,
    int column,
    int row) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  auto* node = target.split_node_id != 0
                   ? layout_node(window.layout, target.split_node_id)
                   : layout_node_at_path(window.layout, window.layout_root, target.path);
  if (node == nullptr || !is_valid_layout_split(*node) || node->direction != target.direction) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }

  if (!resize_layout_split_boundary(*node, target, column, row)) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }
  return {true, PaneError::None, session_id, window.id, window.active_pane_id};
}

PaneOperationResult SessionManager::resize_active_pane(
    SessionId session_id,
    PaneDirection direction,
    std::uint16_t amount,
    int columns,
    int rows) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  int column = 0;
  int row = 0;
  const auto target = keyboard_resize_target(
      window.layout,
      window.active_pane_id,
      direction,
      amount,
      columns,
      rows,
      column,
      row);
  if (!target) {
    return {false, PaneError::NoSplit, session_id, window.id, window.active_pane_id};
  }

  auto* node = layout_node(window.layout, target->split_node_id);
  if (node == nullptr || !is_valid_layout_split(*node) || node->direction != target->direction) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }

  if (!resize_layout_split_boundary(*node, *target, column, row)) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }
  return {true, PaneError::None, session_id, window.id, window.active_pane_id, 0, true};
}

PaneOperationResult SessionManager::equalize_active_window_panes(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  const auto* root = layout_node(window.layout, window.layout_root);
  if (window.panes.size() <= 1 || root == nullptr || root->kind == LayoutNode::Kind::Leaf) {
    return {false, PaneError::NoSplit, session_id, window.id, window.active_pane_id};
  }

  const auto leaf = window.layout.pane_leaf_index.find(window.active_pane_id);
  if (leaf == window.layout.pane_leaf_index.end()) {
    return {false, PaneError::PaneNotFound, session_id, window.id, window.active_pane_id};
  }

  auto* active_leaf = layout_node(window.layout, leaf->second);
  if (active_leaf == nullptr || !active_leaf->parent_id) {
    return {false, PaneError::NoSplit, session_id, window.id, window.active_pane_id};
  }

  auto current = active_leaf->parent_id;
  while (current) {
    if (equalize_layout_immediate_children(window.layout, *current)) {
      return {true, PaneError::None, session_id, window.id, window.active_pane_id, 0, true};
    }

    const auto* current_node = layout_node(window.layout, *current);
    current = current_node == nullptr ? std::nullopt : current_node->parent_id;
  }

  return {true, PaneError::None, session_id, window.id, window.active_pane_id, 0, false};
}

PaneOperationResult SessionManager::kill_active_pane(SessionId session_id) {
  auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return missing_pane_session(session_id);
  }

  auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return {false, PaneError::WindowNotFound, session_id};
  }

  auto& window = *active;
  if (window.active_pane_id == 0) {
    return {false, PaneError::PaneNotFound, session_id, window.id};
  }

  if (window.panes.size() == 1) {
    return {false, PaneError::LastPane, session_id, window.id, window.active_pane_id};
  }

  const PaneId removed_pane_id = window.active_pane_id;
  PaneId replacement_pane_id = 0;
  if (!remove_layout_leaf_and_collapse(window.layout, removed_pane_id, replacement_pane_id)) {
    return {false, PaneError::PaneNotFound, session_id, window.id, removed_pane_id};
  }
  window.layout_root = window.layout.root_id;

  window.panes.erase(removed_pane_id);
  window.pane_order.erase(
      std::remove(window.pane_order.begin(), window.pane_order.end(), removed_pane_id),
      window.pane_order.end());
  window.active_pane_id = replacement_pane_id;

  return {
      true,
      PaneError::None,
      session_id,
      window.id,
      replacement_pane_id,
      removed_pane_id};
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

  const auto* active = active_window(session->second, windows_);
  if (active == nullptr || active->active_pane_id == 0 ||
      !active->panes.contains(active->active_pane_id)) {
    return std::nullopt;
  }

  return active->active_pane_id;
}

std::optional<WindowSummary> SessionManager::active_window_summary(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end() || session->second.active_window_id == 0) {
    return std::nullopt;
  }

  const auto* active = active_window(session->second, windows_);
  if (active == nullptr) {
    return std::nullopt;
  }

  return make_window_summary(*active);
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
      listed.push_back(make_session_summary(session->second, windows_));
    }
  }

  return listed;
}

std::vector<WindowSummary> SessionManager::list_windows(SessionId session_id) const {
  const auto session = sessions_.find(session_id);
  if (session == sessions_.end()) {
    return {};
  }

  std::vector<WindowSummary> listed;
  listed.reserve(session->second.windows.size());
  for (const auto window_id : session->second.windows) {
    const auto* window = window_by_id(windows_, window_id);
    if (window != nullptr) {
      listed.push_back(make_window_summary(*window));
    }
  }
  return listed;
}

std::vector<PaneLayoutRect> compute_pane_layout_rects(
    const PaneNode& root,
    int columns,
    int rows) {
  std::vector<PaneLayoutRect> rects;
  collect_integer_layout_rects(root, 0, 0, columns, rows, rects);
  return rects;
}

std::optional<PaneSplitResizeTarget> find_pane_split_resize_target(
    const PaneNode& root,
    int column,
    int row,
    int columns,
    int rows) {
  if (!in_rect(column, row, 0, 0, columns, rows)) {
    return std::nullopt;
  }

  std::vector<PaneTreePathStep> path;
  return find_split_resize_target(root, 0, 0, columns, rows, column, row, path);
}

}  // namespace wmux
