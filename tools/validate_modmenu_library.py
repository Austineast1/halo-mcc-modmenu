"""Checks every command in HCMInternal/ModMenuLibrary.xml against the official HaloScript
function and global lists for each game (the hs_doc.txt files that ship with the MCC editing kits).

  python tools/validate_modmenu_library.py path/to/hs_docs

hs_docs is a folder with h1/hs_doc_standalone_retail.txt, h2/hs_doc.txt, h3/..., h3odst/..., hr/..., h4/...
(the c20 wiki keeps copies at github.com/Sigmmma/c20 under src/data/hs_docs).

For each mod and each game it supports, it expands {player} and friends exactly like the menu does,
parses the HaloScript, and reports any function or global the game doesn't have, wrong argument counts,
and `set` on a non-boolean global with {bool}. Exit code 1 if anything is wrong.
"""
import os, re, sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "HCMInternal", "ModMenuLibrary.xml")
GAMES = ["Halo1", "Halo2", "Halo3", "Halo3ODST", "HaloReach", "Halo4"]
DOCS = {"Halo1": "h1/hs_doc_standalone_retail.txt", "Halo2": "h2/hs_doc.txt", "Halo3": "h3/hs_doc.txt",
        "Halo3ODST": "h3odst/hs_doc.txt", "HaloReach": "hr/hs_doc.txt", "Halo4": "h4/hs_doc.txt"}
# Campaign teams each engine accepts. Reach's list comes straight from its own compiler error;
# the others are the scenario team enums of each engine.
TEAMS = {
    "Halo1": {"default", "player", "human", "covenant", "flood", "sentinel"},
    "Halo2": {"default", "player", "human", "covenant", "flood", "sentinel", "heretic", "prophet"},
    "Halo3": {"default", "player", "human", "covenant", "flood", "sentinel", "heretic", "prophet", "guilty"},
    "Halo3ODST": {"default", "player", "human", "covenant", "flood", "sentinel", "heretic", "prophet", "guilty"},
    "HaloReach": {"default", "player", "human", "covenant", "brute", "mule", "spare", "covenant_player"},
    "Halo4": {"default", "player", "human", "covenant"},
}
ALL_TEAMS = set().union(*TEAMS.values())
DIFFICULTIES = {"easy", "normal", "hard", "impossible"}
ENUM_VALUES = {g: t | DIFFICULTIES for g, t in TEAMS.items()}
SPECIAL_FORMS = {"begin", "begin_random", "if", "cond", "set", "and", "or", "+", "-", "*", "/", "=", "!=", ">", "<", ">=", "<=",
                 "min", "max", "sleep", "sleep_until", "wake", "inspect", "unit", "not", "print", "list_get", "players", "player_get"}


def load_docs(folder):
    out = {}
    for game, rel in DOCS.items():
        funcs, globs, section = {}, {}, "f"
        for line in open(os.path.join(folder, rel), encoding="utf-8", errors="replace"):
            if "AVAILABLE EXTERNAL GLOBALS" in line: section = "g"; continue
            if "AVAILABLE FUNCTIONS" in line: section = "f"; continue
            m = re.match(r"^\(<([\w ]+)>\s+([^\s<>()\[\]]+)\s*(.*)\)\s*$", line.strip())
            if not m: continue
            if section == "f":
                rest = m.group(3)
                variadic = "(s)>" in rest or "[" in rest
                args = re.findall(r"<([^>]+)>", rest)
                funcs.setdefault(m.group(2), []).append((m.group(1), args, variadic))
            else:
                globs[m.group(2)] = m.group(1)
        out[game] = (funcs, globs)
    return out


def parse_games(spec):
    games = set()
    for part in [p.strip() for p in spec.split(",") if p.strip()]:
        if part == "all": games |= set(GAMES)
        elif part == "gen2up": games |= set(GAMES[1:])
        elif part == "gen3": games |= {"Halo3", "Halo3ODST", "HaloReach", "Halo4"}
        elif part in GAMES: games.add(part)
        else: raise ValueError(f"unknown game '{part}'")
    return games


