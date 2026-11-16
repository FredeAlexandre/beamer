//! `cargo xtask blind <dir>`: the workspace as a test agent may see it (#32, ADR 0002).
//!
//! The export holds the spec, the glossary, the role skills and every crate under `crates/`, with
//! each function body replaced by `todo!()`. It has no `.git`, so no history leads back to the
//! implementation. Tests in a package's `tests/` and its `build.rs` are copied as they are, and so
//! is `xtask/`: it is build tooling, not app code, and a test agent runs it in the export (#113).

use std::fs;
use std::ops::Range;
use std::path::Path;

use syn::spanned::Spanned;
use syn::visit::{self, Visit};

use crate::{Result, copy_tests, workspace};

/// What every blinded body becomes.
const TODO_BODY: &str = "{ todo!() }";

/// The lints a `todo!()` body sets off by itself: parameters, imports and helpers only the old
/// body used. The export allows them in its `.cargo/config.toml`, so they neither bury a test
/// agent's own warnings nor fail a build that denies warnings.
pub const BLIND_LINTS: [&str; 5] = [
    "dead_code",
    "unused_imports",
    "unused_mut",
    "unused_macros",
    "unused_variables",
];

pub fn run(dest: &Path) -> Result {
    let root = workspace::root()?;
    let kept = export(&root, dest)?;
    for note in &kept {
        eprintln!("warning: kept a body: {note}");
    }
    println!(
        "Blind export of {} written to {}.",
        root.display(),
        dest.display()
    );
    println!(
        "Bring its tests back with `cargo xtask copy-tests {}`.",
        dest.display()
    );
    Ok(())
}

/// Writes the blind export of the checkout at `root` into `dest`. Returns a note for every body
/// it had to keep, because `todo!()` cannot stand in for it.
pub fn export(root: &Path, dest: &Path) -> Result<Vec<String>> {
    if fs::read_dir(dest).is_ok_and(|mut entries| entries.next().is_some()) {
        return Err(format!("{} is not empty", dest.display()));
    }
    let mut kept = Vec::new();
    for rel in workspace::files(root)?.iter().filter(|rel| exported(rel)) {
        let from = root.join(rel);
        let to = dest.join(rel);
        fs::create_dir_all(to.parent().expect("a file has a parent"))
            .map_err(|e| format!("cannot create the directory of {}: {e}", to.display()))?;
        if blinded(root, rel) {
            let source =
                fs::read_to_string(&from).map_err(|e| format!("cannot read {rel}: {e}"))?;
            let (blinded, notes) = blind_source(&source).map_err(|e| format!("{rel}: {e}"))?;
            kept.extend(notes.into_iter().map(|note| format!("{rel}:{note}")));
            fs::write(&to, blinded)
        } else {
            fs::copy(&from, &to).map(drop)
        }
        .map_err(|e| format!("cannot write {}: {e}", to.display()))?;
        if copy_tests::returnable(rel) {
            let base = dest.join(copy_tests::BASE).join(rel);
            fs::create_dir_all(base.parent().expect("a file has a parent"))
                .and_then(|()| fs::copy(&from, &base))
                .map_err(|e| format!("cannot write {}: {e}", base.display()))?;
        }
    }
    allow_blind_lints(dest)?;
    Ok(kept)
}

