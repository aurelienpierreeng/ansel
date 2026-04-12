#!/usr/bin/env python3

import sys
from pathlib import Path
from clang.cindex import Index, CursorKind, TypeKind, Config

import regex as re

# Config.set_library_file("/usr/lib/llvm-17/lib/libclang.so")

EXTS = {".c", ".h", ".cpp", ".hpp"}
NULL_NAME = "NULL"

def rewrite_condition(cond: str) -> str:
    """
    Conservative rule:
    - if token is pointer-like: convert
    - if already negated: flip logic
    """

    token_re = re.compile(r'(!?)([A-Za-z_]\w*(?:->\w*)?(?:\[[^\]]+\])?)')
    return token_re.sub(_rewrite_token_repl, cond)

# -------------------------
# extract condition text safely
# -------------------------

def get_text(src, extent):
    return src[extent.start.offset:extent.end.offset].decode("utf-8", "replace")

# -------------------------
# process file
# -------------------------

def find_decl_in_cursor(cursor, name, visited=None):
    if visited is None:
        visited = set()
    # build a stable key for the cursor to avoid infinite recursion: (kind, start, end, spelling)
    key = (cursor.kind, getattr(cursor.extent, 'start', None) and cursor.extent.start.offset,
            getattr(cursor.extent, 'end', None) and cursor.extent.end.offset, cursor.spelling)

    if key in visited:
        return None
    visited.add(key)

    for ch in cursor.get_children():
        if ch.spelling == name and ch.kind in (CursorKind.PARM_DECL, CursorKind.VAR_DECL):
            return ch
        res = find_decl_in_cursor(ch, name, visited)
        if res:
            return res
    return None


def is_pointer_decl(decl):
    ty = decl.type
    if ty.kind == TypeKind.POINTER:
        return True
    # check canonical type in case of typedefs
    get_canon = getattr(ty, 'get_canonical', None)
    if get_canon:
        canon = ty.get_canonical()
        if canon.kind == TypeKind.POINTER:
            return True

    return False


def collect_decls(root):
    """Collect PARAM_DECL and VAR_DECL under root into a name->cursor dict."""
    res = {}
    stack = [root]
    while stack:
        node = stack.pop()
        if node.kind in (CursorKind.PARM_DECL, CursorKind.VAR_DECL):
            if node.spelling:
                res[node.spelling] = node

        for ch in node.get_children():
            stack.append(ch)

    return res


def collect_decl_refs(n, call_refs):
    if n.kind == CursorKind.DECL_REF_EXPR:
        ref = getattr(n, 'referenced', None)
        name = ref.spelling if ref else n.spelling
        if name:
            call_refs.add(name)
    for ch in n.get_children():
        collect_decl_refs(ch, call_refs)


def collect_calls(n, call_refs):
    if n.kind == CursorKind.CALL_EXPR:
        collect_decl_refs(n, call_refs)
    else:
        for ch in n.get_children():
            collect_calls(ch, call_refs)
            
            
def collect_call_extents(n, call_extents):
    if n.kind == CursorKind.CALL_EXPR:
        call_extents.append((n.extent.start.offset, n.extent.end.offset))

    for ch in n.get_children():
        collect_call_extents(ch, call_extents)
        
        
def collect_call_args(n, call_args_names):
    if n.kind == CursorKind.CALL_EXPR:
        collect_decl_refs(n, call_args_names)

    for ch in n.get_children():
        collect_call_args(ch, call_args_names)


# cache for global declarations per translation unit
_global_decls_cache = {}

def node_text(src_bytes, node):
    start = max(0, node.extent.start.offset)
    end = max(start, node.extent.end.offset)
    return src_bytes[start:end].decode("utf-8", "replace")


def _pos_in_string(s: str, pos: int) -> bool:
    """Return True if position `pos` is inside a single- or double-quoted literal in s."""
    in_double = False
    in_single = False
    i = 0
    while i < pos and i < len(s):
        ch = s[i]
        if ch == '\\':
            i += 2
            continue
        if not in_single and ch == '"':
            in_double = not in_double
        elif not in_double and ch == "'":
            in_single = not in_single
        i += 1
    return in_double or in_single


def find_literal_ranges(text: str):
    ranges = []
    i = 0
    L = len(text)
    while i < L:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            start = i
            i += 1
            while i < L:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            end = i
            ranges.append((start, end))
        else:
            i += 1
    return ranges


def is_pos_in_literal(pos: int, lit_ranges) -> bool:
    for a, b in lit_ranges:
        if pos >= a and pos < b:
            return True
    return False


