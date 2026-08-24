use crate::PaneId;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SplitDirection {
    LeftRight,
    TopBottom,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ResizeDirection {
    Left,
    Right,
    Up,
    Down,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Rect {
    pub x: u16,
    pub y: u16,
    pub cols: u16,
    pub rows: u16,
}

impl Rect {
    pub const fn new(x: u16, y: u16, cols: u16, rows: u16) -> Self {
        Self { x, y, cols, rows }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum LayoutNode {
    Leaf(PaneId),
    Split {
        direction: SplitDirection,
        weights: Vec<u16>,
        children: Vec<LayoutNode>,
    },
}

impl LayoutNode {
    pub const fn leaf(pane: PaneId) -> Self {
        Self::Leaf(pane)
    }

    pub fn split_leaf(
        &mut self,
        target: PaneId,
        new_pane: PaneId,
        direction: SplitDirection,
    ) -> bool {
        match self {
            Self::Leaf(pane) if *pane == target => {
                *self = Self::Split {
                    direction,
                    weights: vec![1, 1],
                    children: vec![Self::Leaf(target), Self::Leaf(new_pane)],
                };
                true
            }
            Self::Leaf(_) => false,
            Self::Split {
                direction: existing,
                weights,
                children,
            } => {
                for index in 0..children.len() {
                    if children[index].is_leaf(target) && *existing == direction {
                        children.insert(index + 1, Self::Leaf(new_pane));
                        weights.insert(index + 1, weights.get(index).copied().unwrap_or(1));
                        return true;
                    }
                    if children[index].split_leaf(target, new_pane, direction) {
                        return true;
                    }
                }
                false
            }
        }
    }

    pub fn remove_leaf(&mut self, target: PaneId) -> bool {
        match self {
            Self::Leaf(_) => false,
            Self::Split {
                children, weights, ..
            } => {
                let mut removed = false;
                let mut index = 0;
                children.retain(|child| {
                    let keep = !child.is_leaf(target);
                    if !keep && index < weights.len() {
                        weights.remove(index);
                    }
                    if keep {
                        index += 1;
                    }
                    removed |= !keep;
                    keep
                });
                if !removed {
                    for child in children.iter_mut() {
                        if child.remove_leaf(target) {
                            removed = true;
                            break;
                        }
                    }
                }
                if removed {
                    self.simplify();
                }
                removed
            }
        }
    }

    pub fn swap_panes(&mut self, first: PaneId, second: PaneId) -> bool {
        if first == second {
            return true;
        }
        let mut seen_first = false;
        let mut seen_second = false;
        self.map_leaves(&mut |pane| {
            if *pane == first {
                *pane = second;
                seen_first = true;
            } else if *pane == second {
                *pane = first;
                seen_second = true;
            }
        });
        seen_first && seen_second
    }

    pub fn rotate(&mut self, reverse: bool) -> bool {
        let mut panes = self.leaves();
        if panes.len() < 2 {
            return false;
        }
        if reverse {
            panes.rotate_left(1);
        } else {
            panes.rotate_right(1);
        }
        let mut iter = panes.into_iter();
        self.map_leaves(&mut |pane| {
            if let Some(next) = iter.next() {
                *pane = next;
            }
        });
        true
    }

    pub fn resize_leaf(
        &mut self,
        target: PaneId,
        split: SplitDirection,
        direction: ResizeDirection,
        amount: u16,
    ) -> bool {
        match self {
            Self::Leaf(_) => false,
            Self::Split {
                direction: current,
                weights,
                children,
            } => {
                if *current == split {
                    if let Some(index) = children
                        .iter()
                        .position(|child| child.leaves().contains(&target))
                    {
                        let neighbor = match direction {
                            ResizeDirection::Left | ResizeDirection::Up => index.checked_sub(1),
                            ResizeDirection::Right | ResizeDirection::Down => {
                                (index + 1 < children.len()).then_some(index + 1)
                            }
                        };
                        if let Some(neighbor) = neighbor {
                            let grow =
                                matches!(direction, ResizeDirection::Right | ResizeDirection::Down);
                            let amount = amount.max(1);
                            let source = if grow { neighbor } else { index };
                            let dest = if grow { index } else { neighbor };
                            if weights.get(source).copied().unwrap_or(1) > amount {
                                weights[source] = weights[source].saturating_sub(amount);
                                weights[dest] = weights[dest].saturating_add(amount);
                                return true;
                            }
                        }
                    }
                }
                children
                    .iter_mut()
                    .any(|child| child.resize_leaf(target, split, direction, amount))
            }
        }
    }

    pub fn leaves(&self) -> Vec<PaneId> {
        let mut out = Vec::new();
        self.collect_leaves(&mut out);
        out
    }

    pub fn rects(&self, rect: Rect) -> Vec<(PaneId, Rect)> {
        let mut out = Vec::new();
        self.collect_rects(rect, &mut out);
        out
    }

    pub fn borders(&self, rect: Rect) -> Vec<(u16, u16, char)> {
        let mut out = Vec::new();
        self.collect_borders(rect, &mut out);
        out
    }

    fn is_leaf(&self, target: PaneId) -> bool {
        matches!(self, Self::Leaf(pane) if *pane == target)
    }

    fn simplify(&mut self) {
        if let Self::Split { children, .. } = self {
            for child in children.iter_mut() {
                child.simplify();
            }
            if children.len() == 1 {
                *self = children.remove(0);
            }
        }
    }

    fn collect_leaves(&self, out: &mut Vec<PaneId>) {
        match self {
            Self::Leaf(pane) => out.push(*pane),
            Self::Split { children, .. } => {
                for child in children {
                    child.collect_leaves(out);
                }
            }
        }
    }

    fn map_leaves(&mut self, f: &mut impl FnMut(&mut PaneId)) {
        match self {
            Self::Leaf(pane) => f(pane),
            Self::Split { children, .. } => {
                for child in children {
                    child.map_leaves(f);
                }
            }
        }
    }

    fn collect_rects(&self, rect: Rect, out: &mut Vec<(PaneId, Rect)>) {
        match self {
            Self::Leaf(pane) => out.push((*pane, rect)),
            Self::Split {
                direction,
                weights,
                children,
            } => {
                if children.is_empty() {
                    return;
                }
                let rects = split_rect(rect, *direction, children.len(), weights);
                for (child, rect) in children.iter().zip(rects) {
                    child.collect_rects(rect, out);
                }
            }
        }
    }

    fn collect_borders(&self, rect: Rect, out: &mut Vec<(u16, u16, char)>) {
        match self {
            Self::Leaf(_) => {}
            Self::Split {
                direction,
                weights,
                children,
            } => {
                if children.len() < 2 {
                    return;
                }
                let rects = split_rect(rect, *direction, children.len(), weights);
                for (child, child_rect) in children.iter().zip(rects.iter().copied()) {
                    child.collect_borders(child_rect, out);
                }
                match direction {
                    SplitDirection::LeftRight => {
                        for child in rects.iter().take(rects.len() - 1) {
                            let x = child.x.saturating_add(child.cols);
                            for y in rect.y..rect.y.saturating_add(rect.rows) {
                                out.push((x, y, '|'));
                            }
                        }
                    }
                    SplitDirection::TopBottom => {
                        for child in rects.iter().take(rects.len() - 1) {
                            let y = child.y.saturating_add(child.rows);
                            for x in rect.x..rect.x.saturating_add(rect.cols) {
                                out.push((x, y, '-'));
                            }
                        }
                    }
                }
            }
        }
    }
}

fn split_rect(rect: Rect, direction: SplitDirection, count: usize, weights: &[u16]) -> Vec<Rect> {
    let count = count.max(1);
    let weights = normalized_weights(count, weights);
    let total_weight = weights.iter().copied().map(u32::from).sum::<u32>().max(1);
    let separators = count.saturating_sub(1) as u16;
    match direction {
        SplitDirection::LeftRight => {
            let available = rect.cols.saturating_sub(separators).max(count as u16);
            let mut x = rect.x;
            (0..count)
                .map(|index| {
                    let mut cols =
                        ((u32::from(available) * u32::from(weights[index])) / total_weight) as u16;
                    if index + 1 == count {
                        cols = rect.x.saturating_add(rect.cols).saturating_sub(x);
                    }
                    cols = cols.max(1).min(available);
                    let child = Rect::new(x, rect.y, cols.max(1), rect.rows.max(1));
                    x = x.saturating_add(cols).saturating_add(1);
                    child
                })
                .collect()
        }
        SplitDirection::TopBottom => {
            let available = rect.rows.saturating_sub(separators).max(count as u16);
            let mut y = rect.y;
            (0..count)
                .map(|index| {
                    let mut rows =
                        ((u32::from(available) * u32::from(weights[index])) / total_weight) as u16;
                    if index + 1 == count {
                        rows = rect.y.saturating_add(rect.rows).saturating_sub(y);
                    }
                    rows = rows.max(1).min(available);
                    let child = Rect::new(rect.x, y, rect.cols.max(1), rows.max(1));
                    y = y.saturating_add(rows).saturating_add(1);
                    child
                })
                .collect()
        }
    }
}

fn normalized_weights(count: usize, weights: &[u16]) -> Vec<u16> {
    (0..count)
        .map(|index| weights.get(index).copied().unwrap_or(1).max(1))
        .collect()
}
