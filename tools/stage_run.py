"""Stage one console test in a single command: SD files, kept ELF, hash manifest and the test request.

  python tools/stage_run.py log132 [--script console_gungan.txt] [--read notes.md] [--play "..."]

Steps, each checked:
  1. The build's NACP version (build dir's lswtcs.nacp, display version) must end with the label given, so an old NRO
     is never staged by mistake; the NRO must be newer than the ELF it was packed from.
  2. The ELF is kept as private/lswtcs-<label>.elf (an existing different file is refused unless --force).
  3. SD tree (private/sdcard by default): switch/lswtcs/lswtcs.nro, lswtcs/env.txt from the profile
     (profiles/hardware-ab.env; --script adds LSWTCS_INPUTSCRIPT=<name> at its end), and tests/scripts/*.txt copied to
     lswtcs/tests/ so every route is on the card.
  4. Every staged file is hashed again and compared with its source; private/sdcard-manifest-<label>.json lists them
     (workspace-tools/verify_tcs360_sd.py --sd-root <card> --manifest <json> checks a copied card).
  5. private/PEDIDO_TESTE_<label>.md: build, hashes, what to copy (only files that changed since the last staging),
     how to play, and what will be read (--read: a Markdown file with the run's own list).
Nothing is sent anywhere; the card copy and the play stay with the tester.
"""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PLAY = "Uma sessão, portátil, rota de sempre (hub, Gungan A e B, volta ao hub). Fechar pelo HOME."


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def nacp_version(nacp):
    data = nacp.read_bytes()
    return data[0x3060:0x3070].split(b"\0", 1)[0].decode("ascii", "replace")


def copy_checked(src, dst, data=None):
    """Copy (or write data) only when the bytes differ; returns True if the file changed."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    new = data if data is not None else None
    if dst.is_file():
        if new is not None:
            if dst.read_bytes() == new:
                return False
        elif dst.stat().st_size == src.stat().st_size and sha256(dst) == sha256(src):
            return False
    if new is not None:
        dst.write_bytes(new)
    else:
        shutil.copyfile(src, dst)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("label", help="build label, e.g. log132 (the NACP version must end with it)")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "source/switch/build-nvpgo")
    ap.add_argument("--profile", type=Path, default=ROOT / "profiles/hardware-ab.env")
    ap.add_argument("--sd", type=Path, default=ROOT / "private/sdcard")
    ap.add_argument("--script", help="route in tests/scripts/ to replay on the console (sets LSWTCS_INPUTSCRIPT)")
    ap.add_argument("--play", help="how to play (default: the usual handheld route)")
    ap.add_argument("--read", type=Path, help="Markdown file with what the run's logs will be read for")
    ap.add_argument("--force", action="store_true", help="replace a different private/lswtcs-<label>.elf")
    ap.add_argument("--out", type=Path, default=ROOT / "private",
                    help="where the kept ELF, manifest and request go (default private/; another folder for a trial)")
    args = ap.parse_args()

    label = args.label
    nro, elf, nacp = (args.build_dir / n for n in ("lswtcs.nro", "lswtcs.elf", "lswtcs.nacp"))
    for p in (nro, elf, nacp, args.profile):
        if not p.is_file():
            sys.exit(f"missing: {p}")
    version = nacp_version(nacp)
    if not version.endswith(label):
        sys.exit(f"NACP version is {version!r}, not ...{label}: build the right version first")
    if nro.stat().st_mtime < elf.stat().st_mtime:
        sys.exit("the NRO is older than the ELF: the last build did not finish packing")
    scripts = sorted((ROOT / "tests/scripts").glob("*.txt"))
    if args.script and args.script not in {s.name for s in scripts}:
        sys.exit(f"no route tests/scripts/{args.script}")

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    kept = out / f"lswtcs-{label}.elf"
    if kept.is_file() and sha256(kept) != sha256(elf) and not args.force:
        sys.exit(f"{kept} exists with other bytes (use --force to replace it)")
    copy_checked(elf, kept)

    env = args.profile.read_bytes()
    if args.script:
        nl = b"\r\n" if b"\r\n" in env else b"\n"
        env = env.rstrip(b"\r\n") + nl + b"# stage_run.py: scripted console route" + nl + \
            f"LSWTCS_INPUTSCRIPT={args.script}".encode() + nl
    plan = [(nro, "switch/lswtcs/lswtcs.nro", None), (args.profile, "lswtcs/env.txt", env)]
    plan += [(s, f"lswtcs/tests/{s.name}", None) for s in scripts]
    changed, manifest = [], []
    for src, rel, data in plan:
        dst = args.sd / rel
        if copy_checked(src, dst, data):
            changed.append(rel)
        want = hashlib.sha256(data).hexdigest() if data is not None else sha256(src)
        got = sha256(dst)
        if got != want:
            sys.exit(f"staged {rel} does not match its source")
        manifest.append({"path": rel, "bytes": dst.stat().st_size, "sha256": got})
    man_path = out / f"sdcard-manifest-{label}.json"
    man_path.write_text(json.dumps({"label": label, "version": version, "files": manifest}, indent=2) + "\n",
                        encoding="utf-8")

    nro_hash, elf_hash = sha256(nro), sha256(elf)
    copy_lines = [f"- `private/sdcard/{rel}` -> `sdmc:/{rel}`" for rel in changed if not rel.startswith("lswtcs/tests/")]
    tests_changed = [rel for rel in changed if rel.startswith("lswtcs/tests/")]
    if tests_changed:
        copy_lines.append("- `private/sdcard/lswtcs/tests/` -> `sdmc:/lswtcs/tests/` (roteiros novos ou mudados: "
                          + ", ".join(Path(r).name for r in tests_changed) + ")")
    if not copy_lines:
        copy_lines = ["- Nada mudou desde a última preparação: o cartão já está certo."]
    if args.play:
        play = args.play
    elif args.script:
        play = (f"Uma sessão, portátil. Abra o jogo e **não toque no controle**: o roteiro `{args.script}` joga a rota "
                "sozinho (hub, Gungan A e B, volta ao hub). Se ele sair do caminho ou travar num menu, assuma o "
                "controle, termine a rota e avise em que ponto foi. Fechar pelo HOME no fim.")
    else:
        play = DEFAULT_PLAY
    read = args.read.read_text(encoding="utf-8").strip() if args.read else "- (preencher)"
    request = "\n".join([
        f"# Pedido de teste: {label} (uma sessão só)", "",
        f"**Build:** `{version}`, NRO `{nro_hash[:8]}…`. ELF: `private/lswtcs-{label}.elf` (`{elf_hash[:8]}…`).", "",
        "## Copiar para o cartão", *copy_lines, "", "Não apague nada do cartão.",
        f"Conferir depois de copiar (opcional): `python workspace-tools/verify_tcs360_sd.py --sd-root <cartão> "
        f"--manifest private/sdcard-manifest-{label}.json` (confere NRO, env.txt e roteiros desta preparação).", "",
        "## Jogar", play, "", "## O que eu vou ler", read, ""])
    req_path = out / f"PEDIDO_TESTE_{label}.md"
    req_path.write_text(request, encoding="utf-8")
    print(json.dumps({"version": version, "nro": nro_hash[:16], "elf_kept": str(kept),
                      "changed_on_card": changed, "manifest": str(man_path),
                      "request": str(req_path)}, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
