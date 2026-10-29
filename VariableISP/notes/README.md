# VariableISP articles — layout and provenance

Three articles live in the folder root (TeX source + PDF):

- `KeplerVariableISPRockets` — time-optimal variable-Isp transfers (corrected 2026-10-29).
- `RocketPursuitEvasion` — pursuit, rendezvous and evasion (corrected; formerly `RocketsChaseAstra.tex`).
- `MinimumEffortPlanning` — the unified minimum-effort planning manuscript (new; was `unified_paper.tex` /
  `minimum_effort_revised.pdf`). Its sections are in `minimum_effort_sections/`.

The corrections and the new manuscript were computed by OpenAI Astra in October 2026 and delivered as
`variable_isp_project_revised_archive(1).zip`. Only the parts below were kept; the archive was deleted.

## Build

From the `VariableISP/` folder (IEEEtran is used when installed, otherwise an A4 article fallback):

```sh
for a in KeplerVariableISPRockets RocketPursuitEvasion MinimumEffortPlanning; do
  pdflatex $a && bibtex $a && pdflatex $a && pdflatex $a
done
```

## Folders

- `figures/`, `bib/` — figures and bibliographies of all three articles.
- `notes/` — Astra's point-by-point responses to our reviews (`articles_response_to_review.md`,
  `minimum_effort_revision_response.md`), the unified implementation card and source provenance.
- `verification/kepler_chase/` — numerical audit of the two corrected articles (73 reconstructed Jupiter rows,
  per-cell table ledger `table_diagnostics.csv`, chase checks, capped free-space fixture).
  `build_audit_ledger.py` reads `original_KeplerVariableISPRockets.tex`; recreate it with
  `git show b24ca9b:VariableISP/KeplerVariableISPRockets.tex > original_KeplerVariableISPRockets.tex`.
- `verification/minimum_effort/` — checks for the new manuscript: exact Earth–Mars 70-day witness
  (`certify_mars_witness.py`, stdlib interval arithmetic), worked solar case, heliocentric audits.
  The heliocentric trajectory dumps (`heliocentric_*.npz`, 10 MB) were dropped; `audit_heliocentric.py`
  and `audit_long_shoot.py` regenerate them from `inputs/atlas_seeds.npz`.
- `reference_code/block*/` — C++ headers, tests, fixtures, Python references and implementation cards of
  the research blocks (CW rendezvous, timing sensitivities, exhaust band, perihelion constraint,
  optimality bounds, escape/capture, flybys). Where a card disagrees with `MinimumEffortPlanning`, the
  manuscript wins. `reference_code/integration/` runs all blocks (`run_regression.py`) and cross-checks
  them (`integration_checks.py`); both need NumPy, SciPy and g++.

## Not kept

Original articles (in git history), source snapshots at the pinned solver commit
`bdc9698` of github.com/irigi/VariableISPRocketTrajectories, per-block papers and READMEs, the
`block7_synthesis` first draft (superseded by `MinimumEffortPlanning`), regression logs and checksums.