def find_call_arg_ranges(text: str, lit_ranges):
    ranges = []
    for m in re.finditer(r'\b[A-Za-z_]\w*\s*\(', text):
        open_pos = m.end() - 1
        i = open_pos + 1
        depth = 0
        L = len(text)
        while i < L:
            if is_pos_in_literal(i, lit_ranges):
                # find literal range that contains i
                for a, b in lit_ranges:
                    if i >= a and i < b:
                        i = b
                        break
                continue
            if text[i] == '(':
                depth += 1
            elif text[i] == ')':
                if depth == 0:
                    ranges.append((open_pos + 1, i))
                    break
                depth -= 1
            i += 1
    return ranges


def is_pos_in_call_args(pos: int, call_ranges) -> bool:
    for a, b in call_ranges:
        if pos >= a and pos < b:
            return True
    return False

class ReplEq:
    def __init__(self, var, lit_ranges, call_ranges):
        self.var = var
        self.lit_ranges = lit_ranges
        self.call_ranges = call_ranges

    def __call__(self, m):
        if is_pos_in_literal(m.start(), self.lit_ranges) or is_pos_in_call_args(m.start(), self.call_ranges):
            return m.group(0)
        return f'IS_NULL_PTR({self.var})'


class ReplNeq:
    def __init__(self, var, lit_ranges, call_ranges):
        self.var = var
        self.lit_ranges = lit_ranges
        self.call_ranges = call_ranges

    def __call__(self, m):
        if is_pos_in_literal(m.start(), self.lit_ranges) or is_pos_in_call_args(m.start(), self.call_ranges):
            return m.group(0)
        return f'!IS_NULL_PTR({self.var})'


class ReplNot:
    def __init__(self, lit_ranges, call_ranges):
        self.lit_ranges = lit_ranges
        self.call_ranges = call_ranges

    def __call__(self, m):
        if is_pos_in_literal(m.start(), self.lit_ranges) or is_pos_in_call_args(m.start(), self.call_ranges):
            return m.group(0)
        return f'IS_NULL_PTR({m.group(1)})'


class ReplNotVar:
    def __init__(self, var, lit_ranges, call_ranges):
        self.var = var
        self.lit_ranges = lit_ranges
        self.call_ranges = call_ranges

    def __call__(self, m):
        if is_pos_in_literal(m.start(), self.lit_ranges) or is_pos_in_call_args(m.start(), self.call_ranges):
            return m.group(0)
        return f'IS_NULL_PTR({self.var})'


