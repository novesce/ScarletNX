#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""ASD-STE100 checker for the Claude files (CLAUDE.md, .claude/, the memory files) and for agent messages.

It checks the rules a program can check: sentence length, paragraph length, -ing forms, the passive voice, the
perfect tenses, contractions, semicolons and the words in words.txt. It cannot check the dictionary: the writer
uses the approved words and meanings (SKILL.md). A code span, a link target, a URL and a number count as one word.

  check.py FILE...      the rule breaks in each file: exit 0 when there are none, 1 when there are, 2 on a path
                        it cannot read
  check.py --all        the same for every Claude file of the repository
  check.py --text       the rule breaks in the text on stdin
  check.py --hook       a Claude Code hook (PreToolUse on Edit, Write, MultiEdit and Agent, and SubagentStop):
                        the hook's JSON on stdin
"""

import difflib
import glob
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

MAX_PROCEDURE = 20  # words in an instruction
MAX_DESCRIPTION = 25  # words in a description
MAX_PARAGRAPH = 6  # sentences in a paragraph
MAX_REPORT_REWRITES = 3  # times a subagent must write its report again

# Words that end in -ing and are not verb forms
NOT_VERBS = set(
    'anything bring brings ceiling during everything king meaning meanings nothing ping ring rings sibling siblings '
    'something spring '
    'sting string strings swing thing things wing wings'.split()
)
# Adverbs that can come between an auxiliary verb and its participle
ADVERBS = set('not never only also always then now still already often usually just all both first again ever'.split())
# Past participles that do not end in -ed
IRREGULAR = set(
    'begun bound brought built bought caught chosen cut done drawn driven eaten fallen fed felt forgotten found '
    'frozen given gone got gotten grown held hidden hit hung kept known laid led left lent let lost made meant met '
    'paid put read ridden risen run said seen sent set shown shut sold sought spent split spun stolen stood struck '
    'stuck sworn taken taught thought thrown told torn understood woken won worn wound written withdrawn'.split()
)
# Words that end in -ed and are not past participles
NOT_PARTICIPLES = set(
    'bed bleed breed embed exceed feed greed hundred indeed need proceed red seed shed speed succeed '
    'sacred naked wicked'.split()
)
BE = set('am is are was were be been being'.split())
HAVE = set('has have had'.split())
CONTRACTION = re.compile(r"\b(\w+n['’]t|\w+['’](re|ve|ll|d|m)|(it|that|there|here|what|who|let)['’]s)\b", re.I)
# The first words of an instruction: a sentence that starts with one has the shorter limit
IMPERATIVE = set(
    'add ask attach avoid build bump call check close commit compare copy count declare delete delegate describe do '
    'find fix give go grep install keep kill leave let list load look make mark measure move name never open pass '
    'pick prefer press push put read record remove replace report run say see send set show spawn stage start stop '
    'take tell test use verify wait write zero always'.split()
)


def load_pairs(name):
    """words.txt: a word or phrase that is not approved, a tab, and what to use instead"""
    pairs = []
    path = os.path.join(HERE, name)
    if not os.path.exists(path):
        return pairs
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            word, _, use = line.partition('\t')
            pattern = r'(?<![\w.])' + re.escape(word.strip()) + (r'(?!\w)' if word.strip()[-1].isalnum() else '')
            pairs.append((re.compile(pattern, re.I), word.strip(), use.strip()))
    return pairs


def load_terms():
    """terms.txt: technical names that end in -ing (STE lets technical names keep their form), one a line. A name of
    more than one word (false sharing) counts as one word, and only the whole name lets its -ing word through"""
    path = os.path.join(HERE, 'terms.txt')
    terms = set()
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.split('#', 1)[0].strip().lower()
                if line:
                    terms.add(line)
    return terms


WORDS = load_pairs('words.txt')
TERMS = load_terms()
PHRASES = [re.compile(r'\b' + r'[\s-]+'.join(map(re.escape, t.split())) + r'\b', re.I) for t in TERMS if ' ' in t]


def plain(text):
    """The text with code spans, links, URLs and emphasis replaced: a code span or URL becomes the token CODE"""
    text = re.sub(r'`[^`]*`', ' CODE ', text)
    text = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'\[\[([^\]]*)\]\]', ' CODE ', text)
    text = re.sub(r'<[^>\s]+>', ' CODE ', text)
    text = re.sub(r'\b(https?://|www\.)\S+', ' CODE ', text)
    text = re.sub(r'(?<![\w/~.])[~.]?/[\w./~*-]+', ' CODE ', text)  # paths
    text = re.sub(r'\b[\w-]+\.(md|py|c|h|txt|json|sh|cmake|ini|log|so|comp|glsl|swipc|svc|def)\b', ' CODE ', text)
    text = re.sub(r'(\*\*|__|\*)', '', text)
    for phrase in PHRASES:
        text = phrase.sub(' TERM ', text)
    return text


def sentences(text):
    """The sentences of a block of plain text"""
    parts = re.split(r'(?<=[.!?])["\')]*\s+(?=["\'(]?[A-Z0-9])', text.strip())
    return [p for p in parts if re.search(r'[A-Za-z]', p)]


def tokens(sentence):
    return re.findall(r"CODE|[A-Za-z][A-Za-z'’]*(?:-[A-Za-z0-9'’]+)*|\d[\w.,:/%+-]*", sentence)


def participle(word):
    w = word.lower()
    if w in IRREGULAR:
        return True
    return len(w) > 3 and w.endswith('ed') and w not in NOT_PARTICIPLES


def check_sentence(s, report):
    words = tokens(s)
    if not words:
        return
    first = words[0].lower()
    if first == 'do' and len(words) > 1 and words[1].lower() == 'not':
        limit = MAX_PROCEDURE
    else:
        limit = MAX_PROCEDURE if first in IMPERATIVE else MAX_DESCRIPTION
    if len(words) > limit:
        kind = 'an instruction' if limit == MAX_PROCEDURE else 'a description'
        report('length', f'{len(words)} words, {limit} at most in {kind}: "{short(s)}"')
    lower = [w.lower() for w in words]
    check_words(s, report)
    for i, w in enumerate(lower):
        if w in BE or w in HAVE:
            j = i + 1
            while j < len(lower) and lower[j] in ADVERBS:
                j += 1
            if j < len(lower) and words[j] != 'CODE' and participle(lower[j]):
                phrase = ' '.join(words[i:j + 1])
                if w in BE:
                    report('passive', f'"{phrase}": use the active voice (say who or what does it)')
                else:
                    report('tense', f'"{phrase}": use the simple present, past or future, not a perfect tense')


def check_words(s, report):
    """The rules for single words: -ing forms and contractions"""
    for w in tokens(s):
        if w in ('CODE', 'TERM'):
            continue
        for part in w.lower().split('-'):
            if (len(part) > 4 and part.endswith('ing') and part not in NOT_VERBS and part not in TERMS
                    and not part.endswith(('string', 'thing'))):
                report('ing', f'"{w}": use a verb form or a noun, not the -ing form (or add a technical name to terms.txt)')
                break
    for m in CONTRACTION.finditer(s):
        report('contraction', f'"{m.group(0)}": write the words in full')


def short(s, n=70):
    s = ' '.join(s.split())
    return s if len(s) <= n else s[:n - 3] + '...'


def blocks(text):
    """The prose of a Markdown text as (first line, last line, kind, text): paragraphs, list items, table cells,
    headings and the description in front matter. Code blocks, HTML comments and other front matter are left out."""
    out = []
    lines = text.split('\n')
    i, n = 0, len(lines)
    if n and lines[0].strip() == '---':
        i = 1
        while i < n and lines[i].strip() != '---':
            m = re.match(r'(description|summary)\s*:\s*(.*)', lines[i])
            if m and m.group(2).strip() not in ('', '|', '>'):
                # metadata that picks the skill or agent: the sentence rules, not the paragraph limit
                out.append((i + 1, i + 1, 'cell', m.group(2).strip().strip('"\'')))
            i += 1
        i += 1
    cur, start = [], 0

    def flush(end):
        if cur:
            out.append((start, end, 'para', ' '.join(cur)))
            cur.clear()

    fence = None
    comment = False
    while i < n:
        line = lines[i]
        s = line.strip()
        if fence:
            if s.startswith(fence):
                fence = None
            i += 1
            continue
        if comment:
            if '-->' in s:
                comment = False
            i += 1
            continue
        m = re.match(r'(```+|~~~+)', s)
        if m:
            flush(i)
            fence = m.group(1)
            i += 1
            continue
        if s.startswith('<!--'):
            flush(i)
            comment = '-->' not in s
            i += 1
            continue
        if not s:
            flush(i)
            i += 1
            continue
        if s.startswith('#'):
            flush(i)
            out.append((i + 1, i + 1, 'heading', s.lstrip('#').strip()))
            i += 1
            continue
        if s.startswith('|'):
            flush(i)
            if not re.fullmatch(r'\|?[\s:|-]+\|?', s):
                for cell in s.strip('|').split('|'):
                    if cell.strip():
                        out.append((i + 1, i + 1, 'cell', cell.strip()))
            i += 1
            continue
        if re.match(r'([-*+]|\d+[.)])\s+', s):
            flush(i)
            start = i + 1
            cur.append(re.sub(r'^([-*+]|\d+[.)])\s+(\[[ xX]\]\s+)?', '', s))
            i += 1
            continue
        if not cur:
            start = i + 1
        cur.append(s)
        i += 1
    flush(n)
    return out


def check_text(text, markdown=True):
    """The rule breaks of a text as (line, rule, message)"""
    found = []
    items = blocks(text) if markdown else [(1, 1, 'para', p) for p in re.split(r'\n\s*\n', text)]
    for first, last, kind, raw in items:
        p = plain(raw)

        def report(rule, msg, line=first):
            found.append((line, last, rule, msg))

        if ';' in p:
            report('semicolon', 'use two sentences or a list, not a semicolon')
        for pattern, word, use in WORDS:
            if pattern.search(p):
                report('word', f'"{word}" is not approved: {use}')
        if kind == 'heading':
            check_words(p, report)
            continue
        ss = sentences(p)
        if kind == 'para' and len(ss) > MAX_PARAGRAPH:
            report('paragraph', f'{len(ss)} sentences, {MAX_PARAGRAPH} at most in a paragraph')
        for s in ss:
            check_sentence(s, report)
    return found


def check_file(path, changed=None):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    return [r for r in check_text(text) if changed is None or any(r[0] <= c <= r[1] for c in changed)]


def in_scope(path):
    """A Claude file: CLAUDE.md, Markdown under .claude/ (not other checkouts in .claude/worktrees), or a memory file"""
    p = os.path.abspath(path).replace('\\', '/')
    if not p.endswith('.md'):
        return False
    if '/.claude/worktrees/' in p or '/ste100/test/' in p:
        return False
    if os.path.basename(p) == 'CLAUDE.md':
        return True
    if re.search(r'/\.claude/projects/[^/]+/memory/', p):
        return True
    return '/.claude/' in p


def repo_files():
    files = [os.path.join(ROOT, 'CLAUDE.md')]
    files += sorted(glob.glob(os.path.join(ROOT, '.claude', '**', '*.md'), recursive=True))
    return [f for f in files if os.path.exists(f) and in_scope(f)]


def format_found(found, label, limit=40):
    lines = [f'{label}:{line}: {rule}: {msg}' for line, _, rule, msg in found]
    if len(lines) > limit:
        lines = lines[:limit] + [f'... and {len(lines) - limit} more']
    return '\n'.join(lines)


GUIDE = ('Claude files and agent messages follow ASD-STE100 (the ste100 skill). An instruction has 20 words at most, '
         'and a description has 25 words at most. A paragraph has 6 sentences at most. Use the active voice, simple '
         'tenses and approved words. Do not use -ing forms, contractions or semicolons.')


def edited_text(tool, ti, path):
    """The file's text after the edit, and the numbers of the lines it changes (None: every line)"""
    old = None
    if os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            old = f.read()
    if tool == 'Write':
        new = ti.get('content', '')
    else:
        if old is None:
            return None, None
        new = old
        edits = ti.get('edits') if tool == 'MultiEdit' else [ti]
        for e in edits or []:
            a, b = e.get('old_string', ''), e.get('new_string', '')
            if a and a in new:
                new = new.replace(a, b) if e.get('replace_all') else new.replace(a, b, 1)
    if old is None:
        return new, None
    changed = set()
    sm = difflib.SequenceMatcher(None, old.split('\n'), new.split('\n'), autojunk=False)
    for tag, _, _, j1, j2 in sm.get_opcodes():
        if tag in ('replace', 'insert'):
            changed.update(range(j1 + 1, j2 + 1))
    return new, changed


