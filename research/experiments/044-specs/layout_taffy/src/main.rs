//! Research 140: the known implementation box layout (spec 8) is checked against. Each line of stdin is a tree of
//! flex containers, a JSON object a node: `dir` ("row", "column"), `w`, `h` (a length, or null: auto), `grow`, `shrink`,
//! `basis` (a length, or null: auto), `pad` ([left, right, top, bottom]), `gap`, `justify` ("start", "end", "center",
//! "space-between", "space-around", "space-evenly"), `align` ("start", "end", "center", "stretch") and `kids`. Taffy lays
//! it out (flexbox, no wrap, no rounding) in the root's size; each line of stdout is the nodes' boxes in preorder,
//! [x, y, w, h], each relative to its parent's. Min sizes are 0, which 9.7 floors at an item's padding (its content box
//! at zero); Taffy, given 0, floors at 0 while it resolves lengths (its padding only bounds the size it ends with), so
//! it is given the padding.
//!
//! `layout_taffy --time ROUNDS TREES` times Taffy instead, on the trees layout_check.py writes (each a u32 count, its
//! nodes' rows of 14 binary64s in preorder, as the app takes them, and their boxes): each tree built and laid out, its
//! boxes the file's within 0.01 first, then the median of seven runs of ROUNDS passes. One JSON line.
use serde_json::Value;
use std::io::{self, BufRead, Write};
use std::time::Instant;
use taffy::prelude::*;

fn length(v: &Value) -> Dimension {
    v.as_f64().map_or(Dimension::auto(), |x| Dimension::length(x as f32))
}

fn node(tree: &mut TaffyTree<()>, v: &Value) -> NodeId {
    let kids: Vec<NodeId> = v["kids"].as_array().map_or(vec![], |k| k.iter().map(|c| node(tree, c)).collect());
    let pad: Vec<f32> = v["pad"].as_array().map_or(vec![0.0; 4], |p| p.iter().map(|x| x.as_f64().unwrap() as f32).collect());
    let gap = v["gap"].as_f64().unwrap_or(0.0) as f32;
    let style = Style {
        display: Display::Flex,
        flex_direction: if v["dir"] == "column" { FlexDirection::Column } else { FlexDirection::Row },
        flex_wrap: FlexWrap::NoWrap,
        size: Size { width: length(&v["w"]), height: length(&v["h"]) },
        min_size: Size {
            width: LengthPercentageAuto::length(pad[0] + pad[1]),
            height: LengthPercentageAuto::length(pad[2] + pad[3]),
        },
        flex_grow: v["grow"].as_f64().unwrap_or(0.0) as f32,
        flex_shrink: v["shrink"].as_f64().unwrap_or(0.0) as f32,
        flex_basis: length(&v["basis"]),
        padding: Rect {
            left: LengthPercentage::length(pad[0]),
            right: LengthPercentage::length(pad[1]),
            top: LengthPercentage::length(pad[2]),
            bottom: LengthPercentage::length(pad[3]),
        },
        gap: Size { width: LengthPercentage::length(gap), height: LengthPercentage::length(gap) },
        justify_content: match v["justify"].as_str().unwrap_or("start") {
            "end" => JustifyContent::END,
            "center" => JustifyContent::CENTER,
            "space-between" => JustifyContent::SPACE_BETWEEN,
            "space-around" => JustifyContent::SPACE_AROUND,
            "space-evenly" => JustifyContent::SPACE_EVENLY,
            _ => JustifyContent::START,
        },
        align_items: match v["align"].as_str().unwrap_or("stretch") {
            "start" => AlignItems::START,
            "end" => AlignItems::END,
            "center" => AlignItems::CENTER,
            _ => AlignItems::STRETCH,
        },
        ..Default::default()
    };
    tree.new_with_children(style, &kids).unwrap()
}

fn boxes(tree: &TaffyTree<()>, n: NodeId, out: &mut Vec<[f32; 4]>) {
    let l = tree.layout(n).unwrap();
    out.push([l.location.x, l.location.y, l.size.width, l.size.height]);
    for c in tree.children(n).unwrap() {
        boxes(tree, c, out);
    }
}

