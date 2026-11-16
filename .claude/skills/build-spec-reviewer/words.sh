#!/usr/bin/env bash
# Lists every word GLOSSARY.md says to avoid that a PR adds to spec/ or crates/, with the term the
# glossary uses instead. Each hit is a candidate: the reviewer judges whether it names the
# glossary's concept. Rust identifiers are split at `_` and at case changes, so `HotEdge` and
# `hot_edge` both read as "hot edge". Avoid words that are themselves glossary terms (input,
# output, driver) are skipped. Read-only.
#
#   .claude/skills/build-spec-reviewer/words.sh              # the diff from origin/rust to HEAD
#   .claude/skills/build-spec-reviewer/words.sh <base-ref>
set -euo pipefail

base=${1:-origin/rust}
root=$(git rev-parse --show-toplevel)

git -C "$root" diff --no-color -U0 "$base"...HEAD -- spec crates | perl -e '
  use strict; use warnings;
  my $glossary = shift;
  open my $g, "<", $glossary or die "no $glossary\n";
  my (%avoid, %term, $current);
  while (<$g>) {
    if (/^\*\*(.+?)\*\*:/) {
      $current = $1;
      $term{lc $_} = 1 for split m{\s*/\s*}, $current;
    } elsif (/^_Avoid_:\s*(.+)$/ && defined $current) {
      for my $word (split /\s*,\s*/, $1) {
        $word =~ s/\s*\(.*?\)\s*//g;
        push @{$avoid{lc $word}}, $current if length $word;
      }
    }
  }
  delete $avoid{$_} for keys %term;
  my @words = sort { length $b <=> length $a } keys %avoid;

  my ($file, $line) = ("", 0);
  while (<STDIN>) {
    if (/^\+\+\+ b\/(.+)$/) { $file = $1; next }
    if (/^@@ -\S+ \+(\d+)/) { $line = $1; next }
    next unless /^\+(.*)$/;
    my $text = $1;
    (my $plain = $text) =~ s/([a-z0-9])([A-Z])/$1 $2/g;
    $plain =~ s/_/ /g;
    $plain = lc $plain;
    for my $word (@words) {
      my $pattern = join q{[\s-]+}, map { quotemeta } split /[\s-]+/, $word;
      if ($plain =~ /\b$pattern(?:s|es)?\b/) {
        (my $shown = $text) =~ s/^\s+//;
        my $terms = join q{ or }, @{$avoid{$word}};
        print "$file:$line: \"$word\" (glossary: $terms): $shown\n";
      }
    }
    $line++;
  }
' "$root/GLOSSARY.md"