def expand(line, game, value):
    player = "(unit (list_get (players) 0))" if game in ("Halo1", "Halo2") else "(player_get 0)"
    return line.replace("{player}", player).replace("{players}", "(players)").replace("{value}", value).replace("{bool}", value)


def tokenize(s):
    return re.findall(r'\(|\)|"[^"]*"|[^\s()]+', s)


def parse(tokens):
    stack, cur = [], []
    for t in tokens:
        if t == "(":
            stack.append(cur); cur = []
        elif t == ")":
            if not stack: raise SyntaxError("unbalanced )")
            done = cur; cur = stack.pop(); cur.append(done)
        else:
            cur.append(t)
    if stack: raise SyntaxError("unbalanced (")
    return cur


def check_expr(expr, game, docs, problems, where):
    funcs, globs = docs[game]
    if not isinstance(expr, list) or not expr: return
    head = expr[0]
    if isinstance(head, list):
        problems.append(f"{where}: expression starts with a list"); return
    args = expr[1:]
    if head == "set":
        if len(args) != 2: problems.append(f"{where}: set needs a global and a value")
        elif args[0] not in globs: problems.append(f"{where}: {game} has no global '{args[0]}'")
        elif args[1] in ("true", "false") and globs[args[0]] != "boolean":
            problems.append(f"{where}: global '{args[0]}' is {globs[args[0]]}, not boolean")
    elif head in SPECIAL_FORMS:
        pass
    elif head not in funcs:
        problems.append(f"{where}: {game} has no function '{head}'")
    else:
        ok = False
        for ret, sig, variadic in funcs[head]:
            optional = 0
            if len(args) == len(sig) or (variadic and len(args) >= max(0, len(sig) - 1)): ok = True; break
        if not ok:
            sigs = " | ".join(" ".join(f"<{a}>" for a in s) or "(no args)" for _, s, _ in funcs[head])
            problems.append(f"{where}: {head} takes {sigs}, got {len(args)} args")
    for a in args:
        if isinstance(a, list): check_expr(a, game, docs, problems, where)
        elif re.match(r"^[a-z_][a-z0-9_]*$", a) and a not in ("true", "false", "none") and a not in ENUM_VALUES.get(game, set()):
            if a in globs: continue
            if a in funcs: continue
            if a in ALL_TEAMS: problems.append(f"{where}: '{a}' is not a team in {game}"); continue
            problems.append(f"{where}: '{a}' isn't a known value in {game}")


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    docs = load_docs(sys.argv[1])
    tree = ET.parse(LIB)
    problems, checked, mods, ids = [], 0, 0, set()
    for cat in tree.getroot().iter("Category"):
        for mod in cat.iter("Mod"):
            mods += 1
            mid, mtype = mod.get("id"), mod.get("type")
            if mid in ids: problems.append(f"duplicate mod id '{mid}'")
            ids.add(mid)
            if mtype in ("hcm-toggle", "hcm-slider", "hcm-action", "skull"): continue
            blocks = mod.findall("Game")
            if not blocks: problems.append(f"{mid}: no <Game> block")
            options = mod.findall("Option")
            for block in blocks:
                for game in sorted(parse_games(block.get("games", ""))):
                    values = ["true", "false"] if mtype == "toggle" else ["1.5"]
                    if mtype == "choice":
                        values = [o.get("value") for o in options if game in parse_games(o.get("games", "all"))] or ["x"]
                    if mtype == "text": continue  # free text
                    for child in block:
                        for raw in child.text.splitlines() if child.text else []:
                            raw = raw.strip()
                            if not raw or raw.startswith(";"): continue
                            for v in values:
                                line = expand(raw, game, v)
                                where = f"{cat.get('id')}/{mid} [{game}] {child.tag}"
                                try:
                                    for expr in parse(tokenize(line)):
                                        check_expr(expr, game, docs, problems, where)
                                    checked += 1
                                except SyntaxError as e:
                                    problems.append(f"{where}: {e}")
    seen, unique = set(), []
    for p in problems:
        if p not in seen: seen.add(p); unique.append(p)
    print(f"{mods} mods, {checked} command lines checked, {len(unique)} problems")
    for p in unique: print("  " + p)
    sys.exit(1 if unique else 0)


if __name__ == "__main__":
    main()
