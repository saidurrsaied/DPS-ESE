# pyCPA model + run (optional)

This folder contains a small pyCPA model so you can compute WCRT on WSL.

## Install prerequisites (WSL/Ubuntu)

If you see errors like “No module named pip” or “ensurepip not available”, install these first:

```bash
sudo apt update
sudo apt install -y python3-pip python3-venv
```

Then install pyCPA:

```bash
cd /home/saidur/projects/tpgit/DPS-ESE/DPS/simulation
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip setuptools wheel
# pyCPA is not on PyPI; install from GitHub. Using --no-deps avoids trying
# to install the legacy 'argparse' backport (argparse is built into Python 3).
pip install --no-deps -r docs/schedulability/requirements.txt
```

## Run the model

Leader (single-core):

```bash
python3 docs/schedulability/pycpa_model.py --system leader
```

Follower (single-core):

```bash
python3 docs/schedulability/pycpa_model.py --system follower
```

## What’s modeled

- Uses the analysis WCET bounds from docs/schedulability/wcet_bounds.md.
- Uses simple **periodic event models** (P=period, jitter=0).
- Models “rare” paths (emergency/timeout) conservatively with shorter periods.

This is intentionally conservative and meant as a defensible project deliverable, not a perfect micro-architecture model of the event queue.

## If pyCPA API differs

pyCPA has had small API changes across versions. The script is defensive, but if you hit attribute/type errors, tell me the traceback and I’ll adapt the script to your installed pyCPA version.
