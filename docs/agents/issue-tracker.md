# Issue tracker: Local Markdown

Issues and specs for this repo live as markdown files in `.scratch/`. Wayfinding maps live in `.wayfinder/`.

## Conventions

- One feature per directory: `.scratch/<feature-slug>/`
- The spec is `.scratch/<feature-slug>/spec.md`
- Implementation issues are one file per ticket at `.scratch/<feature-slug>/issues/<NN>-<slug>.md`, numbered from `01` — never a single combined tickets file
- Triage state is recorded as a `Status:` line near the top of each issue file (see `triage-labels.md` for the role strings)
- Comments and conversation history append to the bottom of the file under a `## Comments` heading

## When a skill says "publish to the issue tracker"

Create a new file under `.scratch/<feature-slug>/` (creating the directory if needed).

## When a skill says "fetch the relevant ticket"

Read the file at the referenced path. The user will normally pass the path or the issue number directly.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a file with one **child** file per ticket.

- **Map**: `.wayfinder/<effort>/map.md` — frontmatter `label: wayfinder:map` and `title`, then the Destination / Notes /
  Decisions so far / Not yet specified / Out of scope body. The effort's glossary is `.wayfinder/<effort>/CONTEXT.md`.
- **Child ticket**: `.wayfinder/<effort>/tickets/NN-<slug>.md`, numbered from `01`, with frontmatter `title`, `type`
  (`research`/`prototype`/`grilling`/`task`), `status` (`open`/`closed`), `assignee` and `blocked_by: [NN, ...]`, and
  the question under `## Question`.
- **Assets**: research findings in `.wayfinder/<effort>/research/NN-<slug>.md`, prototypes in `prototypes/`, other
  artifacts in `assets/NN-<slug>/`; linked from the ticket, not pasted in.
- **Blocking**: the `blocked_by` list. A ticket is unblocked when every ticket it lists has `status: closed`.
- **Frontier**: scan `.wayfinder/<effort>/tickets/` for files that are `status: open`, unblocked, and have an empty
  `assignee`; first by number wins.
- **Claim**: set `assignee` to the dev driving the map and save before any work.
- **Resolve**: append the answer under a `## Resolution` heading, set `status: closed`, then append a context pointer
  (gist + link) to the map's Decisions so far.
- **Out of scope**: set `status: closed` without a resolution on the route, and add one line to the map's Out of scope.