def transform_condition(cond_text: str, pointer_vars: list) -> str:
    s = cond_text
    # literal and call-arg ranges are computed by top-level helpers
    lit_ranges = find_literal_ranges(s)
    call_ranges = find_call_arg_ranges(s, lit_ranges)

    # first, normalize spacing for reliable regex matching
    # handle comparisons var == NULL and var != NULL (and NULL == var)
    for var in pointer_vars:
        # var == NULL -> IS_NULL_PTR(var)
        s = re.sub(r'\b' + re.escape(var) + r"\s*==\s*NULL\b", ReplEq(var, lit_ranges, call_ranges), s)
        s = re.sub(r'\bNULL\s*==\s*' + re.escape(var) + r"\b", ReplEq(var, lit_ranges, call_ranges), s)
        # var != NULL -> !IS_NULL_PTR(var)
        s = re.sub(r'\b' + re.escape(var) + r"\s*!=\s*NULL\b", ReplNeq(var, lit_ranges, call_ranges), s)
        s = re.sub(r'\bNULL\s*!=\s*' + re.escape(var) + r"\b", ReplNeq(var, lit_ranges, call_ranges), s)

    # handle explicit negation !var -> IS_NULL_PTR(var) (avoid matching '!=')
    # explicit negation patterns: only apply to pointer variables
    for var in pointer_vars:
        # avoid matching negation of function calls: do not match if identifier is followed by '('
        pat = r'(?<![=])!\s*' + re.escape(var) + r'\b(?!\s*(?:->|\.|\[|\())'
        s = re.sub(pat, ReplNotVar(var, lit_ranges, call_ranges), s)

    # replace bare occurrences of var (not part of comparisons or member access) with !IS_NULL_PTR(var)
    # We do this by scanning matches and checking surrounding characters
    for var in pointer_vars:
        out = []
        last = 0
        for m in re.finditer(r'\b' + re.escape(var) + r'\b', s):
            start, end = m.start(), m.end()
            # skip if inside string/char literal or inside call argument list
            if is_pos_in_literal(start, lit_ranges) or is_pos_in_call_args(start, call_ranges):
                continue
            # skip if match lies inside a string or char literal
            if _pos_in_string(s, start):
                continue
            # check surrounding context
            before = s[max(0, start - 10):start]
            after = s[end:end + 10]
            # skip if member access like '->' or '.' following or preceding (allowing spaces)
            if re.search(r'(->|\.)\s*$', before) or re.match(r'\s*(->|\.|\[)', after):
                continue
            # skip if unary '!' immediately precedes the identifier (allowing spaces): '! ptr' or '!ptr'
            if re.search(r'!\s*$', before):
                continue
            # skip if already transformed (e.g., IS_NULL_PTR(var) or !IS_NULL_PTR(var))
            prefix = s[max(0, start - 16):start]
            if 'IS_NULL_PTR' in prefix or '!IS_NULL_PTR' in prefix:
                continue
            # skip if part of comparisons (==, !=, <=, >=, <, >) with non-NULL rhs
            if re.match(r"\s*(?:==|!=|<=|>=|<|>)", s[end:]):
                mright = re.match(r"\s*(?:==|!=|<=|>=|<|>)\s*NULL\b", s[end:])
                if not mright:
                    continue
            if re.search(r"(?:==|!=|<=|>=|<|>)\s*$", s[max(0, start - 10):start]):
                mleft = re.search(r"\bNULL\s*(?:==|!=|<=|>=|<|>)\s*$", s[max(0, start - 30):start])
                if not mleft:
                    continue
            # skip dereferenced identifiers (e.g. '*var') — do not transform the pointee
            if re.search(r'\*\s*$', before):
                continue
            out.append((start, end))
        if not out:
            continue
        # build new s with replacements from end to start to not disturb indices
        parts = []
        last = 0
        for (start, end) in out:
            parts.append(s[last:start])
            parts.append(f'!IS_NULL_PTR({var})')
            last = end
        parts.append(s[last:])
        s = ''.join(parts)

    return s


def collect_ast_replacements(cond_node, src_bytes):
    """Walk condition AST and produce precise replacements as (start,end,bytes,old,new).
    Replacements handled:
    - `var == NULL` and `NULL == var` -> `IS_NULL_PTR(var)`
    - `var != NULL` and `NULL != var` -> `!IS_NULL_PTR(var)`
    - `!var` where var is a bare DeclRefExpr (not member access) -> `IS_NULL_PTR(var)`
    - bare `var` (DeclRefExpr) used as boolean -> `!IS_NULL_PTR(var)`
    The function avoids touching identifiers that are part of CALL_EXPR, MEMBER_REF_EXPR,
    ARRAY_SUBSCRIPT_EXPR, or binary comparisons not involving NULL.
    """
    tu = cond_node.translation_unit
    edits = []

    _collect_ast_walk(cond_node, None, tu, src_bytes, edits)
    # deduplicate overlapping edits by preferring larger spans (sort by start)
    # We'll return raw edits; caller will sort and apply
    return edits


def collect_call_args_tokens(cond_node):
    """Collect identifier names that appear as arguments of any call-like token sequence
    inside cond_node by tokenizing the source via libclang. This handles macros
    and other call-like constructs that do not appear as CALL_EXPR in the AST.
    """
    tu = cond_node.translation_unit
    args = set()
    tokens = list(tu.get_tokens(extent=cond_node.extent))

    i = 0
    # tokens: inspect sequences IDENTIFIER '(' ... ')'
    while i < len(tokens) - 1:
        t = tokens[i]
        nxt = tokens[i + 1]
        # token.kind may not be TokenKind enum accessible here; compare name when available
        kind_name = getattr(t.kind, 'name', None)
        if kind_name == 'IDENTIFIER' and getattr(nxt, 'spelling', '') == '(':
            # parse until matching ')'
            depth = 0
            j = i + 1
            # find start of inner tokens
            j += 1
            content_idents = []
            while j < len(tokens):
                s = tokens[j].spelling
                if s == '(':
                    depth += 1
                elif s == ')':
                    if depth == 0:
                        break
                    depth -= 1
                else:
                    # collect identifier tokens inside parentheses
                    kname = getattr(tokens[j].kind, 'name', None)
                    if kname == 'IDENTIFIER':
                        content_idents.append(s)
                j += 1
            for ident in content_idents:
                args.add(ident)
            i = j
        else:
            i += 1

    return args


