//! What a crate's Rust source says, parsed with `syn`: comments and strings never count. Code
//! inside a macro's body isn't parsed, so it isn't seen.

use std::fs;
use std::path::{Path, PathBuf};
use syn::visit::Visit;

/// A parsed `.rs` file and its path relative to the crate.
pub struct SourceFile {
    pub path: String,
    pub syntax: syn::File,
}

/// Every `.rs` file under `dir` (its `target/` and hidden directories aside), parsed. A file that
/// doesn't parse is a problem, since its rules can't be checked.
pub fn parse_all(crate_name: &str, dir: &Path, problems: &mut Vec<String>) -> Vec<SourceFile> {
    let mut paths = Vec::new();
    collect(dir, &mut paths);
    paths.sort();
    let mut files = Vec::new();
    for path in paths {
        let relative = path
            .strip_prefix(dir)
            .unwrap_or(&path)
            .to_string_lossy()
            .replace('\\', "/");
        let parsed = fs::read_to_string(&path)
            .map_err(|e| e.to_string())
            .and_then(|text| syn::parse_file(&text).map_err(|e| e.to_string()));
        match parsed {
            Ok(syntax) => files.push(SourceFile {
                path: relative,
                syntax,
            }),
            Err(error) => problems.push(format!("{crate_name}: {relative} doesn't parse: {error}")),
        }
    }
    files
}

fn collect(dir: &Path, paths: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if path.is_dir() {
            if name != "target" && !name.starts_with('.') {
                collect(&path, paths);
            }
        } else if name.ends_with(".rs") {
            paths.push(path);
        }
    }
}

/// Every foreign ABI the file names (`extern "C"` blocks, functions and function pointer types),
/// as the ABI string and the line it is on. A bare `extern` is `"C"`.
pub fn extern_abis(file: &syn::File) -> Vec<(String, usize)> {
    struct Abis(Vec<(String, usize)>);
    impl Visit<'_> for Abis {
        fn visit_abi(&mut self, abi: &syn::Abi) {
            let name = abi.name.as_ref().map_or("C".to_owned(), syn::LitStr::value);
            if name != "Rust" {
                self.0.push((name, abi.extern_token.span.start().line));
            }
        }
    }
    let mut abis = Abis(Vec::new());
    abis.visit_file(file);
    abis.0
}

/// A branch or loop in an `-ffi` crate that does more than map an error: the construct, the
/// function holding it and its line.
pub struct Branch {
    pub construct: &'static str,
    pub function: String,
    pub line: usize,
}

/// Every branch and loop in `file` beyond error mapping. Error mapping is `?`, and an `if`,
/// `match` or `let … else` whose every branch is just `Ok(…)`, `Err(…)`, `Some(…)` or `None`, or
/// a `return` of one. A `match` guard is a branch of its own, so a guarded `match` never maps
/// errors only. Loops never do. Items under `#[cfg(test)]` aren't shims and are skipped.
pub fn branches(file: &syn::File) -> Vec<Branch> {
    let mut finder = Branches {
        functions: Vec::new(),
        owner: None,
        found: Vec::new(),
    };
    finder.visit_file(file);
    finder.found
}

struct Branches {
    /// The functions being visited, innermost last.
    functions: Vec<String>,
    /// The type or trait whose `impl` or definition is being visited.
    owner: Option<String>,
    found: Vec<Branch>,
}

impl Branches {
    fn flag(&mut self, construct: &'static str, span: proc_macro2::Span) {
        let function = self
            .functions
            .last()
            .cloned()
            .unwrap_or_else(|| "a static or const".into());
        self.found.push(Branch {
            construct,
            function,
            line: span.start().line,
        });
    }

    fn in_function(&mut self, name: &syn::Ident, visit: impl FnOnce(&mut Self)) {
        let name = match &self.owner {
            Some(owner) => format!("{owner}::{name}"),
            None => name.to_string(),
        };
        self.functions.push(name);
        visit(self);
        self.functions.pop();
    }

    fn with_owner(&mut self, owner: String, visit: impl FnOnce(&mut Self)) {
        let outer = self.owner.replace(owner);
        visit(self);
        self.owner = outer;
    }
}