/// Adds `-A <lint>` for each of [`BLIND_LINTS`] to `build.rustflags` in the export's
/// `.cargo/config.toml`, after any flags the workspace already sets there.
fn allow_blind_lints(dest: &Path) -> Result {
    let path = dest.join(".cargo/config.toml");
    let mut config: toml::Table = match fs::read_to_string(&path) {
        Ok(text) => text
            .parse()
            .map_err(|e| format!("{}: {e}", path.display()))?,
        Err(_) => toml::Table::new(),
    };
    let build = config
        .entry("build")
        .or_insert_with(|| toml::Table::new().into())
        .as_table_mut()
        .ok_or(".cargo/config.toml: `build` is not a table")?;
    let flags = match build.remove("rustflags") {
        None => Vec::new(),
        Some(toml::Value::Array(flags)) => flags,
        Some(toml::Value::String(flags)) => flags.split_whitespace().map(Into::into).collect(),
        Some(_) => return Err(".cargo/config.toml: `build.rustflags` is not a list".into()),
    };
    let allow = BLIND_LINTS
        .iter()
        .flat_map(|lint| ["-A", lint])
        .map(Into::into);
    build.insert(
        "rustflags".into(),
        flags.into_iter().chain(allow).collect::<Vec<_>>().into(),
    );
    let text = format!(
        "# Written by `cargo xtask blind`: the workspace's own config, plus the lints that\n\
         # todo!() bodies set off by themselves, allowed.\n{}",
        toml::to_string(&config).map_err(|e| e.to_string())?
    );
    fs::create_dir_all(path.parent().expect("has a parent")).map_err(|e| e.to_string())?;
    fs::write(&path, text).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

/// Whether a file of the checkout goes into the export: what a build needs, the spec, the
/// glossary, and the build guide with its role skills. Everything else (other docs, assets,
/// native code) stays out.
fn exported(rel: &str) -> bool {
    let (first, rest) = rel.split_once('/').unwrap_or((rel, ""));
    match first {
        "crates" | "xtask" | "spec" | ".cargo" | ".config" | ".claude" => true,
        "Cargo.lock" | "GLOSSARY.md" | "CLAUDE.md" => true,
        _ => rest.is_empty() && first.ends_with(".toml"),
    }
}

/// Whether a file's function bodies become `todo!()`: a Rust file of a crate under `crates/`,
/// except that crate's `tests/` and `build.rs`. `xtask/` and the rest are copied as they are.
fn blinded(root: &Path, rel: &str) -> bool {
    rel.starts_with("crates/") && rel.ends_with(".rs") && !verbatim(root, rel)
}

/// Whether a Rust file is copied as it is: a package's `tests/` and its `build.rs`.
fn verbatim(root: &Path, rel: &str) -> bool {
    let parts: Vec<&str> = rel.split('/').collect();
    // The file's package is the nearest directory above it that holds a Cargo.toml.
    let package = (0..parts.len()).rev().find(|&depth| {
        let dir: std::path::PathBuf = parts[..depth].iter().collect();
        root.join(dir).join("Cargo.toml").is_file()
    });
    package.is_some_and(|depth| matches!(&parts[depth..], ["build.rs"] | ["tests", ..]))
}

/// `source` with every function body replaced by `todo!()` and every `#[cfg(test)]` item
/// removed. Returns the new source and a note (`line: fn name (reason)`) for each body kept.
pub fn blind_source(source: &str) -> Result<(String, Vec<String>)> {
    // Not `syn::parse_file`: it strips a byte order mark or a shebang, which shifts every span.
    let source = source.strip_prefix('\u{feff}').unwrap_or(source);
    let file: syn::File = syn::parse_str(source).map_err(|e| format!("does not parse: {e}"))?;
    let mut blinder = Blinder::default();
    blinder.visit_file(&file);
    let mut blinded = source.to_owned();
    blinder
        .edits
        .sort_by_key(|(range, _)| std::cmp::Reverse(range.start));
    for (range, replacement) in blinder.edits {
        blinded.replace_range(range, replacement);
    }
    Ok((blinded, blinder.kept))
}

#[derive(Default)]
struct Blinder {
    edits: Vec<(Range<usize>, &'static str)>,
    kept: Vec<String>,
}

impl Blinder {
    fn body(&mut self, sig: &syn::Signature, block: &syn::Block) {
        let reason = if sig.constness.is_some() {
            Some("a const fn may be evaluated while compiling")
        } else if returns_impl_trait(&sig.output) {
            Some("todo!() cannot stand in for an `impl Trait` return type")
        } else {
            None
        };
        match reason {
            Some(reason) => self.kept.push(format!(
                "{}: fn {} ({reason})",
                sig.ident.span().start().line,
                sig.ident
            )),
            None => self
                .edits
                .push((block.brace_token.span.join().byte_range(), TODO_BODY)),
        }
    }

    /// Removes an item, attributes and doc comments included.
    fn remove(&mut self, item: &impl Spanned) {
        self.edits.push((item.span().byte_range(), ""));
    }
}

impl<'ast> Visit<'ast> for Blinder {
    fn visit_item(&mut self, item: &'ast syn::Item) {
        if cfg_test(item_attrs(item)) {
            self.remove(item);
        } else {
            visit::visit_item(self, item);
        }
    }

    fn visit_impl_item(&mut self, item: &'ast syn::ImplItem) {
        if cfg_test(impl_item_attrs(item)) {
            self.remove(item);
        } else {
            visit::visit_impl_item(self, item);
        }
    }

    // The three kinds of function body. None recurses: whatever the body held is gone.
    fn visit_item_fn(&mut self, item: &'ast syn::ItemFn) {
        self.body(&item.sig, &item.block);
    }

    fn visit_impl_item_fn(&mut self, item: &'ast syn::ImplItemFn) {
        self.body(&item.sig, &item.block);
    }

    fn visit_trait_item_fn(&mut self, item: &'ast syn::TraitItemFn) {
        if let Some(block) = &item.default {
            self.body(&item.sig, block);
        }
    }
}

fn returns_impl_trait(output: &syn::ReturnType) -> bool {
    struct Finder(bool);
    impl<'ast> Visit<'ast> for Finder {
        fn visit_type_impl_trait(&mut self, _: &'ast syn::TypeImplTrait) {
            self.0 = true;
        }
    }
    let mut finder = Finder(false);
    finder.visit_return_type(output);
    finder.0
}

/// Whether the attributes hold exactly `#[cfg(test)]`: the implementer's unit tests, which are
/// implementation and never part of the export.
fn cfg_test(attrs: &[syn::Attribute]) -> bool {
    attrs.iter().any(|attr| {
        attr.path().is_ident("cfg")
            && attr
                .parse_args::<syn::Ident>()
                .is_ok_and(|ident| ident == "test")
    })
}

fn item_attrs(item: &syn::Item) -> &[syn::Attribute] {
    use syn::Item;
    match item {
        Item::Const(i) => &i.attrs,
        Item::Enum(i) => &i.attrs,
        Item::ExternCrate(i) => &i.attrs,
        Item::Fn(i) => &i.attrs,
        Item::ForeignMod(i) => &i.attrs,
        Item::Impl(i) => &i.attrs,
        Item::Macro(i) => &i.attrs,
        Item::Mod(i) => &i.attrs,
        Item::Static(i) => &i.attrs,
        Item::Struct(i) => &i.attrs,
        Item::Trait(i) => &i.attrs,
        Item::TraitAlias(i) => &i.attrs,
        Item::Type(i) => &i.attrs,
        Item::Union(i) => &i.attrs,
        Item::Use(i) => &i.attrs,
        _ => &[],
    }
}

fn impl_item_attrs(item: &syn::ImplItem) -> &[syn::Attribute] {
    use syn::ImplItem;
    match item {
        ImplItem::Const(i) => &i.attrs,
        ImplItem::Fn(i) => &i.attrs,
        ImplItem::Macro(i) => &i.attrs,
        ImplItem::Type(i) => &i.attrs,
        _ => &[],
    }
}