def _rewrite_token_repl(m):
    neg = m.group(1)
    tok = m.group(2)

    # skip NULL comparisons
    if tok == NULL_NAME:
        return tok

    if neg:
        return f"IS_NULL_PTR({tok})"
    else:
        return f"!IS_NULL_PTR({tok})"


def _collect_ast_walk(n, parent, tu, src_bytes, edits):
    kind = n.kind

    # Binary operator: look for == or != with NULL
    if kind == CursorKind.BINARY_OPERATOR:
        # get tokens to detect operator
        tokens = list(tu.get_tokens(extent=n.extent))
        ops = [t.spelling for t in tokens if t.spelling in ('==', '!=')]

        if ops:
            op = ops[0]
            # check children for declref and NULL
            children = list(n.get_children())
            decl_child = None
            null_found = False
            for ch in children:
                if ch.kind == CursorKind.DECL_REF_EXPR:
                    ref = getattr(ch, 'referenced', None)
                    if ref and is_pointer_decl(ref):
                        decl_child = ch
                # cheap NULL detection in child's text
                if 'NULL' in node_text(src_bytes, ch):
                    null_found = True
            if decl_child and null_found:
                var_text = node_text(src_bytes, decl_child).strip()
                if op == '==':
                    new = f'IS_NULL_PTR({var_text})'
                else:
                    new = f'!IS_NULL_PTR({var_text})'
                start = n.extent.start.offset
                end = n.extent.end.offset
                edits.append((start, end, new.encode('utf-8'), node_text(src_bytes, n), new))
                return  # don't descend into replaced node

    # Unary operator: handle leading '!' applied to bare DeclRefExpr
    if kind == CursorKind.UNARY_OPERATOR:
        tokens = list(tu.get_tokens(extent=n.extent))
        if tokens and tokens[0].spelling == '!':
            children = list(n.get_children())
            if len(children) == 1 and children[0].kind == CursorKind.DECL_REF_EXPR:
                ref = getattr(children[0], 'referenced', None)
                if ref and is_pointer_decl(ref):
                    var_text = node_text(src_bytes, children[0]).strip()
                    new = f'IS_NULL_PTR({var_text})'
                    start = n.extent.start.offset
                    end = n.extent.end.offset
                    edits.append((start, end, new.encode('utf-8'), node_text(src_bytes, n), new))
                    return

    # DeclRefExpr: bare identifier used as condition (not member, not call arg)
    if kind == CursorKind.DECL_REF_EXPR:
        ref = getattr(n, 'referenced', None)
        if ref and is_pointer_decl(ref):
            # skip if parent is call, member access, array subscript or binary op (handled above)
            if parent is not None and parent.kind in (CursorKind.CALL_EXPR, CursorKind.MEMBER_REF_EXPR, CursorKind.ARRAY_SUBSCRIPT_EXPR):
                pass
            elif parent is not None and parent.kind == CursorKind.BINARY_OPERATOR:
                # if binary operator with NULL, was handled earlier
                pass
            else:
                var_text = node_text(src_bytes, n).strip()
                new = f'!IS_NULL_PTR({var_text})'
                start = n.extent.start.offset
                end = n.extent.end.offset
                edits.append((start, end, new.encode('utf-8'), node_text(src_bytes, n), new))

    for ch in n.get_children():
        _collect_ast_walk(ch, n, tu, src_bytes, edits)