impl<'a> Visit<'a> for Branches {
    fn visit_item(&mut self, item: &'a syn::Item) {
        let attrs = match item {
            syn::Item::Fn(item) => &item.attrs,
            syn::Item::Mod(item) => &item.attrs,
            syn::Item::Impl(item) => &item.attrs,
            syn::Item::Trait(item) => &item.attrs,
            _ => return syn::visit::visit_item(self, item),
        };
        if !attrs.iter().any(is_cfg_test) {
            syn::visit::visit_item(self, item);
        }
    }

    fn visit_item_fn(&mut self, item: &'a syn::ItemFn) {
        self.in_function(&item.sig.ident, |me| syn::visit::visit_item_fn(me, item));
    }

    fn visit_impl_item_fn(&mut self, item: &'a syn::ImplItemFn) {
        self.in_function(&item.sig.ident, |me| {
            syn::visit::visit_impl_item_fn(me, item)
        });
    }

    fn visit_trait_item_fn(&mut self, item: &'a syn::TraitItemFn) {
        self.in_function(&item.sig.ident, |me| {
            syn::visit::visit_trait_item_fn(me, item)
        });
    }

    fn visit_item_impl(&mut self, item: &'a syn::ItemImpl) {
        let owner = match &*item.self_ty {
            syn::Type::Path(path) => path.path.segments.last().map(|s| s.ident.to_string()),
            _ => None,
        };
        self.with_owner(owner.unwrap_or_else(|| "impl".into()), |me| {
            syn::visit::visit_item_impl(me, item)
        });
    }

    fn visit_item_trait(&mut self, item: &'a syn::ItemTrait) {
        self.with_owner(item.ident.to_string(), |me| {
            syn::visit::visit_item_trait(me, item)
        });
    }

    fn visit_expr(&mut self, expr: &'a syn::Expr) {
        match expr {
            syn::Expr::ForLoop(e) => self.flag("for", e.for_token.span),
            syn::Expr::While(e) => self.flag("while", e.while_token.span),
            syn::Expr::Loop(e) => self.flag("loop", e.loop_token.span),
            syn::Expr::If(e) if !maps_errors_if(e) => self.flag("if", e.if_token.span),
            syn::Expr::Match(e) if !maps_errors_match(e) => self.flag("match", e.match_token.span),
            _ => {}
        }
        syn::visit::visit_expr(self, expr);
    }

    fn visit_local(&mut self, local: &'a syn::Local) {
        if let Some(init) = &local.init
            && let Some((_, diverge)) = &init.diverge
            && !maps_errors(diverge)
        {
            self.flag("let else", local.let_token.span);
        }
        syn::visit::visit_local(self, local);
    }
}

fn is_cfg_test(attr: &syn::Attribute) -> bool {
    attr.path().is_ident("cfg")
        && attr
            .parse_args::<syn::Ident>()
            .is_ok_and(|ident| ident == "test")
}

/// The `if` maps errors when its own branches do. An `else if` is judged on its own.
fn maps_errors_if(e: &syn::ExprIf) -> bool {
    maps_errors_block(&e.then_branch)
        && e.else_branch
            .as_ref()
            .is_none_or(|(_, other)| matches!(&**other, syn::Expr::If(_)) || maps_errors(other))
}

fn maps_errors_match(e: &syn::ExprMatch) -> bool {
    e.arms
        .iter()
        .all(|arm| arm.guard.is_none() && maps_errors(&arm.body))
}

fn maps_errors_block(block: &syn::Block) -> bool {
    matches!(block.stmts.as_slice(), [syn::Stmt::Expr(e, _)] if maps_errors(e))
}

/// `Ok(…)`, `Err(…)`, `Some(…)`, `None`, a `return` of one, or a block holding only that.
fn maps_errors(expr: &syn::Expr) -> bool {
    let named = |path: &syn::Path, names: &[&str]| {
        path.segments
            .last()
            .is_some_and(|s| names.iter().any(|n| s.ident == n))
    };
    match expr {
        syn::Expr::Call(call) => {
            matches!(&*call.func, syn::Expr::Path(p) if named(&p.path, &["Ok", "Err", "Some"]))
        }
        syn::Expr::Path(p) => named(&p.path, &["None"]),
        syn::Expr::Return(r) => r.expr.as_deref().is_some_and(maps_errors),
        syn::Expr::Paren(p) => maps_errors(&p.expr),
        syn::Expr::Block(b) => maps_errors_block(&b.block),
        _ => false,
    }
}