/// A row's node: parent, column, w, h (-1: auto), grow, shrink, basis, left, right, top, bottom, gap, justify, align.
fn style_of(r: &[f64]) -> Style {
    let length = |x: f64| if x < 0.0 { Dimension::auto() } else { Dimension::length(x as f32) };
    let (l, rt, t, b) = (r[7] as f32, r[8] as f32, r[9] as f32, r[10] as f32);
    Style {
        display: Display::Flex,
        flex_direction: if r[1] != 0.0 { FlexDirection::Column } else { FlexDirection::Row },
        size: Size { width: length(r[2]), height: length(r[3]) },
        min_size: Size { width: LengthPercentageAuto::length(l + rt), height: LengthPercentageAuto::length(t + b) },
        flex_grow: r[4] as f32,
        flex_shrink: r[5] as f32,
        flex_basis: if r[0] < 0.0 { Dimension::auto() } else { length(r[6]) },
        padding: Rect {
            left: LengthPercentage::length(l),
            right: LengthPercentage::length(rt),
            top: LengthPercentage::length(t),
            bottom: LengthPercentage::length(b),
        },
        gap: Size { width: LengthPercentage::length(r[11] as f32), height: LengthPercentage::length(r[11] as f32) },
        justify_content: [JustifyContent::START, JustifyContent::END, JustifyContent::CENTER, JustifyContent::SPACE_BETWEEN,
            JustifyContent::SPACE_AROUND, JustifyContent::SPACE_EVENLY][r[12] as usize],
        align_items: [AlignItems::START, AlignItems::END, AlignItems::CENTER, AlignItems::STRETCH][r[13] as usize],
        ..Default::default()
    }
}

/// A tree of rows built and laid out in its root's size: its nodes, in the rows' order.
fn lay(size: Size<f32>, styles: &[(i64, Style)]) -> (TaffyTree<()>, Vec<NodeId>) {
    let mut tree: TaffyTree<()> = TaffyTree::new();
    tree.disable_rounding();
    let ids: Vec<NodeId> = styles.iter().map(|(_, s)| tree.new_leaf(s.clone()).unwrap()).collect();
    for (k, (parent, _)) in styles.iter().enumerate().skip(1) {
        tree.add_child(ids[*parent as usize], ids[k]).unwrap();
    }
    let space = Size { width: AvailableSpace::Definite(size.width), height: AvailableSpace::Definite(size.height) };
    tree.compute_layout(ids[0], space).unwrap();
    (tree, ids)
}

fn time(rounds: usize, path: &str) {
    let data = std::fs::read(path).unwrap();
    let number = |at: usize| f64::from_le_bytes(data[at..at + 8].try_into().unwrap());
    let (mut trees, mut wants, mut at, mut nodes) = (vec![], vec![], 0, 0);
    while at < data.len() {
        let n = u32::from_le_bytes(data[at..at + 4].try_into().unwrap()) as usize;
        let rows: Vec<Vec<f64>> = (0..n).map(|k| (0..14).map(|f| number(at + 4 + 112 * k + 8 * f)).collect()).collect();
        let boxes: Vec<f64> = (0..4 * n).map(|f| number(at + 4 + 112 * n + 8 * f)).collect();
        let size = Size { width: rows[0][2] as f32, height: rows[0][3] as f32 };
        trees.push((size, rows.iter().map(|r| (r[0] as i64, style_of(r))).collect::<Vec<_>>()));
        wants.push(boxes);
        at += 4 + 112 * n + 32 * n;
        nodes += n;
    }
    for ((size, styles), want) in trees.iter().zip(&wants) {
        let (tree, ids) = lay(*size, styles);
        for (k, id) in ids.iter().enumerate() {
            let l = tree.layout(*id).unwrap();
            let got = [l.location.x, l.location.y, l.size.width, l.size.height];
            assert!((0..4).all(|f| (got[f] as f64 - want[4 * k + f]).abs() <= 0.01), "a box not the reference's");
        }
    }
    let mut runs = vec![];
    for _ in 0..7 {
        let start = Instant::now();
        for _ in 0..rounds {
            for (size, styles) in &trees {
                std::hint::black_box(lay(*size, styles));
            }
        }
        runs.push(start.elapsed().as_nanos() as f64 / rounds as f64);
    }
    runs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    println!("{{\"trees\":{},\"nodes\":{},\"taffy_ns_per_node\":{:.1}}}", trees.len(), nodes, runs[3] / nodes as f64);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() == 4 && args[1] == "--time" {
        return time(args[2].parse().unwrap(), &args[3]);
    }
    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());
    for line in io::stdin().lock().lines() {
        let v: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let mut tree: TaffyTree<()> = TaffyTree::new();
        tree.disable_rounding();
        let root = node(&mut tree, &v);
        let space = Size {
            width: AvailableSpace::Definite(v["w"].as_f64().unwrap() as f32),
            height: AvailableSpace::Definite(v["h"].as_f64().unwrap() as f32),
        };
        tree.compute_layout(root, space).unwrap();
        let mut b = vec![];
        boxes(&tree, root, &mut b);
        writeln!(out, "{}", serde_json::to_string(&b).unwrap()).unwrap();
    }
}