def process_if(node, src_bytes, path, edits):
    text = get_text(src_bytes, node.extent)
    # try to extract condition from first child (AST-provided extent)
    cond_text = None
    cond_node = None

    # pick a child that looks like the condition expression (prefer expression-like kinds)
    expr_kinds = (
        CursorKind.BINARY_OPERATOR,
        CursorKind.UNARY_OPERATOR,
        CursorKind.DECL_REF_EXPR,
        CursorKind.CALL_EXPR,
        CursorKind.PAREN_EXPR,
        CursorKind.COMPOUND_STMT,
    )
    for c in list(node.get_children()):
        # ensure child is fully inside the if extent
        if not (c.extent.start.offset >= node.extent.start.offset and c.extent.end.offset <= node.extent.end.offset):
            continue
        # prefer expression-like nodes
        if c.kind in expr_kinds or c.kind.is_expression():
            cond_node = c
            cond_text = node_text(src_bytes, c)
            break
    # as a safety: confirm the extracted cond_node text sits between parentheses immediately
    # following the 'if' token in the source; otherwise discard to avoid overbroad extents
    if cond_node is not None:
        # look backwards from cond_node.start to find the nearest '(' before it
        start = cond_node.extent.start.offset
        # search a small window before start
        window_start = max(node.extent.start.offset, start - 256)
        prefix = src_bytes[window_start:start].decode('utf-8', 'replace')
        idx = prefix.rfind('(')
        if idx == -1:
            # no '(' found nearby; abandon to avoid touching non-condition code
            cond_node = None
            cond_text = None
        else:
            # ensure there's a closing ')' after cond_node.end within the if extent
            end = cond_node.extent.end.offset
            window_end = min(node.extent.end.offset, end + 256)
            suffix = src_bytes[end:window_end].decode('utf-8', 'replace')
            if ')' not in suffix:
                cond_node = None
                cond_text = None

    # collect pointer variables from the condition AST (if available)
    pointer_vars = []
    if cond_node is not None:
        # fallback approach: extract identifier tokens from condition text
        cond_text = node_text(src_bytes, cond_node)
        # extract identifier tokens outside of string/char literals
        idents = []
        for m in re.finditer(r"[A-Za-z_]\w*", cond_text):
            if _pos_in_string(cond_text, m.start()):
                continue
            idents.append(m.group(0))
        skip = {NULL_NAME, 'IS_NULL_PTR', 'sizeof'}
        found = set()

        # find enclosing function for scope resolution
        func = cond_node.semantic_parent
        while func is not None and func.kind != CursorKind.FUNCTION_DECL:
            func = func.semantic_parent

        tu = node.translation_unit

        # collect declarations in function (fast) and globals (cached)
        func_decls = collect_decls(func) if func is not None else {}
        tu_key = id(tu.cursor)
        if tu_key not in _global_decls_cache:
            _global_decls_cache[tu_key] = collect_decls(tu.cursor)
        global_decls = _global_decls_cache[tu_key]

        for ident in idents:
            if ident in skip:
                continue
            decl = func_decls.get(ident)
            if decl is None:
                decl = global_decls.get(ident)
            if decl is not None and is_pointer_decl(decl):
                found.add(ident)

        # collect identifiers that are passed as arguments to any CALL_EXPR inside the condition (AST)
        call_arg_names = set()
        collect_call_args(cond_node, call_arg_names)

        # also collect identifiers appearing inside call-like tokens (macros / unexpanded calls)
        token_args = collect_call_args_tokens(cond_node)
        call_arg_names.update(token_args)

        # filter out idents that appear as call arguments (AST or token-level)
        pointer_vars = sorted(x for x in found if x not in call_arg_names)

        # conservative textual transform for now (AST branch is available but may miss cases)
        if pointer_vars:
            new_cond = transform_condition(cond_text, pointer_vars)
            if new_cond != cond_text:
                start = cond_node.extent.start.offset
                end = cond_node.extent.end.offset
                edits.append((start, end, new_cond.encode('utf-8'), cond_text, new_cond))

    if(pointer_vars):
        print("[IF]", path, "\n", text)
        print("[IFCOND]", path, "\n", cond_text)
        print("[IFVARS]", path, "\n", pointer_vars)


def process_file(path):
    index = Index.create()
    tu = index.parse(str(path), args=["-x", "c", "-std=c11"])

    # read bytes because libclang extents are byte offsets
    src_bytes = path.read_bytes()
    src_text = src_bytes.decode("utf-8", "replace")
    edits = []
    visit(tu.cursor, src_bytes, path, edits)

    if edits:
        # apply edits in descending order of start offset
        edits.sort(key=lambda e: e[0], reverse=True)
        new_src = src_bytes
        for (start, end, new_bytes, old_text, new_text) in edits:
            before = new_src[:start]
            after = new_src[end:]
            new_src = before + new_bytes + after
            print("[EDIT]", path, "start=", start, "end=", end, "->", len(new_bytes), "bytes", "\n-", old_text, "\n+", new_text)
        path.write_bytes(new_src)
        print("[MODIFIED]", path)


def visit(node, src_bytes, path, edits):
    # If this node is an if-statement, process it
    if node.kind == CursorKind.IF_STMT:
        process_if(node, src_bytes, path, edits)

    # Recurse into all children
    for child in node.get_children():
        visit(child, src_bytes, path, edits)

 
# -------------------------
# folder runner
# -------------------------

def process_folder(folder):
    for p in Path(folder).rglob("*"):
        if p.suffix in EXTS:
            process_file(p)

# -------------------------
# main
# -------------------------

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: script.py <folder>")
        sys.exit(1)

    process_folder(sys.argv[1])