def hook():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return 0
    event = data.get('hook_event_name', '')
    if event == 'PreToolUse':
        tool = data.get('tool_name', '')
        ti = data.get('tool_input') or {}
        if tool in ('Agent', 'Task'):
            found = check_text(ti.get('prompt', ''))
            if found:
                print(f'{GUIDE} Write the prompt again in STE:\n{format_found(found, "prompt")}', file=sys.stderr)
                return 2
            return 0
        if tool in ('Edit', 'Write', 'MultiEdit'):
            path = ti.get('file_path', '')
            if not path or not in_scope(path):
                return 0
            new, changed = edited_text(tool, ti, path)
            if new is None:
                return 0
            found = [r for r in check_text(new) if changed is None or any(r[0] <= c <= r[1] for c in changed)]
            if found:
                print(f'{GUIDE} Write the changed text again in STE:\n{format_found(found, path)}', file=sys.stderr)
                return 2
        return 0
    if event == 'SubagentStop':
        found = check_text(data.get('last_assistant_message') or '')
        if not found:
            return 0
        key = re.sub(r'\W', '_', f"{data.get('session_id', '')}_{data.get('agent_id', '')}")
        state = os.path.join(tempfile.gettempdir(), 'snx-ste100', key)
        os.makedirs(os.path.dirname(state), exist_ok=True)
        count = 0
        if os.path.exists(state):
            with open(state) as f:
                count = int(f.read() or 0)
        if count >= MAX_REPORT_REWRITES:
            return 0
        with open(state, 'w') as f:
            f.write(str(count + 1))
        reason = f'{GUIDE} Write your final report again in STE, with the same facts:\n{format_found(found, "report")}'
        print(json.dumps({'decision': 'block', 'reason': reason}))
        return 0
    return 0


def main(argv):
    if argv[1:2] == ['--hook']:
        return hook()
    if argv[1:2] == ['--text']:
        found = check_text(sys.stdin.read())
        if found:
            print(format_found(found, 'text', limit=10**9))
        return 1 if found else 0
    paths = repo_files() if argv[1:2] == ['--all'] else argv[1:]
    status = 0
    for path in paths:
        try:
            found = check_file(path)
        except OSError as e:
            print(f'{path}: {e.strerror}', file=sys.stderr)
            return 2
        if found:
            print(format_found(found, os.path.relpath(path), limit=10**9))
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv))
